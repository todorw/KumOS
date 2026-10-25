#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

#define PAGE_SIZE        0x1000
#define PAGE_ENTRIES     1024
#define PAGE_ALIGN(a)    (((a) + PAGE_SIZE-1) & ~(PAGE_SIZE-1))
#define PAGE_DIR_IDX(a)  ((a) >> 22)
#define PAGE_TBL_IDX(a)  (((a) >> 12) & 0x3FF)

#define PAGE_PRESENT     (1 << 0)
#define PAGE_WRITE       (1 << 1)
#define PAGE_USER        (1 << 2)
#define PAGE_ACCESSED    (1 << 5)
#define PAGE_DIRTY       (1 << 6)
#define PAGE_COW         (1 << 9)

#define KERN_BASE        0x00100000
#define VMALLOC_BASE     0x01000000
#define VMALLOC_END      0x02000000
#define USER_BASE        0x40000000

/* Every process's ring-3 stack lives at the same fixed location, growing
   down from here - shared by elf.c (initial spawn), syscall.c (execve's
   stack remap) and paging.c (clone_dir's bounded PDE255 clone). */
#define ELF_USER_STACK_TOP   0x40000000
#define ELF_USER_STACK_SIZE  16384

/* pmm_init() reserves frames below this for the kernel's own image/heap
   (see its kern_end local) - pmm_free() refuses to free anything below it
   no matter who asks, and it's the safe lower bound for any code that
   wants to reclaim/reset a range of PDE1 (see paging_clone_dir()'s comment
   in paging.c for why that matters). */
#define KHEAP_BASE           0x00800000
#define KHEAP_SIZE           0x00200000
#define KERNEL_RESERVED_END  (KHEAP_BASE + KHEAP_SIZE)

void     pmm_init(uint32_t mem_kb);
uint32_t pmm_alloc(void);
void     pmm_free(uint32_t addr);
uint32_t pmm_used(void);
uint32_t pmm_total(void);
void     pmm_ref(uint32_t addr);
uint32_t pmm_refcount(uint32_t addr);

void     paging_init(uint32_t mem_kb);
void     paging_map(uint32_t virt, uint32_t phys, uint32_t flags);
void     paging_unmap(uint32_t virt);
void     paging_unmap_range(uint32_t start, uint32_t end);
uint32_t paging_virt_to_phys(uint32_t virt);
int      paging_is_mapped(uint32_t virt);
int      paging_user_range_ok(uint32_t addr, uint32_t len, int need_write);
int      paging_user_str_ok(uint32_t addr, uint32_t maxlen);
void     paging_dump_range(uint32_t start, uint32_t end);

void     demand_paging_init(void);
uint32_t demand_fault_count(void);
uint32_t demand_cow_count(void);

void    *vmalloc(uint32_t size);
void     vmfree(void *ptr);
int      vmalloc_copy_on_write(uint32_t virt);

uint32_t paging_clone_dir(void);
void     paging_set_pde1_clone_bound(uint32_t start, uint32_t end);
void     paging_switch(uint32_t page_dir_phys);
void     paging_free_user(uint32_t page_dir_phys);
uint32_t paging_current_dir(void);
uint32_t paging_root_dir(void);
void     paging_copy_user_range(uint32_t src_dir, uint32_t dst_dir,
                                uint32_t start, uint32_t end);

#endif
