#ifndef IDT_H
#define IDT_H

#include <stdint.h>

typedef struct __attribute__((packed)) {
    uint32_t ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, esp, ss;
} registers_t;

void idt_init(void);
void idt_set_raw(int n, uint32_t base, uint16_t sel, uint8_t flags);

typedef void (*irq_handler_t)(registers_t *);
void irq_register(int irq, irq_handler_t handler);
void irq_unregister(int irq);

void exc_register(int n, irq_handler_t handler);
void kernel_fault(registers_t *r);

#endif
