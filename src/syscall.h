#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include "idt.h"

#define SYS_EXIT      0
#define SYS_WRITE     1
#define SYS_READ      2
#define SYS_GETPID    3
#define SYS_SLEEP     4
#define SYS_YIELD     5
#define SYS_SBRK      6
#define SYS_UPTIME    7
#define SYS_GETKEY    8
#define SYS_PUTCHAR   9
#define SYS_OPEN     10
#define SYS_CLOSE    11
#define SYS_FREAD    12
#define SYS_FWRITE   13
#define SYS_LISTDIR  14
#define SYS_WAITPID  15
#define SYS_WAIT     16
#define SYS_GETTIME  17
#define SYS_SERIAL   18
#define SYS_EXEC     19
#define SYS_FORK     20
#define SYS_PIPE     21
#define SYS_DUP2     22
#define SYS_GETCWD   23
#define SYS_CHDIR    24
#define SYS_STAT     25
#define SYS_UNLINK   26
#define SYS_CHMOD    27
#define SYS_GETPPID  28
#define SYS_ISATTY   29
#define SYS_KILL     30
#define SYS_SIGNAL   31
#define SYS_TCP_CONNECT 40
#define SYS_TCP_SEND    41
#define SYS_TCP_RECV    42
#define SYS_TCP_CLOSE   43
#define SYS_EXECVE   44
#define SYS_SELECT   45
#define SYS_POLL     46
#define SYS_WAITSTATUS 47
#define SYS_PROCSTATE  48
#define SYSCALL_MAX  49

void syscall_init(void);

struct syscall_regs;
uint32_t syscall_dispatch(struct syscall_regs *regs);

static inline void sys_exit(int code) {
    __asm__ volatile("int $0x80" :: "a"(SYS_EXIT), "b"(code));
}
static inline int sys_write(const char *buf, uint32_t len) {
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(SYS_WRITE),"b"(buf),"c"(len));
    return r;
}
static inline int sys_read(char *buf, uint32_t len) {
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(SYS_READ),"b"(buf),"c"(len));
    return r;
}
static inline int sys_getpid(void) {
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(SYS_GETPID));
    return r;
}
static inline void sys_sleep(uint32_t ms) {
    __asm__ volatile("int $0x80" :: "a"(SYS_SLEEP),"b"(ms));
}
static inline void sys_yield(void) {
    __asm__ volatile("int $0x80" :: "a"(SYS_YIELD));
}
static inline uint32_t sys_uptime(void) {
    uint32_t r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(SYS_UPTIME));
    return r;
}
static inline char sys_getkey(void) {
    char r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(SYS_GETKEY));
    return r;
}
static inline void sys_putchar(char c) {
    __asm__ volatile("int $0x80" :: "a"(SYS_PUTCHAR),"b"((int)c));
}

static inline void sys_puts(const char *s) {
    uint32_t len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}
static inline int sys_open(const char *name) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(SYS_OPEN),"b"(name)); return r;
}
static inline int sys_close(int fd) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(SYS_CLOSE),"b"(fd)); return r;
}
static inline int sys_fread(int fd, void *buf, uint32_t sz) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(SYS_FREAD),"b"(fd),"c"(buf),"d"(sz)); return r;
}
static inline int sys_fwrite(int fd, const void *buf, uint32_t sz) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(SYS_FWRITE),"b"(fd),"c"(buf),"d"(sz)); return r;
}
static inline int sys_waitpid(int pid) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(SYS_WAITPID),"b"(pid)); return r;
}
static inline int sys_wait(int *code) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(SYS_WAIT),"b"(code)); return r;
}
static inline void sys_serial(const char *buf, uint32_t len) {
    __asm__ volatile("int $0x80"::"a"(SYS_SERIAL),"b"(buf),"c"(len):"memory");
}
static inline int sys_exec(const char *path) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(SYS_EXEC),"b"(path)); return r;
}
static inline int sys_fork(void) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(SYS_FORK)); return r;
}
static inline int sys_pipe(int fds[2]) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(SYS_PIPE),"b"(fds)); return r;
}
static inline int sys_dup2(int old, int nw) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(SYS_DUP2),"b"(old),"c"(nw)); return r;
}
static inline int sys_getcwd(char *buf, uint32_t sz) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(SYS_GETCWD),"b"(buf),"c"(sz)); return r;
}
static inline int sys_chdir(const char *path) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(SYS_CHDIR),"b"(path)); return r;
}
static inline int sys_getppid(void) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(SYS_GETPPID)); return r;
}
static inline int sys_isatty(int fd) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(SYS_ISATTY),"b"(fd)); return r;
}
static inline int sys_unlink(const char *path) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(SYS_UNLINK),"b"(path)); return r;
}

#endif
