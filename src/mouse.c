#include "mouse.h"
#include "idt.h"
#include "vga.h"
#include <stdint.h>

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64

#define PIC2_CMD    0xA0
#define PIC2_DATA   0xA1
#define PIC1_DATA   0x21
#define PIC_EOI     0x20

static mouse_state_t state;
static int           detected = 0;
static int32_t       max_x = 79, max_y = 24;

static uint8_t  pkt[3];
static int      pkt_idx = 0;

static inline void outb(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));
}
static inline uint8_t inb(uint16_t p) {
    uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v;
}
static inline void io_wait(void) { outb(0x80,0); }

static void ps2_wait_write(void) {
    int t = 100000;
    while ((inb(PS2_STATUS) & 0x02) && --t);
}
static void ps2_wait_read(void) {
    int t = 100000;
    while (!(inb(PS2_STATUS) & 0x01) && --t);
}

static void mouse_write(uint8_t val) {
    ps2_wait_write();
    outb(PS2_CMD, 0xD4);
    ps2_wait_write();
    outb(PS2_DATA, val);
}

static uint8_t mouse_read(void) {
    ps2_wait_read();
    return inb(PS2_DATA);
}

static void mouse_irq(registers_t *r) {
    (void)r;
    uint8_t st = inb(PS2_STATUS);
    if (!(st & 0x01) || !(st & 0x20)) return;
    uint8_t byte = inb(PS2_DATA);

    if (pkt_idx == 0 && !(byte & 0x08)) return;

    pkt[pkt_idx++] = byte;
    if (pkt_idx < 3) return;
    pkt_idx = 0;

    uint8_t flags = pkt[0];
    int8_t  rel_x = (int8_t)pkt[1];
    int8_t  rel_y = (int8_t)pkt[2];

    if ((flags & 0x40) || (flags & 0x80)) return;

    state.dx = rel_x;
    state.dy = -rel_y;
    state.x += state.dx;
    state.y += state.dy;

    if (state.x < 0)      state.x = 0;
    if (state.y < 0)      state.y = 0;
    if (state.x > max_x)  state.x = max_x;
    if (state.y > max_y)  state.y = max_y;

    state.pressed |= (uint8_t)((flags & 0x07) & ~state.buttons);
    state.buttons = flags & 0x07;
    state.left    = (flags & 0x01) ? 1 : 0;
    state.right   = (flags & 0x02) ? 1 : 0;
    state.middle  = (flags & 0x04) ? 1 : 0;
}

void mouse_set_bounds(int32_t mx, int32_t my) {
    max_x = mx; max_y = my;
    state.x = mx / 2;
    state.y = my / 2;
}

void mouse_init(void) {
    state.x = max_x / 2;
    state.y = max_y / 2;
    state.buttons = 0;
    pkt_idx = 0;
    detected = 0;

    ps2_wait_write();
    outb(PS2_CMD, 0xA8);
    io_wait();

    ps2_wait_write();
    outb(PS2_CMD, 0x20);
    ps2_wait_read();
    uint8_t cfg = inb(PS2_DATA);
    cfg |= 0x02;
    cfg &= ~0x20;
    ps2_wait_write();
    outb(PS2_CMD, 0x60);
    ps2_wait_write();
    outb(PS2_DATA, cfg);
    io_wait();

    mouse_write(0xFF);
    uint8_t ack = mouse_read();
    if (ack != 0xFA) return;
    mouse_read();
    mouse_read();

    mouse_write(0xF6);
    mouse_read();

    mouse_write(0xF4);
    uint8_t ack2 = mouse_read();
    if (ack2 != 0xFA) return;

    detected = 1;

    uint8_t mask = inb(PIC2_DATA);
    mask &= ~0x10;
    outb(PIC2_DATA, mask);

    mask = inb(PIC1_DATA);
    mask &= ~0x04;
    outb(PIC1_DATA, mask);
    io_wait();

    irq_register(12, mouse_irq);
}

mouse_state_t *mouse_get(void)   { return &state;    }
int            mouse_ready(void) { return detected;  }

uint8_t mouse_take_pressed(void) {
    uint32_t f;
    __asm__ volatile ("pushfl; pop %0; cli" : "=r"(f) :: "memory");
    uint8_t p = state.pressed;
    state.pressed = 0;
    __asm__ volatile ("push %0; popfl" :: "r"(f) : "memory");
    return p;
}
