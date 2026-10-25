#include "pipe.h"
#include "kstring.h"
#include "sched.h"
#include "signal.h"
#include <stdint.h>

static pipe_t pipes[PIPE_MAX];

void pipe_init(void) {
    kmemset(pipes, 0, sizeof(pipes));
}

int pipe_create(void) {
    for (int i = 0; i < PIPE_MAX; i++) {
        if (!pipes[i].used) {
            kmemset(&pipes[i], 0, sizeof(pipe_t));
            pipes[i].used    = 1;
            pipes[i].writers = 1;
            pipes[i].readers = 1;
            pipes[i].head    = 0;
            pipes[i].tail    = 0;
            return i;
        }
    }
    return -1;
}

pipe_t *pipe_get(int id) {
    if (id < 0 || id >= PIPE_MAX || !pipes[id].used) return 0;
    return &pipes[id];
}

void pipe_close_read(int id) {
    if (id < 0 || id >= PIPE_MAX) return;
    if (pipes[id].readers > 0) pipes[id].readers--;
    if (pipes[id].readers == 0 && pipes[id].writers == 0)
        pipes[id].used = 0;
}

void pipe_close_write(int id) {
    if (id < 0 || id >= PIPE_MAX) return;
    if (pipes[id].writers > 0) pipes[id].writers--;
    if (pipes[id].readers == 0 && pipes[id].writers == 0)
        pipes[id].used = 0;
}

int pipe_has_data(int id) {
    if (id < 0 || id >= PIPE_MAX || !pipes[id].used) return 0;
    return pipes[id].head != pipes[id].tail;
}

static uint32_t pipe_avail(pipe_t *p) {
    return (p->head - p->tail + PIPE_BUF_SIZE) % PIPE_BUF_SIZE;
}

static uint32_t pipe_free(pipe_t *p) {
    return PIPE_BUF_SIZE - 1 - pipe_avail(p);
}

int pipe_write(int id, const void *buf, uint32_t len) {
    pipe_t *p = pipe_get(id);
    if (!p) return -1;
    if (p->readers == 0) return -1;

    const uint8_t *src = (const uint8_t *)buf;
    uint32_t written = 0;

    while (written < len) {
        while (pipe_free(p) == 0) {
            if (p->readers == 0) return (int)written;
            signal_check();
            sched_yield();
        }
        p->buf[p->head] = src[written++];
        p->head = (p->head + 1) % PIPE_BUF_SIZE;
    }
    return (int)written;
}

int pipe_read(int id, void *buf, uint32_t len) {
    pipe_t *p = pipe_get(id);
    if (!p) return -1;

    uint8_t *dst = (uint8_t *)buf;
    uint32_t nread = 0;

    while (nread < len) {
        while (p->head == p->tail) {
            if (p->writers == 0 || nread > 0) return (int)nread;
            signal_check();
            sched_yield();
        }
        dst[nread++] = p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
    }
    return (int)nread;
}
