#include "signal.h"
#include "sched.h"
#include "kstring.h"
#include <stdint.h>

static sigset_t proc_signals[SCHED_MAX_TASKS];

void signal_init(void) {
    kmemset(proc_signals, 0, sizeof(proc_signals));
}

int signal_send(int pid, int sig) {
    if (sig <= 0 || sig >= NSIG) return -1;

    if (sig == SIGKILL) {
        task_t *t = sched_get_task(pid);
        if (!t) return -1;
        sched_exit_code(-1);
        return 0;
    }

    /* SIGSTOP/SIGCONT are job control, not deliverable/catchable signals -
       they act on the scheduler's task state directly instead of going
       through the pending/handler machinery below, same as real Unix (a
       process can't ignore or handle SIGSTOP). A stopped task never runs
       signal_check() again on its own (pick_next() only schedules READY/
       RUNNING tasks), so SIGCONT has to flip it back to READY here rather
       than relying on the target to notice a pending bit. */
    if (sig == SIGSTOP) {
        task_t *t = sched_get_task(pid);
        if (!t) return -1;
        if (t->state != TASK_ZOMBIE && t->state != TASK_DEAD) t->state = TASK_STOPPED;
        return 0;
    }
    if (sig == SIGCONT) {
        task_t *t = sched_get_task(pid);
        if (!t) return -1;
        if (t->state == TASK_STOPPED) t->state = TASK_READY;
        return 0;
    }

    task_t *t = sched_get_task(pid);
    if (!t) return -1;

    int idx = 0;
    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        task_t *ti = sched_get_task(i + 1);
        if (ti && ti->pid == pid) { idx = i; break; }
    }

    proc_signals[idx].pending |= (1u << sig);

    if (sig == SIGINT || sig == SIGTERM || sig == SIGKILL) {
        task_t *task = sched_get_task(pid);
        if (task) task->state = TASK_ZOMBIE;
    }
    return 0;
}

int signal_set_handler(int sig, sighandler_t handler) {
    if (sig <= 0 || sig >= NSIG) return -1;
    task_t *cur = sched_current();
    int idx = 0;
    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        task_t *t = sched_get_task(i+1);
        if (t && t->pid == cur->pid) { idx = i; break; }
    }
    proc_signals[idx].handlers[sig] = handler;
    return 0;
}

void signal_check(void) {
    task_t *cur = sched_current();
    int idx = 0;
    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        task_t *t = sched_get_task(i+1);
        if (t && t->pid == cur->pid) { idx = i; break; }
    }
    uint32_t pending = proc_signals[idx].pending & ~proc_signals[idx].blocked;
    if (!pending) return;
    for (int sig = 1; sig < NSIG; sig++) {
        if (!(pending & (1u << sig))) continue;
        proc_signals[idx].pending &= ~(1u << sig);
        sighandler_t h = proc_signals[idx].handlers[sig];
        if ((uintptr_t)h == SIG_IGN) continue;
        if ((uintptr_t)h == SIG_DFL || h == 0) {
            if (sig == SIGCHLD || sig == SIGCONT) continue;
            /* Unlike SIGSTOP (handled synchronously in signal_send(), can't
               be caught/ignored), SIGTSTP's default action is also "stop"
               but it's a real catchable/ignorable signal - a handler above
               this branch already intercepted it if one was registered, so
               reaching here means default behavior: stop, same mechanism
               sched_sleep() uses (set state, then yield away). */
            if (sig == SIGTSTP) { cur->state = TASK_STOPPED; sched_yield(); return; }
            sched_exit_code(128 + sig);
            return;
        }
        h(sig);
    }
}

int signal_pending(void) {
    task_t *cur = sched_current();
    int idx = 0;
    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        task_t *t = sched_get_task(i+1);
        if (t && t->pid == cur->pid) { idx = i; break; }
    }
    return (proc_signals[idx].pending & ~proc_signals[idx].blocked) != 0;
}

void signal_fork(task_t *parent, task_t *child) {
    (void)parent; (void)child;
}
