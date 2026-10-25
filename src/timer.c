#include "timer.h"
#include "idt.h"
#include "signal.h"
#include "sched.h"
#include <stdint.h>

#define PIT_CHANNEL0  0x40
#define PIT_CMD       0x43
#define PIT_BASE_HZ   1193180

static volatile uint32_t tick_count = 0;
static uint32_t          tick_hz    = 100;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static void timer_callback(registers_t *r) {
    tick_count++;
    sched_tick(r);
    if ((r->cs & 3) == 3) signal_check();
}

void timer_init(uint32_t hz) {
    tick_hz    = hz;
    tick_count = 0;

    uint32_t divisor = PIT_BASE_HZ / hz;

    outb(PIT_CMD, 0x36);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    irq_register(0, timer_callback);
}

uint32_t timer_ticks(void) {
    return tick_count;
}

uint32_t timer_seconds(void) {
    return tick_count / tick_hz;
}

void timer_sleep(uint32_t ms) {
    uint32_t target = tick_count + (ms * tick_hz / 1000);
    while (tick_count < target)
        __asm__ volatile ("sti; hlt");
}
