#ifndef KMALLOC_H
#define KMALLOC_H

#include <stddef.h>
#include <stdint.h>

void  kmalloc_init(uint32_t start, uint32_t size);
void *kmalloc(size_t size);
void  kfree(void *ptr);
uint32_t kmalloc_free(void);
uint32_t kmalloc_used(void);
uint32_t kmalloc_base(void);

#endif
