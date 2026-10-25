#ifndef PIPE_H
#define PIPE_H

#include <stdint.h>

#define PIPE_BUF_SIZE   16384
#define PIPE_MAX        12

typedef struct {
    uint8_t  buf[PIPE_BUF_SIZE];
    uint32_t head, tail;
    int      writers;
    int      readers;
    int      used;
} pipe_t;

void  pipe_init(void);
int   pipe_create(void);
void  pipe_close_read(int id);
void  pipe_close_write(int id);
int   pipe_write(int id, const void *buf, uint32_t len);
int   pipe_read (int id, void *buf, uint32_t len);
int   pipe_has_data(int id);
pipe_t *pipe_get(int id);

#endif
