#include "idt.h"
#include "vga.h"
#include "kstring.h"
#include "sched.h"
#include "serial.h"
#include "paging.h"
#include "signal.h"
#include "gui.h"
#include <stdint.h>

typedef struct __attribute__((packed)) {
    uint16_t base_low;
    uint16_t selector;
    uint8_t  zero;
    uint8_t  flags;
    uint16_t base_high;
} idt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint32_t base;
} idt_ptr_t;

#define IDT_ENTRIES 256
static idt_entry_t idt[IDT_ENTRIES];
static idt_ptr_t   idt_ptr;

static irq_handler_t irq_handlers[16];

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}
static inline void io_wait(void) { outb(0x80, 0); }

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20

static void pic_remap(void) {

    outb(PIC1_CMD,  0x11); io_wait();
    outb(PIC2_CMD,  0x11); io_wait();
    outb(PIC1_DATA, 0x20); io_wait();
    outb(PIC2_DATA, 0x28); io_wait();
    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();

    outb(PIC1_DATA, 0xFC);
    outb(PIC2_DATA, 0xFF);
    io_wait();
}

static void pic_eoi(uint8_t irq) {
    if (irq >= 8) outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

static void idt_set(int n, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[n].base_low  = base & 0xFFFF;
    idt[n].base_high = (base >> 16) & 0xFFFF;
    idt[n].selector  = sel;
    idt[n].zero      = 0;
    idt[n].flags     = flags;
}

void idt_set_raw(int n, uint32_t base, uint16_t sel, uint8_t flags) {
    idt_set(n, base, sel, flags);
}

extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);

extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);

extern void lidt_flush(uint32_t ptr);

static const char *exception_names[] = {
    "Division By Zero",          "Debug",
    "Non Maskable Interrupt",    "Breakpoint",
    "Overflow",                  "Out of Bounds",
    "Invalid Opcode",            "No FPU Coprocessor",
    "Double Fault",              "Coprocessor Segment Overrun",
    "Bad TSS",                   "Segment Not Present",
    "Stack Fault",               "General Protection Fault",
    "Page Fault",                "Unknown Interrupt",
    "FPU Fault",                 "Alignment Check",
    "Machine Check",             "SIMD Float Exception",
};

static void draw_panic(registers_t *r) {
    vga_fill_rect(0, 0, 80, 25, ' ', VGA_WHITE, VGA_RED);

    vga_puts_at("*** KumOS KERNEL PANIC ***", 27, 1, VGA_YELLOW, VGA_RED);

    const char *name = (r->int_no < 20)
        ? exception_names[r->int_no] : "Unknown Exception";

    char buf[64];
    vga_puts_at("Exception: ", 5, 3, VGA_WHITE, VGA_RED);
    vga_puts_at(name, 16, 3, VGA_YELLOW, VGA_RED);

    vga_puts_at("INT:", 5, 4, VGA_WHITE, VGA_RED);
    kitoa(r->int_no, buf, 16);
    vga_puts_at(buf, 10, 4, VGA_YELLOW, VGA_RED);

    vga_puts_at("ERR:", 20, 4, VGA_WHITE, VGA_RED);
    kitoa(r->err_code, buf, 16);
    vga_puts_at(buf, 25, 4, VGA_YELLOW, VGA_RED);

    vga_puts_at("--- Register Dump ---", 29, 6, VGA_WHITE, VGA_RED);

    #define REGROW(label, val, col, row) \
        vga_puts_at(label, col, row, VGA_WHITE, VGA_RED); \
        kitoa(val, buf, 16); \
        vga_puts_at(buf, col+5, row, VGA_YELLOW, VGA_RED);

    REGROW("EAX=", r->eax, 5, 8);   REGROW("EBX=", r->ebx, 25, 8);
    REGROW("ECX=", r->ecx, 45, 8);  REGROW("EDX=", r->edx, 65, 8);
    REGROW("ESI=", r->esi, 5, 9);   REGROW("EDI=", r->edi, 25, 9);
    REGROW("EBP=", r->ebp, 45, 9);  REGROW("ESP=", r->esp, 65, 9);
    REGROW("EIP=", r->eip, 5, 10);  REGROW("EFL=", r->eflags, 25, 10);
    REGROW("CS= ", r->cs,  45, 10); REGROW("SS= ", r->ss,  65, 10);

    if (r->int_no == 14) {
        uint32_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        vga_puts_at("CR2 (fault addr):", 5, 12, VGA_WHITE, VGA_RED);
        kitoa(cr2, buf, 16);
        vga_puts_at(buf, 23, 12, VGA_YELLOW, VGA_RED);

        vga_puts_at("Cause: ", 5, 13, VGA_WHITE, VGA_RED);
        vga_puts_at((r->err_code & 1) ? "Protection violation" : "Page not present",
                    12, 13, VGA_YELLOW, VGA_RED);
        vga_puts_at((r->err_code & 2) ? " on WRITE" : " on READ",
                    32, 13, VGA_YELLOW, VGA_RED);
    }

    vga_puts_at("--- Stack Trace ---", 30, 15, VGA_WHITE, VGA_RED);
    uint32_t *ebp = (uint32_t *)r->ebp;
    int frame = 0;
    int row = 16;
    while (ebp && frame < 5 && row < 21 && paging_is_mapped((uint32_t)ebp) && paging_is_mapped((uint32_t)(ebp + 1))) {
        uint32_t ret = *(ebp + 1);
        if (ret < 0x100000 || ret > 0x800000) break;
        vga_puts_at("#", 5, row, VGA_WHITE, VGA_RED);
        kitoa(frame, buf, 10);
        vga_puts_at(buf, 7, row, VGA_WHITE, VGA_RED);
        vga_puts_at(" EIP=0x", 9, row, VGA_WHITE, VGA_RED);
        kitoa(ret, buf, 16);
        vga_puts_at(buf, 16, row, VGA_YELLOW, VGA_RED);
        ebp = (uint32_t *)*ebp;
        frame++; row++;
    }

    vga_puts_at("System halted. Reset to continue.", 23, 22, VGA_WHITE, VGA_RED);
    vga_puts_at("KumOS kernel panic - details on COM1", 22, 23, VGA_LIGHT_GREY, VGA_RED);

    __asm__ volatile ("cli; hlt");
    while(1);
}

static irq_handler_t exc_handlers[32];

void exc_register(int n, irq_handler_t handler) {
    if (n >= 0 && n < 32) exc_handlers[n] = handler;
}

static int fault_signal(uint32_t n) {
    if (n == 0) return SIGFPE;
    if (n == 6) return SIGILL;
    return SIGSEGV;
}

void kernel_fault(registers_t *r) {
    uint32_t cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    const char *name = (r->int_no < 20) ? exception_names[r->int_no] : "Unknown Exception";
    task_t *t = sched_current();
    if ((r->cs & 3) == 3) {
        serial_printf("[fault] pid %d (%s): %s at eip=%x cr2=%x err=%x, killed\r\n",
                      t->pid, t->name, name, r->eip, cr2, r->err_code);
        sched_exit_code(128 + fault_signal(r->int_no));
    }
    exc_register(14, 0);
    serial_printf("[panic] %s eip=%x cr2=%x err=%x pid=%d (%s)\r\n",
                  name, r->eip, cr2, r->err_code, t->pid, t->name);
    paging_switch(paging_root_dir());
    gui_emergency_text();
    draw_panic(r);
}

void isr_handler(registers_t *r) {
    if (r->int_no < 32 && exc_handlers[r->int_no]) {
        exc_handlers[r->int_no](r);
        return;
    }
    kernel_fault(r);
}

void irq_handler(registers_t *r) {
    uint8_t irq = (uint8_t)(r->int_no - 32);
    pic_eoi(irq);
    if (irq < 16 && irq_handlers[irq])
        irq_handlers[irq](r);
}

void irq_register(int irq, irq_handler_t handler) {
    if (irq >= 0 && irq < 16) irq_handlers[irq] = handler;
}

void irq_unregister(int irq) {
    if (irq >= 0 && irq < 16) irq_handlers[irq] = 0;
}

void idt_init(void) {
    kmemset(idt, 0, sizeof(idt));
    kmemset(irq_handlers, 0, sizeof(irq_handlers));

    idt_set(0,  (uint32_t)isr0,  0x08, 0x8E);
    idt_set(1,  (uint32_t)isr1,  0x08, 0x8E);
    idt_set(2,  (uint32_t)isr2,  0x08, 0x8E);
    idt_set(3,  (uint32_t)isr3,  0x08, 0x8E);
    idt_set(4,  (uint32_t)isr4,  0x08, 0x8E);
    idt_set(5,  (uint32_t)isr5,  0x08, 0x8E);
    idt_set(6,  (uint32_t)isr6,  0x08, 0x8E);
    idt_set(7,  (uint32_t)isr7,  0x08, 0x8E);
    idt_set(8,  (uint32_t)isr8,  0x08, 0x8E);
    idt_set(9,  (uint32_t)isr9,  0x08, 0x8E);
    idt_set(10, (uint32_t)isr10, 0x08, 0x8E);
    idt_set(11, (uint32_t)isr11, 0x08, 0x8E);
    idt_set(12, (uint32_t)isr12, 0x08, 0x8E);
    idt_set(13, (uint32_t)isr13, 0x08, 0x8E);
    idt_set(14, (uint32_t)isr14, 0x08, 0x8E);
    idt_set(15, (uint32_t)isr15, 0x08, 0x8E);
    idt_set(16, (uint32_t)isr16, 0x08, 0x8E);
    idt_set(17, (uint32_t)isr17, 0x08, 0x8E);
    idt_set(18, (uint32_t)isr18, 0x08, 0x8E);
    idt_set(19, (uint32_t)isr19, 0x08, 0x8E);
    idt_set(20, (uint32_t)isr20, 0x08, 0x8E);
    idt_set(21, (uint32_t)isr21, 0x08, 0x8E);
    idt_set(22, (uint32_t)isr22, 0x08, 0x8E);
    idt_set(23, (uint32_t)isr23, 0x08, 0x8E);
    idt_set(24, (uint32_t)isr24, 0x08, 0x8E);
    idt_set(25, (uint32_t)isr25, 0x08, 0x8E);
    idt_set(26, (uint32_t)isr26, 0x08, 0x8E);
    idt_set(27, (uint32_t)isr27, 0x08, 0x8E);
    idt_set(28, (uint32_t)isr28, 0x08, 0x8E);
    idt_set(29, (uint32_t)isr29, 0x08, 0x8E);
    idt_set(30, (uint32_t)isr30, 0x08, 0x8E);
    idt_set(31, (uint32_t)isr31, 0x08, 0x8E);

    pic_remap();

    idt_set(32, (uint32_t)irq0,  0x08, 0x8E);
    idt_set(33, (uint32_t)irq1,  0x08, 0x8E);
    idt_set(34, (uint32_t)irq2,  0x08, 0x8E);
    idt_set(35, (uint32_t)irq3,  0x08, 0x8E);
    idt_set(36, (uint32_t)irq4,  0x08, 0x8E);
    idt_set(37, (uint32_t)irq5,  0x08, 0x8E);
    idt_set(38, (uint32_t)irq6,  0x08, 0x8E);
    idt_set(39, (uint32_t)irq7,  0x08, 0x8E);
    idt_set(40, (uint32_t)irq8,  0x08, 0x8E);
    idt_set(41, (uint32_t)irq9,  0x08, 0x8E);
    idt_set(42, (uint32_t)irq10, 0x08, 0x8E);
    idt_set(43, (uint32_t)irq11, 0x08, 0x8E);
    idt_set(44, (uint32_t)irq12, 0x08, 0x8E);
    idt_set(45, (uint32_t)irq13, 0x08, 0x8E);
    idt_set(46, (uint32_t)irq14, 0x08, 0x8E);
    idt_set(47, (uint32_t)irq15, 0x08, 0x8E);

    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (uint32_t)&idt;

    lidt_flush((uint32_t)&idt_ptr);
}