#include "kmalloc.h"
#include "kstring.h"

#define MAGIC_FREE  0xDEADBEEF
#define MAGIC_USED  0xCAFEBABE
#define MIN_SPLIT   16

typedef struct block {
    uint32_t      magic;
    uint32_t      size;
    struct block *prev;
    struct block *next;
} block_t;

static block_t *free_list  = 0;
static uint32_t heap_start = 0;
static uint32_t heap_end   = 0;
static uint32_t heap_cap   = 0;

static inline uint32_t irq_save(void) {
    uint32_t f;
    __asm__ volatile ("pushfl; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uint32_t f) {
    __asm__ volatile ("push %0; popfl" :: "r"(f) : "memory");
}

static inline block_t *block_after(block_t *b) {
    return (block_t *)((uint8_t *)b + sizeof(block_t) + b->size);
}

static void list_remove(block_t *b) {
    if (b->prev) b->prev->next = b->next; else free_list = b->next;
    if (b->next) b->next->prev = b->prev;
    b->prev = b->next = 0;
}

static void list_push(block_t *b) {
    b->prev = 0;
    b->next = free_list;
    if (free_list) free_list->prev = b;
    free_list = b;
}

void kmalloc_init(uint32_t start, uint32_t size) {
    heap_start = start;
    heap_end   = start;
    heap_cap   = start + size;
    free_list  = 0;
}

void *kmalloc(size_t size) {
    if (!size) return 0;
    size = (size + 7) & ~7u;
    uint32_t f = irq_save();

    block_t *b = free_list;
    while (b && b->size < size) b = b->next;

    if (b) {
        list_remove(b);
        if (b->size >= size + sizeof(block_t) + MIN_SPLIT) {
            block_t *rest = (block_t *)((uint8_t *)b + sizeof(block_t) + size);
            rest->magic = MAGIC_FREE;
            rest->size  = b->size - size - sizeof(block_t);
            list_push(rest);
            b->size = size;
        }
    } else {
        uint32_t total = sizeof(block_t) + size;
        if (heap_end + total > heap_cap) { irq_restore(f); return 0; }
        b = (block_t *)heap_end;
        b->size = size;
        b->prev = b->next = 0;
        heap_end += total;
    }

    b->magic = MAGIC_USED;
    irq_restore(f);
    return (void *)((uint8_t *)b + sizeof(block_t));
}

void kfree(void *ptr) {
    if (!ptr) return;
    block_t *b = (block_t *)((uint8_t *)ptr - sizeof(block_t));
    uint32_t f = irq_save();
    if (b->magic != MAGIC_USED) { irq_restore(f); return; }
    b->magic = MAGIC_FREE;

    block_t *next = block_after(b);
    if ((uint32_t)next < heap_end && next->magic == MAGIC_FREE) {
        list_remove(next);
        b->size += sizeof(block_t) + next->size;
    }

    if ((uint32_t)block_after(b) >= heap_end) {
        heap_end = (uint32_t)b;
        for (block_t *p = free_list; p; p = p->next) {
            if (block_after(p) == b) { list_remove(p); heap_end = (uint32_t)p; break; }
        }
        irq_restore(f);
        return;
    }

    for (block_t *p = free_list; p; p = p->next) {
        if (block_after(p) == b) {
            p->size += sizeof(block_t) + b->size;
            irq_restore(f);
            return;
        }
    }
    list_push(b);
    irq_restore(f);
}

uint32_t kmalloc_used(void) {
    uint32_t f = irq_save();
    uint32_t used = 0;
    for (uint32_t pos = heap_start; pos < heap_end; ) {
        block_t *b = (block_t *)pos;
        if (b->magic == MAGIC_USED) used += b->size;
        pos += sizeof(block_t) + b->size;
    }
    irq_restore(f);
    return used;
}

uint32_t kmalloc_free(void) {
    uint32_t f = irq_save();
    uint32_t fr = heap_cap - heap_end;
    for (block_t *b = free_list; b; b = b->next) fr += b->size;
    irq_restore(f);
    return fr;
}

uint32_t kmalloc_base(void) { return heap_start; }
