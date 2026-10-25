#ifndef IRQ_H
#define IRQ_H

#include <stdint.h>

static inline uint32_t irq_save(void) {
    uint32_t f;
    __asm__ volatile ("pushfl; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}

static inline void irq_restore(uint32_t f) {
    __asm__ volatile ("push %0; popfl" :: "r"(f) : "memory");
}

#endif
