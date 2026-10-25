#ifndef SIGNAL_H
#define SIGNAL_H

#include <stdint.h>
#include "sched.h"

#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGABRT  6
#define SIGFPE   8
#define SIGKILL  9
#define SIGSEGV  11
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define NSIG     32

#define SIG_DFL  0
#define SIG_IGN  1

typedef void (*sighandler_t)(int);

void signal_init(void);
int  signal_send(int pid, int sig);
int  signal_set_handler(int sig, sighandler_t handler);
void signal_check(void);
int  signal_pending(void);
void signal_fork(task_t *parent, task_t *child);
void signal_reset(task_t *t);

#endif
