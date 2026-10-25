#include "signal.h"
#include "sched.h"
#include "kstring.h"
#include <stdint.h>

typedef struct {
    uint32_t pending;
    uint32_t ignored;
} sigstate_t;

static sigstate_t sigs[SCHED_MAX_TASKS];

static int slot_of(task_t *t) {
    return (int)(t - sched_task_at(0));
}

static int is_kernel_task(task_t *t) {
    return slot_of(t) < 2;
}

void signal_init(void) {
    kmemset(sigs, 0, sizeof(sigs));
}

void signal_reset(task_t *t) {
    int s = slot_of(t);
    sigs[s].pending = 0;
    sigs[s].ignored = 0;
}

void signal_fork(task_t *parent, task_t *child) {
    sigs[slot_of(child)].pending = 0;
    sigs[slot_of(child)].ignored = sigs[slot_of(parent)].ignored;
}

int signal_send(int pid, int sig) {
    if (sig <= 0 || sig >= NSIG) return -1;
    task_t *t = sched_get_task(pid);
    if (!t || t->state == TASK_ZOMBIE || is_kernel_task(t)) return -1;
    int s = slot_of(t);

    switch (sig) {
    case SIGKILL:
        return sched_kill(pid, 128 + SIGKILL);
    case SIGSTOP:
        t->state = TASK_STOPPED;
        return 0;
    case SIGCONT:
        if (t->state == TASK_STOPPED) t->state = TASK_READY;
        return 0;
    case SIGCHLD:
        return 0;
    }

    if (sigs[s].ignored & (1u << sig)) return 0;
    if (sig == SIGTSTP) {
        t->state = TASK_STOPPED;
        return 0;
    }
    sigs[s].pending |= 1u << sig;
    if (t->state == TASK_SLEEPING) t->state = TASK_READY;
    return 0;
}

int signal_set_handler(int sig, sighandler_t handler) {
    if (sig <= 0 || sig >= NSIG || sig == SIGKILL || sig == SIGSTOP) return -1;
    uint32_t h = (uint32_t)(uintptr_t)handler;
    if (h != SIG_DFL && h != SIG_IGN) return -1;
    int s = slot_of(sched_current());
    if (h == SIG_IGN) { sigs[s].ignored |= 1u << sig; sigs[s].pending &= ~(1u << sig); }
    else              sigs[s].ignored &= ~(1u << sig);
    return 0;
}

void signal_check(void) {
    task_t *cur = sched_current();
    if (is_kernel_task(cur)) return;
    sigstate_t *st = &sigs[slot_of(cur)];
    uint32_t live = st->pending & ~st->ignored;
    st->pending = 0;
    if (!live) return;
    for (int sig = 1; sig < NSIG; sig++)
        if (live & (1u << sig)) sched_exit_code(128 + sig);
}

int signal_pending(void) {
    task_t *cur = sched_current();
    sigstate_t *st = &sigs[slot_of(cur)];
    return (st->pending & ~st->ignored) != 0;
}
