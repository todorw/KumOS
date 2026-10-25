#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>
#include "idt.h"

/* 4KB was tight enough that a task's own syscall-entry + exit/reschedule
   call chain could overflow past its allocated kernel stack into an
   adjacent kmalloc'd block (another task's suspended stack, corrupting
   its saved registers on resume) - see the fork/exec debugging session
   that traced this exact scenario. 16KB gives real headroom. */
#define SCHED_STACK_SIZE  16384
#define SCHED_MAX_TASKS   16
#define SCHED_QUANTUM     10

typedef enum {
    TASK_RUNNING  = 0,
    TASK_READY    = 1,
    TASK_SLEEPING = 2,
    TASK_DEAD     = 3,
    TASK_ZOMBIE   = 4,
    TASK_STOPPED  = 5,
} task_state_t;

typedef struct {
    uint32_t     esp;
    uint32_t    *stack;
    uint32_t     stack_size;
    task_state_t state;
    int          pid;
    int          parent_pid;
    int          exit_code;
    char         name[32];
    uint32_t     ticks;
    uint32_t     sleep_until;
    int          kum_level;
    uint32_t     page_dir_phys;
    char         cwd[128];
    uint32_t     brk;
    int          tty;
} task_t;

void    sched_init(void);
int     sched_spawn(const char *name, void (*entry)(void), int kum_level);
void    sched_exit(void);
void    sched_exit_code(int code);
void    sched_sleep(uint32_t ms);
void    sched_yield(void);
void    sched_tick(registers_t *r);
task_t *sched_current(void);
task_t *sched_get_task(int pid);
int     sched_task_count(void);
task_t *sched_task_at(int index);
int     sched_current_index(void);
int     sched_waitpid(int pid);
int     sched_wait(int *exit_code);
int     sched_waitstatus(int pid, int *exit_code);
int     sched_procstate(int pid);
void    sched_list(void);

#endif