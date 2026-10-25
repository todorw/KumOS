
#include "syscall.h"
#include "idt.h"
#include "vga.h"
#include "keyboard.h"
#include "timer.h"
#include "sched.h"
#include "fs.h"
#include "fat12.h"
#include "paging.h"
#include "rtc.h"
#include "serial.h"
#include "kstring.h"
#include "vfs.h"
#include "pipe.h"
#include "elf.h"
#include "signal.h"
#include "kstring.h"
#include "kmalloc.h"
#include "net.h"
#include <stdint.h>

extern void isr128(void);
extern void user_entry_trampoline(void);
extern void switch_context(uint32_t *old_esp, uint32_t new_esp);

/* Exact stack layout isr128 builds before calling us: pusha, then
   push ds/es/fs/gs, then the CPU's own ring3->ring0 transition pushes
   (eip/cs/eflags/useresp/ss). Needed so sc_fork() can build a child
   that resumes at the parent's real return address instead of a
   fabricated one - see the invalid-opcode panic this fixed. */
typedef struct syscall_regs {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t eip, cs, eflags, useresp, ss;
} __attribute__((packed)) syscall_regs_t;

static syscall_regs_t *g_cur_regs;

/* Every syscall below that takes a raw user pointer must run it through
   one of these before dereferencing it - see paging_user_range_ok()'s
   comment for what this closes (arbitrary kernel-memory read/write from
   ring 3). USTR_MAX bounds the walk for path-shaped strings; it matches
   VFS_MAX_PATH so a path that would get rejected downstream anyway is
   rejected here just as validly. */
#define USTR_MAX 128
static inline int ubuf_r(uint32_t addr, uint32_t len) { return paging_user_range_ok(addr, len, 0); }
static inline int ubuf_w(uint32_t addr, uint32_t len) { return paging_user_range_ok(addr, len, 1); }
static inline int ustr(uint32_t addr)                 { return paging_user_str_ok(addr, USTR_MAX); }

static uint32_t sc_exit(uint32_t code, uint32_t b, uint32_t c) {
    (void)b; (void)c;

    sched_exit_code((int)code);
    return 0;
}

static uint32_t sc_write(uint32_t buf_addr, uint32_t len, uint32_t c) {
    (void)c;

    if (len > 4096) len = 4096;
    if (!ubuf_r(buf_addr, len)) return (uint32_t)-1;
    const char *buf = (const char *)buf_addr;
    int n = vfs_write(1, buf, len);
    return n < 0 ? 0 : (uint32_t)n;
}

static uint32_t sc_read(uint32_t buf_addr, uint32_t len, uint32_t c) {
    (void)c;
    if (len == 0) return 0;
    if (len > 256) len = 256;
    if (!ubuf_w(buf_addr, len)) return 0;
    char *buf = (char *)buf_addr;
    return (uint32_t)keyboard_getline(buf, (int)len);
}

static uint32_t sc_getpid(uint32_t a, uint32_t b, uint32_t c) {
    (void)a; (void)b; (void)c;
    return (uint32_t)sched_current()->pid;
}

static uint32_t sc_sleep(uint32_t ms, uint32_t b, uint32_t c) {
    (void)b; (void)c;
    timer_sleep(ms);
    return 0;
}

static uint32_t sc_yield(uint32_t a, uint32_t b, uint32_t c) {
    (void)a; (void)b; (void)c;
    sched_yield();
    return 0;
}

static uint32_t sc_sbrk(uint32_t inc, uint32_t b, uint32_t c) {
    (void)inc; (void)b; (void)c;
    return (uint32_t)-1;
}

static uint32_t sc_uptime(uint32_t a, uint32_t b, uint32_t c) {
    (void)a; (void)b; (void)c;
    return timer_seconds();
}

static uint32_t sc_getkey(uint32_t a, uint32_t b, uint32_t c) {
    (void)a; (void)b; (void)c;
    return (uint32_t)keyboard_getchar_blocking();
}

static uint32_t sc_putchar(uint32_t ch, uint32_t b, uint32_t c) {
    (void)b; (void)c;
    char ch8 = (char)ch;
    vfs_write(1, &ch8, 1);
    return 0;
}

static uint32_t sc_waitpid(uint32_t pid, uint32_t b, uint32_t c) {
    (void)b; (void)c;
    return (uint32_t)sched_waitpid((int)pid);
}

static uint32_t sc_wait(uint32_t code_ptr, uint32_t b, uint32_t c) {
    (void)b; (void)c;
    return (uint32_t)sched_wait((int *)(code_ptr ? code_ptr : 0));
}

static uint32_t sc_waitstatus(uint32_t pid, uint32_t code_ptr, uint32_t c) {
    (void)c;
    return (uint32_t)sched_waitstatus((int)pid, (int *)(code_ptr ? code_ptr : 0));
}

static uint32_t sc_procstate(uint32_t pid, uint32_t b, uint32_t c) {
    (void)b; (void)c;
    return (uint32_t)sched_procstate((int)pid);
}

static uint32_t sc_gettime(uint32_t buf_addr, uint32_t b, uint32_t c) {
    (void)b; (void)c;
    if (!ubuf_w(buf_addr, sizeof(rtc_time_t))) return (uint32_t)-1;
    rtc_time_t t = rtc_read();
    kmemcpy((void *)buf_addr, &t, sizeof(rtc_time_t));
    return 0;
}

static uint32_t sc_serial_write(uint32_t buf, uint32_t len, uint32_t c) {
    (void)c;
    if (len > 4096) return (uint32_t)-1;
    if (!ubuf_r(buf, len)) return (uint32_t)-1;
    if (serial_ready()) serial_write(COM1, (const char *)buf, len);
    return len;
}

static uint32_t sc_vfs_open(uint32_t path, uint32_t flags, uint32_t c) {
    (void)c;
    if (!ustr(path)) return (uint32_t)-1;
    return (uint32_t)vfs_open((const char *)path, (int)flags);
}
static uint32_t sc_vfs_close(uint32_t fd, uint32_t b, uint32_t c) {
    (void)b; (void)c;
    return (uint32_t)vfs_close((int)fd);
}
static uint32_t sc_vfs_read(uint32_t fd, uint32_t buf, uint32_t len) {
    if (len == 0) return 0;
    if (!ubuf_w(buf, len)) return (uint32_t)-1;
    return (uint32_t)vfs_read((int)fd, (void *)buf, len);
}
static uint32_t sc_vfs_write(uint32_t fd, uint32_t buf, uint32_t len) {
    if (len == 0) return 0;
    if (!ubuf_r(buf, len)) return (uint32_t)-1;
    return (uint32_t)vfs_write((int)fd, (const void *)buf, len);
}
static uint32_t sc_vfs_readdir(uint32_t path, uint32_t buf, uint32_t sz) {
    if (!ustr(path) || sz == 0 || !ubuf_w(buf, sz)) return 0;
    return (uint32_t)vfs_readdir((const char *)path, (char *)buf, sz);
}
static uint32_t sc_vfs_stat(uint32_t path, uint32_t st, uint32_t c) {
    (void)c;
    if (!ustr(path) || !ubuf_w(st, sizeof(vfs_stat_t))) return (uint32_t)-1;
    return (uint32_t)vfs_stat((const char *)path, (vfs_stat_t *)st);
}
static uint32_t sc_vfs_unlink(uint32_t path, uint32_t b, uint32_t c) {
    (void)b;(void)c;
    if (!ustr(path)) return (uint32_t)-1;
    return (uint32_t)vfs_unlink((const char *)path);
}
static uint32_t sc_vfs_chmod(uint32_t path, uint32_t mode, uint32_t c) {
    (void)c;
    if (!ustr(path) || !ustr(mode)) return (uint32_t)-1;
    return (uint32_t)vfs_chmod((const char *)path, (const char *)mode);
}
static uint32_t sc_vfs_pipe(uint32_t fds, uint32_t b, uint32_t c) {
    (void)b;(void)c;
    if (!ubuf_w(fds, sizeof(int)*2)) return (uint32_t)-1;
    return (uint32_t)vfs_pipe((int *)fds);
}
static uint32_t sc_vfs_dup2(uint32_t old, uint32_t nw, uint32_t c) {
    (void)c;
    return (uint32_t)vfs_dup2((int)old, (int)nw);
}
static uint32_t sc_vfs_getcwd(uint32_t buf, uint32_t sz, uint32_t c) {
    (void)c;
    if (sz == 0 || !ubuf_w(buf, sz)) return (uint32_t)-1;
    return (uint32_t)vfs_getcwd((char *)buf, sz);
}
static uint32_t sc_vfs_chdir(uint32_t path, uint32_t b, uint32_t c) {
    (void)b;(void)c;
    if (!ustr(path)) return (uint32_t)-1;
    return (uint32_t)vfs_chdir((const char *)path);
}
static uint32_t sc_isatty(uint32_t fd, uint32_t b, uint32_t c) {
    (void)b;(void)c;
    return (uint32_t)vfs_isatty((int)fd);
}

static uint32_t sc_kill(uint32_t pid, uint32_t sig, uint32_t c) {
    (void)c;
    return (uint32_t)signal_send((int)pid, (int)sig);
}

static uint32_t sc_signal_set(uint32_t sig, uint32_t handler, uint32_t c) {
    (void)c;
    return (uint32_t)signal_set_handler((int)sig, (sighandler_t)handler);
}
static uint32_t sc_getppid(uint32_t a, uint32_t b, uint32_t c) {
    (void)a;(void)b;(void)c;
    task_t *t = sched_current();
    return (uint32_t)t->parent_pid;
}

static uint32_t sc_execve(uint32_t path_addr, uint32_t argv_addr, uint32_t envp_addr) {
    (void)envp_addr;
    if (!ustr(path_addr)) return (uint32_t)-1;
    const char *path = (const char *)path_addr;

    char upper[64]; int i = 0;
    while (path[i] && i < 59) { char ch = path[i]; if (ch >= 'a' && ch <= 'z') ch -= 32; upper[i++] = ch; }
    upper[i] = 0;
    if (!kstrchr(upper, '.')) kstrcat(upper, ".ELF");

    if (elf_validate_disk(upper) != 0) return (uint32_t)-1;

    static char argbuf[ELF_MAX_ARGS][ELF_MAX_ARG_LEN];
    const char *kargv[ELF_MAX_ARGS];
    int argc = 0;
    if (argv_addr) {
        while (argc < ELF_MAX_ARGS && ubuf_r(argv_addr + (uint32_t)argc * 4, 4)) {
            uint32_t s_addr = ((uint32_t *)argv_addr)[argc];
            if (!s_addr) break;
            if (!paging_user_str_ok(s_addr, ELF_MAX_ARG_LEN)) return (uint32_t)-1;
            kstrncpy_safe(argbuf[argc], (const char *)s_addr, ELF_MAX_ARG_LEN);
            kargv[argc] = argbuf[argc];
            argc++;
        }
    }
    if (argc == 0) { kstrncpy_safe(argbuf[0], upper, ELF_MAX_ARG_LEN); kargv[0] = argbuf[0]; argc = 1; }

    task_t *cur = sched_current();
    uint32_t new_dir = paging_new_dir();
    if (!new_dir) return (uint32_t)-1;
    uint32_t old_dir = cur->page_dir_phys;
    cur->page_dir_phys = new_dir;
    paging_switch(new_dir);
    if (old_dir) paging_free_user(old_dir);

    elf_load_result_t r = elf_load_disk(upper);
    uint32_t user_esp = r.error == 0 ? elf_user_stack(argc, kargv) : 0;
    if (!user_esp) sched_exit_code(127);

    kstrncpy_safe(cur->name, kargv[0], sizeof(cur->name));
    cur->esp = elf_user_frame((uint32_t *)((uint8_t *)cur->stack + cur->stack_size), r.entry, user_esp);

    uint32_t discard_esp;
    switch_context(&discard_esp, cur->esp);
    return 0;
}

static uint32_t sc_exec(uint32_t path_addr, uint32_t b, uint32_t c) {
    return sc_execve(path_addr, b, c);
}

static uint32_t sc_fork(uint32_t a, uint32_t b, uint32_t c) {
    (void)a;(void)b;(void)c;
    task_t *parent = sched_current();
    syscall_regs_t *pr = g_cur_regs;
    uint32_t child_dir = paging_clone_dir();
    if (!child_dir) return (uint32_t)-1;

    uint32_t *child_kstack = kmalloc(SCHED_STACK_SIZE);
    if (!child_kstack) { paging_free_user(child_dir); return (uint32_t)-1; }

    int child_pid = sched_spawn(parent->name, 0, parent->kum_level);
    task_t *child = child_pid < 0 ? 0 : sched_get_task(child_pid);
    if (!child) {
        kfree(child_kstack);
        paging_free_user(child_dir);
        return (uint32_t)-1;
    }

    child->parent_pid    = parent->pid;
    child->page_dir_phys = child_dir;
    kstrcpy(child->cwd, parent->cwd);
    signal_fork(parent, child);

    kfree(child->stack);
    child->stack      = child_kstack;
    child->stack_size = SCHED_STACK_SIZE;

    uint32_t *sp = (uint32_t *)((uint8_t *)child_kstack + SCHED_STACK_SIZE);
    *--sp = pr->ss;
    *--sp = pr->useresp;
    *--sp = pr->eflags;
    *--sp = pr->cs;
    *--sp = pr->eip;
    *--sp = 0;
    *--sp = pr->ecx;
    *--sp = pr->edx;
    *--sp = pr->ebx;
    *--sp = 0;
    *--sp = pr->ebp;
    *--sp = pr->esi;
    *--sp = pr->edi;
    *--sp = pr->ds;
    *--sp = (uint32_t)(uintptr_t)user_entry_trampoline;
    *--sp = 0x202;
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;
    child->esp = (uint32_t)(uintptr_t)sp;

    return (uint32_t)child_pid;
}

static uint32_t sc_tcp_connect(uint32_t ip, uint32_t port, uint32_t x) {
    (void)x; return (uint32_t)tcp_connect(ip,(uint16_t)port);
}
static uint32_t sc_tcp_send(uint32_t s, uint32_t buf, uint32_t len) {
    if (len == 0) return 0;
    if (!ubuf_r(buf, len)) return (uint32_t)-1;
    return (uint32_t)tcp_send((int)s,(const void*)buf,(uint16_t)len);
}
static uint32_t sc_tcp_recv(uint32_t s, uint32_t buf, uint32_t len) {
    if (len == 0) return 0;
    if (!ubuf_w(buf, len)) return (uint32_t)-1;
    return (uint32_t)tcp_recv((int)s,(void*)buf,(uint16_t)len);
}
static uint32_t sc_tcp_close(uint32_t s, uint32_t b, uint32_t x) {
    (void)b;(void)x; tcp_close((int)s); return 0;
}

static uint32_t sc_select(uint32_t nfds, uint32_t rfds_addr, uint32_t wfds_addr) {
    uint32_t timeout_ms = 5000;
    uint32_t deadline = timer_ticks() + timeout_ms / 10;
    int ready = 0;

    if (rfds_addr && !ubuf_r(rfds_addr, sizeof(uint32_t))) return 0;

    while (timer_ticks() < deadline) {
        for (uint32_t fd = 0; fd < nfds && fd < 32; fd++) {
            if (rfds_addr && (*(uint32_t*)rfds_addr & (1u<<fd))) {
                vfs_fd_t *f = vfs_get_fd((int)fd);
                if (!f) continue;
                if (f->type == VFS_PIPE && pipe_has_data(f->pipe_id)) {
                    ready++; continue;
                }
                if (f->type == VFS_DEV && f->fd_data == 0) {
                    if (keyboard_has_input()) ready++;
                }
            }
        }
        if (ready) break;
        sched_yield();
    }
    return (uint32_t)ready;
}

static uint32_t sc_poll(uint32_t fds_addr, uint32_t nfds, uint32_t timeout_ms) {
    return sc_select(nfds, fds_addr, 0);
    (void)timeout_ms;
}
typedef uint32_t (*syscall_fn)(uint32_t, uint32_t, uint32_t);

static syscall_fn syscall_table[SYSCALL_MAX] = {
    [SYS_EXIT]    = sc_exit,
    [SYS_WRITE]   = sc_write,
    [SYS_READ]    = sc_read,
    [SYS_GETPID]  = sc_getpid,
    [SYS_SLEEP]   = sc_sleep,
    [SYS_YIELD]   = sc_yield,
    [SYS_SBRK]    = sc_sbrk,
    [SYS_UPTIME]  = sc_uptime,
    [SYS_GETKEY]  = sc_getkey,
    [SYS_PUTCHAR] = sc_putchar,
    [SYS_OPEN]    = sc_vfs_open,
    [SYS_CLOSE]   = sc_vfs_close,
    [SYS_FREAD]   = sc_vfs_read,
    [SYS_FWRITE]  = sc_vfs_write,
    [SYS_LISTDIR] = sc_vfs_readdir,
    [SYS_WAITPID] = sc_waitpid,
    [SYS_WAIT]    = sc_wait,
    [SYS_GETTIME] = sc_gettime,
    [SYS_SERIAL]  = sc_serial_write,
    [SYS_EXEC]    = sc_exec,
    [SYS_FORK]    = sc_fork,
    [SYS_PIPE]    = sc_vfs_pipe,
    [SYS_DUP2]    = sc_vfs_dup2,
    [SYS_GETCWD]  = sc_vfs_getcwd,
    [SYS_CHDIR]   = sc_vfs_chdir,
    [SYS_STAT]    = sc_vfs_stat,
    [SYS_UNLINK]  = sc_vfs_unlink,
    [SYS_CHMOD]   = sc_vfs_chmod,
    [SYS_GETPPID] = sc_getppid,
    [SYS_ISATTY]  = sc_isatty,
    [SYS_KILL]    = sc_kill,
    [SYS_SIGNAL]  = sc_signal_set,
    [SYS_TCP_CONNECT] = sc_tcp_connect,
    [SYS_TCP_SEND]    = sc_tcp_send,
    [SYS_TCP_RECV]    = sc_tcp_recv,
    [SYS_TCP_CLOSE]   = sc_tcp_close,
    [SYS_EXECVE]      = sc_execve,
    [SYS_SELECT]      = sc_select,
    [SYS_POLL]        = sc_poll,
    [SYS_WAITSTATUS]  = sc_waitstatus,
    [SYS_PROCSTATE]   = sc_procstate,
};

uint32_t syscall_dispatch(syscall_regs_t *regs) {
    g_cur_regs = regs;
    uint32_t num = regs->eax, a = regs->ebx, b = regs->ecx, c = regs->edx;
    if (num >= SYSCALL_MAX || !syscall_table[num]) return (uint32_t)-1;
    uint32_t ret = syscall_table[num](a, b, c);
    signal_check();
    return ret;
}

void syscall_init(void) {
    idt_set_raw(0x80, (uint32_t)isr128, 0x08, 0xEE);
}