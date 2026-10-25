#include "gdt.h"
#include "kstring.h"
#include <stdint.h>

typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} gdt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint32_t base;
} gdt_ptr_t;

typedef struct __attribute__((packed)) {
    uint32_t prev_tss;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t esp1;
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax, ecx, edx, ebx;
    uint32_t esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} tss_entry_t;

static uint8_t kernel_stack[8192];

#define GDT_ENTRIES 6
static gdt_entry_t gdt[GDT_ENTRIES];
static gdt_ptr_t   gdt_ptr;
static tss_entry_t tss;

static void gdt_set(int idx, uint32_t base, uint32_t limit,
                    uint8_t access, uint8_t flags) {
    gdt[idx].base_low    = base & 0xFFFF;
    gdt[idx].base_mid    = (base >> 16) & 0xFF;
    gdt[idx].base_high   = (base >> 24) & 0xFF;
    gdt[idx].limit_low   = limit & 0xFFFF;
    gdt[idx].granularity = ((limit >> 16) & 0x0F) | (flags & 0xF0);
    gdt[idx].access      = access;
}

extern void gdt_flush(uint32_t gdt_ptr);
extern void tss_flush(void);

void gdt_init(void) {
    gdt_set(0, 0, 0, 0, 0);

    gdt_set(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);

    gdt_set(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

    gdt_set(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);

    gdt_set(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    kmemset(&tss, 0, sizeof(tss));
    tss.ss0  = GDT_KERNEL_DATA;
    tss.esp0 = (uint32_t)kernel_stack + sizeof(kernel_stack);
    tss.iomap_base = sizeof(tss_entry_t);

    uint32_t tss_base  = (uint32_t)&tss;
    uint32_t tss_limit = sizeof(tss_entry_t) - 1;
    gdt_set(5, tss_base, tss_limit, 0x89, 0x40);

    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base  = (uint32_t)&gdt;

    gdt_flush((uint32_t)&gdt_ptr);
    tss_flush();
}

void tss_set_kernel_stack(uint32_t stack) {
    tss.esp0 = stack;
}
