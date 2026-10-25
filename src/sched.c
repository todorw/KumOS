#include "sched.h"
#include "timer.h"
#include "vga.h"
#include "kstring.h"
#include "kmalloc.h"
#include "gdt.h"
#include "paging.h"
#include "keyboard.h"
#include "signal.h"
#include <stdint.h>

/* Switch CR3 to whatever address space the about-to-run task expects.
   Tasks that never forked/execve'd have page_dir_phys==0 and use the
   shared root directory. Without this, a task with its own cloned
   directory (fork/execve) that yields would leave CR3 pointing at
   whichever address space last ran when a *different* task resumes -
   every virtual address above 1GB (user stack/code) then resolves
   through the wrong page tables. This was never hit before fork() got
   fixed to actually work, since nothing previously coexisted via
   cooperative yield with a genuinely different page directory live. */
static inline void sched_switch_dir(task_t *next) {
    paging_switch(next->page_dir_phys ? next->page_dir_phys : paging_root_dir());
}

static task_t   tasks[SCHED_MAX_TASKS];
static int      task_count  = 0;
static int      current_idx = 0;
static int      next_pid    = 1;
static uint32_t tick_accum  = 0;

extern void switch_context(uint32_t *old_esp, uint32_t new_esp);

static void task_setup_stack(task_t *t, void (*entry)(void)) {

    uint32_t *sp = (uint32_t *)((uint8_t *)t->stack + t->stack_size);

    /* switch_context resumes a task via a bare `ret` after popping
       edi,esi,ebx,ebp,eflags (no iret) — these are ring-0 kernel tasks, no
       privilege transition needed, so the resume target is just `entry`.
       eflags=0x202 (IF set) since a freshly-started kernel task should run
       with interrupts enabled, same as everything else that isn't parked
       mid-epilogue. */
    *--sp = (uint32_t)entry;
    *--sp = 0x202;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;

    t->esp = (uint32_t)sp;
}

static void idle_task(void) {
    while (1) {
        __asm__ volatile ("hlt");
    }
}

void sched_init(void) {
    kmemset(tasks, 0, sizeof(tasks));
    task_count  = 0;
    current_idx = 0;
    tick_accum  = 0;

    tasks[0].pid        = next_pid++;
    tasks[0].state      = TASK_RUNNING;
    tasks[0].stack      = 0;
    tasks[0].kum_level  = 1;
    tasks[0].stack_size = 0;
    tasks[0].parent_pid = 0;
    kstrcpy(tasks[0].name, "kshell");
    task_count = 1;

    uint32_t *idle_stack = kmalloc(SCHED_STACK_SIZE);
    if (idle_stack) {
        tasks[1].pid        = next_pid++;
        tasks[1].state      = TASK_READY;
        tasks[1].stack      = idle_stack;
        tasks[1].stack_size = SCHED_STACK_SIZE;
        tasks[1].kum_level  = 1;
        tasks[1].parent_pid = 0;
        kstrcpy(tasks[1].name, "idle");
        task_setup_stack(&tasks[1], idle_task);
        task_count = 2;
    }
}

int sched_spawn(const char *name, void (*entry)(void), int kum_level) {
    if (task_count >= SCHED_MAX_TASKS) return -1;

    uint32_t *stack = kmalloc(SCHED_STACK_SIZE);
    if (!stack) return -1;

    int slot = -1;
    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        if (tasks[i].state == TASK_DEAD || tasks[i].state == TASK_ZOMBIE
            || tasks[i].pid == 0) {
            slot = i; break;
        }
    }
    if (slot < 0) slot = task_count;
    if (slot >= SCHED_MAX_TASKS) { kfree(stack); return -1; }

    tasks[slot].pid        = next_pid++;
    tasks[slot].state      = TASK_READY;
    tasks[slot].stack      = stack;
    tasks[slot].stack_size = SCHED_STACK_SIZE;
    tasks[slot].ticks      = 0;
    tasks[slot].exit_code  = 0;
    tasks[slot].kum_level  = kum_level;
    tasks[slot].parent_pid = tasks[current_idx].pid;
    tasks[slot].tty        = tasks[current_idx].tty;
    tasks[slot].page_dir_phys = 0;
    kstrcpy(tasks[slot].name, name);
    task_setup_stack(&tasks[slot], entry);

    if (slot >= task_count) task_count = slot + 1;
    return tasks[slot].pid;
}

void sched_exit(void) {
    sched_exit_code(0);
}

void sched_exit_code(int code) {
    __asm__ volatile ("cli");
    tasks[current_idx].exit_code = code;

    /* Every fork()'d/execve'd task owns a private page directory
       (page_dir_phys) cloned by paging_clone_dir() - this was never freed
       on exit, only the kernel stack was, so every process that forked
       and exited leaked its entire address space (page directory + every
       privately-owned page table + page). A few fork() cycles in a row
       exhausted physical memory and crashed with a real page fault
       (NOTPRES, CR2 nowhere near anything the failing code touched
       directly - just pmm_alloc() silently running out and demand-paging
       leaving the page unmapped). Must switch off this directory before
       freeing it - it's still the live CR3 at this point. */
    if (tasks[current_idx].page_dir_phys) {
        paging_switch(paging_root_dir());
        paging_free_user(tasks[current_idx].page_dir_phys);
        tasks[current_idx].page_dir_phys = 0;
    }

    int has_parent = 0;
    for (int i = 0; i < task_count; i++) {
        if (tasks[i].pid == tasks[current_idx].parent_pid
            && tasks[i].state != TASK_DEAD
            && tasks[i].state != TASK_ZOMBIE) {
            has_parent = 1; break;
        }
    }
    if (has_parent) {
        tasks[current_idx].state = TASK_ZOMBIE;

        if (tasks[current_idx].stack) {
            kfree(tasks[current_idx].stack);
            tasks[current_idx].stack = 0;
        }

        /* Nothing ever actually sent SIGCHLD before - sched_waitpid() and
           friends busy-poll task state directly so nothing depended on it,
           but that also meant a handler installed via signal()/SIGCHLD had
           no way to ever fire. Real Unix behavior: notify the parent every
           time a child becomes reapable. */
        signal_send(tasks[current_idx].parent_pid, SIGCHLD);
    } else {
        tasks[current_idx].state = TASK_DEAD;
        if (tasks[current_idx].stack) {
            kfree(tasks[current_idx].stack);
            tasks[current_idx].stack = 0;
        }
    }
    __asm__ volatile ("sti");
    sched_yield();
}

void sched_sleep(uint32_t ms) {
    __asm__ volatile ("cli");
    tasks[current_idx].state       = TASK_SLEEPING;
    tasks[current_idx].sleep_until = timer_ticks() + (ms * 100 / 1000);
    __asm__ volatile ("sti");
    sched_yield();
}

static int pick_next(void) {
    int start = (current_idx + 1) % task_count;
    int i = start;
    do {

        if (tasks[i].state == TASK_SLEEPING &&
            timer_ticks() >= tasks[i].sleep_until) {
            tasks[i].state = TASK_READY;
        }
        if (tasks[i].state == TASK_READY || tasks[i].state == TASK_RUNNING)
            return i;
        i = (i + 1) % task_count;
    } while (i != start);
    return 0;
}

void sched_yield(void) {
    __asm__ volatile ("cli");
    int next = pick_next();
    if (next == current_idx) { __asm__ volatile ("sti"); return; }

    int prev = current_idx;
    if (tasks[prev].state == TASK_RUNNING)
        tasks[prev].state = TASK_READY;
    tasks[next].state = TASK_RUNNING;
    current_idx = next;

    tss_set_kernel_stack((uint32_t)tasks[next].stack + tasks[next].stack_size);
    sched_switch_dir(&tasks[next]);

    uint32_t *old_esp_ptr = &tasks[prev].esp;
    uint32_t  new_esp     =  tasks[next].esp;

    /* Was a blanket "sti" here before switch_context() - real bug, since
       current_idx is already updated to `next` above but the stack swap
       hasn't happened yet. switch_context() now saves/restores EFLAGS
       itself via pushfd/popfd (boot/sched_switch.asm) instead of a blanket
       sti, so each task resumes with its own last-saved interrupt state.
       This fixed a real reentrancy corruption (confirmed via serial
       tracing: a nested timer tick landing in the old sti-then-switch gap
       corrupted the register frame of whichever task was mid-switch), but
       kush's do_exec() still can't safely use real fork()+exec()+wait() -
       a second, deeper bug remains in the scheduler's handling of a task
       that yields many times in a loop (sched_waitpid()'s busy-wait) then
       finally returns to ring 3: the syscall's own saved iret frame reads
       back correct right up to the last C statement before isr128's
       fixed pop/iret epilogue (verified with -O0 on the whole call chain,
       ruling out an optimizer bug), yet the actual iret lands in garbage
       stack-address-looking values. Root cause not found this round -
       see feedback-kumos-commit-style / project-kumos-round14 memory.
       do_exec() is back to the safe self-replacing sys_exec() rather than
       ship a reproducible crash. */
    switch_context(old_esp_ptr, new_esp);
}

void sched_tick(registers_t *r) {
    (void)r;
    tasks[current_idx].ticks++;
    tick_accum++;

    for (int i = 0; i < task_count; i++) {
        if (tasks[i].state == TASK_SLEEPING &&
            timer_ticks() >= tasks[i].sleep_until) {
            tasks[i].state = TASK_READY;
        }
    }

    if (tick_accum >= SCHED_QUANTUM) {
        tick_accum = 0;
        int next = pick_next();
        if (next != current_idx) {
            int prev = current_idx;
            if (tasks[prev].state == TASK_RUNNING)
                tasks[prev].state = TASK_READY;
            tasks[next].state = TASK_RUNNING;
            current_idx = next;
            tss_set_kernel_stack(
                (uint32_t)tasks[next].stack + tasks[next].stack_size);
            sched_switch_dir(&tasks[next]);

            uint32_t *old_ptr = &tasks[prev].esp;
            uint32_t  new_esp =  tasks[next].esp;
            switch_context(old_ptr, new_esp);
        }
    }
}

task_t *sched_current(void) {
    return &tasks[current_idx];
}

task_t *sched_get_task(int pid) {
    for (int i = 0; i < task_count; i++)
        if (tasks[i].pid == pid && tasks[i].state != TASK_DEAD)
            return &tasks[i];
    return 0;
}

static const char *tstate(task_state_t s) {
    switch(s) {
        case TASK_RUNNING:  return "RUN  ";
        case TASK_READY:    return "READY";
        case TASK_SLEEPING: return "SLEEP";
        case TASK_DEAD:     return "DEAD ";
        case TASK_ZOMBIE:   return "ZOMBI";
        case TASK_STOPPED:  return "STOP ";
        default:            return "?    ";
    }
}

int sched_task_count(void) { return task_count; }

task_t *sched_task_at(int index) {
    if (index < 0 || index >= task_count) return 0;
    return &tasks[index];
}

int sched_current_index(void) { return current_idx; }

void sched_list(void) {
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    vga_puts("  PID  STATE  TICKS   KUM  NAME\n");
    vga_puts("  ---  -----  -----   ---  ----\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    for (int i = 0; i < task_count; i++) {
        if (tasks[i].state == TASK_DEAD && tasks[i].pid == 0) continue;
        vga_puts("  ");
        vga_put_dec(tasks[i].pid);
        vga_puts("    ");
        vga_puts(tstate(tasks[i].state));
        vga_puts("  ");
        vga_put_dec(tasks[i].ticks);
        vga_puts("    ");
        vga_puts(tasks[i].kum_level ? "yes" : "no ");
        vga_puts("  ");
        if (i == current_idx) {
            vga_set_color(VGA_GREEN, VGA_BLACK);
            vga_puts(tasks[i].name);
            vga_puts(" *");
            vga_set_color(VGA_WHITE, VGA_BLACK);
        } else {
            vga_puts(tasks[i].name);
        }
        vga_putchar('\n');
    }
}

int sched_waitpid(int pid) {
    while (1) {
        for (int i = 0; i < task_count; i++) {
            if (tasks[i].pid == pid) {
                if (tasks[i].state == TASK_ZOMBIE || tasks[i].state == TASK_DEAD) {
                    int code = tasks[i].exit_code;
                    tasks[i].state = TASK_DEAD;
                    return code;
                }
                break;
            }
        }

        int found = 0;
        for (int i = 0; i < task_count; i++)
            if (tasks[i].pid == pid) { found = 1; break; }
        if (!found) return 0;

        if (keyboard_check_ctrlc()) signal_send(pid, SIGINT);

        sched_yield();
    }
}

/* Like sched_waitpid() but also polls for Ctrl+Z (job-control stop) instead
   of only Ctrl+C, and stops blocking (without reaping) if the child gets
   stopped rather than just when it exits. Returns 1 if the child exited
   (exit_code filled in, task reaped to TASK_DEAD - same as sched_waitpid()),
   2 if the child is now TASK_STOPPED (job control - caller keeps the pid
   around for a later fg/bg, nothing reaped), 0 if no such pid exists. */
int sched_waitstatus(int pid, int *exit_code) {
    while (1) {
        int found = 0;
        for (int i = 0; i < task_count; i++) {
            if (tasks[i].pid != pid) continue;
            found = 1;
            if (tasks[i].state == TASK_ZOMBIE || tasks[i].state == TASK_DEAD) {
                if (exit_code) *exit_code = tasks[i].exit_code;
                tasks[i].state = TASK_DEAD;
                return 1;
            }
            if (tasks[i].state == TASK_STOPPED) return 2;
            break;
        }
        if (!found) return 0;

        if (keyboard_check_ctrlc()) signal_send(pid, SIGINT);
        /* SIGTSTP, not SIGSTOP - Ctrl+Z's stop has to be the catchable/
           ignorable one (real Unix convention); SIGSTOP stays reserved for
           an unconditional `kill -19` that can't be dodged. */
        if (keyboard_check_ctrlz()) signal_send(pid, SIGTSTP);

        sched_yield();
    }
}

/* Non-blocking job-control status for `jobs`/fg/bg to display without
   reaping: 0 = running/ready/sleeping, 1 = stopped, 2 = exited (zombie,
   not yet reaped by wait/waitpid/waitstatus), -1 = no such task. */
int sched_procstate(int pid) {
    for (int i = 0; i < task_count; i++) {
        if (tasks[i].pid != pid) continue;
        if (tasks[i].state == TASK_STOPPED) return 1;
        if (tasks[i].state == TASK_ZOMBIE || tasks[i].state == TASK_DEAD) return 2;
        return 0;
    }
    return -1;
}

int sched_wait(int *exit_code) {
    int my_pid = tasks[current_idx].pid;
    while (1) {
        for (int i = 0; i < task_count; i++) {
            if (tasks[i].parent_pid == my_pid
                && (tasks[i].state == TASK_ZOMBIE
                    || tasks[i].state == TASK_DEAD)) {
                int code = tasks[i].exit_code;
                int pid  = tasks[i].pid;
                tasks[i].state = TASK_DEAD;
                if (exit_code) *exit_code = code;
                return pid;
            }
        }

        int children = 0;
        for (int i = 0; i < task_count; i++)
            if (tasks[i].parent_pid == my_pid
                && tasks[i].state != TASK_DEAD)
                children++;
        if (!children) return -1;
        sched_yield();
    }
}