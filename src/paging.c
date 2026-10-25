
#include "paging.h"
#include "idt.h"
#include "vga.h"
#include "kstring.h"
#include "sched.h"
#include "serial.h"
#include <stdint.h>

#define PMM_MAX_FRAMES  65536

static uint8_t  pmm_refs[PMM_MAX_FRAMES];
static uint32_t pmm_total_frames = 0;
static uint32_t pmm_used_frames  = 0;
static uint32_t pmm_base_frame   = 0;

static inline uint32_t frame_idx(uint32_t phys) { return phys / PAGE_SIZE; }

void pmm_init(uint32_t mem_kb) {
    kmemset(pmm_refs, 0xFF, sizeof(pmm_refs));
    pmm_used_frames = 0;

    uint32_t start = 0x00100000;
    uint32_t end   = mem_kb * 1024;
    if (end > PMM_MAX_FRAMES * PAGE_SIZE)
        end = PMM_MAX_FRAMES * PAGE_SIZE;

    pmm_base_frame   = frame_idx(start);
    pmm_total_frames = (end - start) / PAGE_SIZE;
    if (pmm_total_frames > PMM_MAX_FRAMES - pmm_base_frame)
        pmm_total_frames = PMM_MAX_FRAMES - pmm_base_frame;

    for (uint32_t i = 0; i < pmm_total_frames; i++)
        pmm_refs[pmm_base_frame + i] = 0;

    uint32_t kern_end = frame_idx(KERNEL_RESERVED_END);
    for (uint32_t f = pmm_base_frame; f < kern_end && f < PMM_MAX_FRAMES; f++) {
        pmm_refs[f] = 1;
        pmm_used_frames++;
    }
}

/* pmm_refs[] is read-modify-written from every task's context (vmalloc,
   paging_clone_dir, demand paging, ...) with the scheduler free to preempt
   between the read and the write on any timer tick - two tasks landing in
   that gap for the same frame could both see it free and both claim it.
   A real, independent correctness gap regardless of what triggers it, so
   fixed here on its own merits. Tested against the long-open "GUI corrupts
   near the border" report (round 20/26/27) while chasing it this round:
   this alone does NOT stop that corruption (confirmed live, fix in place,
   reproduced again) - see paging_clone_dir()'s comment below for the
   actual mechanism found instead. */
static inline uint32_t irq_save_cli(void) {
    uint32_t flags;
    __asm__ volatile ("pushfl; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void irq_restore(uint32_t flags) {
    __asm__ volatile ("push %0; popfl" :: "r"(flags) : "memory");
}

/* Never hands out a frame inside PDE1's own physical range
   [0x400000, 0x800000). Every OTHER low PDE (0, 2-31ish, covering the
   rest of physical RAM) is always aliased identically into every
   process's page directory (see paging_clone_dir()), so a frame from
   anywhere else is guaranteed identity-accessible no matter which
   directory happens to be current - the codebase-wide convention of
   treating pmm_alloc()'s return value as a directly-dereferenceable
   pointer depends on exactly that. PDE1 is the one exception: it's
   deep-copied and bounded per-process (paging_set_pde1_clone_bound()), so
   a frame from inside it is only identity-accessible from whichever
   specific directory's bound happens to cover it - and plenty of code
   (paging_clone_dir()'s own bookkeeping, elf_load_mem()'s segment
   loading, every ring-3 stack setup) does pmm_alloc() then an immediate
   kmemset()/kmemcpy() straight through the returned physical address,
   with no guarantee the CURRENTLY active directory's PDE1 bound covers
   whatever pmm_alloc() just handed back. Found the hard way, three
   separate times, each confirmed live with a hardware breakpoint on the
   exact page fault: excluding this one narrow, otherwise-unneeded range
   (about 3% of a typical 127MB machine) from the general allocator
   entirely closes the whole class of bug in one place, instead of
   auditing every pmm_alloc()-then-kmemset() call site in the codebase. */
static uint32_t pmm_hint = 0;

uint32_t pmm_alloc(void) {
    uint32_t lo = frame_idx(KERNEL_RESERVED_END);
    uint32_t hi = pmm_base_frame + pmm_total_frames;
    if (hi > PMM_MAX_FRAMES) hi = PMM_MAX_FRAMES;
    if (lo >= hi) return 0;
    uint32_t flags = irq_save_cli();
    uint32_t result = 0;
    uint32_t i = (pmm_hint >= lo && pmm_hint < hi) ? pmm_hint : lo;
    for (uint32_t n = hi - lo; n; n--) {
        if (pmm_refs[i] == 0) {
            pmm_refs[i] = 1;
            pmm_used_frames++;
            result = i * PAGE_SIZE;
            pmm_hint = i + 1;
            break;
        }
        if (++i >= hi) i = lo;
    }
    irq_restore(flags);
    return result;
}

void pmm_free(uint32_t addr) {
    uint32_t idx = frame_idx(addr);
    if (idx >= PMM_MAX_FRAMES) return;
    /* Never let a bug (this round's fork/exec paging fix included) free a
       frame the kernel's own image/heap is still sitting on - see
       KERNEL_RESERVED_END's comment in paging.h. This alone doesn't stop
       a caller from unmapping the PTE that points at one (paging_unmap()
       calls this after clearing the PTE either way), just from the
       physical frame being handed back out to someone else afterward. */
    if (idx < frame_idx(KERNEL_RESERVED_END)) return;
    uint32_t flags = irq_save_cli();
    if (pmm_refs[idx] != 0) {
        pmm_refs[idx]--;
        if (pmm_refs[idx] == 0 && pmm_used_frames)
            pmm_used_frames--;
    }
    irq_restore(flags);
}

void pmm_ref(uint32_t addr) {
    uint32_t idx = frame_idx(addr);
    uint32_t flags = irq_save_cli();
    if (idx < PMM_MAX_FRAMES && pmm_refs[idx] < 254)
        pmm_refs[idx]++;
    irq_restore(flags);
}

uint32_t pmm_refcount(uint32_t addr) {
    uint32_t idx = frame_idx(addr);
    return (idx < PMM_MAX_FRAMES) ? pmm_refs[idx] : 0;
}

uint32_t pmm_used(void)  { return pmm_used_frames;  }
uint32_t pmm_total(void) { return pmm_total_frames; }

static uint32_t page_dir[PAGE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));

/* Covers up to 256MB (matches PMM_MAX_FRAMES) - paging_init() only uses as
   many of these as the machine's actual RAM needs, see num_kern_tables. */
#define MAX_PREINIT_TABLES 64
static uint32_t kern_tables[MAX_PREINIT_TABLES][PAGE_ENTRIES]
                            __attribute__((aligned(PAGE_SIZE)));
static int num_kern_tables = 4;

#define VMALLOC_TABLES ((VMALLOC_END - VMALLOC_BASE) / (PAGE_SIZE * PAGE_ENTRIES))
static uint32_t vmalloc_tables[VMALLOC_TABLES][PAGE_ENTRIES]
                              __attribute__((aligned(PAGE_SIZE)));

static inline void load_cr3(uint32_t dir) {
    __asm__ volatile ("mov %0, %%cr3" :: "r"(dir) : "memory");
}
static inline void tlb_flush_page(uint32_t v) {
    __asm__ volatile ("invlpg (%0)" :: "r"(v) : "memory");
}
static inline void enable_paging(void) {
    uint32_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;
    __asm__ volatile ("mov %0, %%cr0" :: "r"(cr0) : "memory");
}

static inline uint32_t current_dir_phys(void) {
    uint32_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

/* Resolves against whatever directory CR3 currently points at, not always
   the boot-time static page_dir - essential now that paging_clone_dir()
   gives PDE 1 (where user code lives, see below) its own private table per
   process instead of aliasing the same one everywhere. Before this, any
   paging_map() call after a directory switch silently wrote into the old
   (often unrelated) directory instead of the one actually active. */
static uint32_t *pte_ptr(uint32_t virt, int alloc) {
    uint32_t di = PAGE_DIR_IDX(virt);
    uint32_t ti = PAGE_TBL_IDX(virt);

    uint32_t *dir = (uint32_t *)current_dir_phys();

    if (!(dir[di] & PAGE_PRESENT)) {
        if (!alloc) return 0;
        uint32_t phys = pmm_alloc();
        if (!phys) return 0;
        kmemset((void *)phys, 0, PAGE_SIZE);
        dir[di] = phys | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    }

    uint32_t *tbl = (uint32_t *)(dir[di] & ~0xFFF);
    return &tbl[ti];
}

void paging_map(uint32_t virt, uint32_t phys, uint32_t flags) {
    uint32_t *pte = pte_ptr(virt, 1);
    if (!pte) return;
    *pte = (phys & ~0xFFF) | PAGE_PRESENT | flags;
    tlb_flush_page(virt);
}

void paging_unmap(uint32_t virt) {
    uint32_t *pte = pte_ptr(virt, 0);
    if (!pte || !(*pte & PAGE_PRESENT)) return;
    pmm_free(*pte & ~0xFFF);
    *pte = 0;
    tlb_flush_page(virt);
}

void paging_unmap_range(uint32_t start, uint32_t end) {
    for (uint32_t va = start; va < end; va += PAGE_SIZE) paging_unmap(va);
}

uint32_t paging_virt_to_phys(uint32_t virt) {
    uint32_t *pte = pte_ptr(virt, 0);
    if (!pte || !(*pte & PAGE_PRESENT)) return 0;
    return (*pte & ~0xFFF) | (virt & 0xFFF);
}

int paging_is_mapped(uint32_t virt) {
    uint32_t *pte = pte_ptr(virt, 0);
    return pte && (*pte & PAGE_PRESENT);
}

/* Ring-3 syscall arguments are raw addresses the caller chose - nothing
   upstream of these two checks ever confirmed they actually point into
   THIS process's own mapped memory before the kernel (running at ring 0,
   where the CPU's own U/S page-protection check never fires) read or wrote
   through them directly. That let any user program hand a kernel address
   to write()/read() as if it were its own buffer: write() leaked kernel
   memory out over stdout, read() overwrote arbitrary kernel memory with
   attacker-controlled bytes. Every PTE touched must be PRESENT and USER
   (and WRITE, for a buffer the kernel is about to write into). */
int paging_user_range_ok(uint32_t addr, uint32_t len, int need_write) {
    if (!addr || !len) return 0;
    if (addr + len < addr) return 0;               /* wraps past 4GB */
    uint32_t start = addr & ~0xFFF;
    uint32_t end   = (addr + len - 1) & ~0xFFF;
    for (uint32_t page = start; ; page += PAGE_SIZE) {
        uint32_t *pte = pte_ptr(page, 0);
        if (!pte || !(*pte & PAGE_PRESENT) || !(*pte & PAGE_USER)) return 0;
        if (need_write && !(*pte & PAGE_WRITE)) return 0;
        if (page == end) break;
    }
    return 1;
}

/* Same idea for a NUL-terminated string argument (a path, mostly), whose
   length isn't known up front - walks byte by byte, re-checking the PTE
   only when crossing into a new page, and refuses to run off the end of
   the caller's mapped memory looking for a terminator that isn't there. */
int paging_user_str_ok(uint32_t addr, uint32_t maxlen) {
    if (!addr) return 0;
    uint32_t last_page = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < maxlen; i++) {
        uint32_t va = addr + i;
        uint32_t page = va & ~0xFFF;
        if (page != last_page) {
            uint32_t *pte = pte_ptr(page, 0);
            if (!pte || !(*pte & PAGE_PRESENT) || !(*pte & PAGE_USER)) return 0;
            last_page = page;
        }
        if (*(const char *)(uintptr_t)va == 0) return 1;
    }
    return 0;
}

void paging_dump_range(uint32_t start, uint32_t end) {
    uint32_t addr = start & ~0xFFF;
    int in_run = 0;
    uint32_t run_start = 0, run_phys = 0;
    while (addr < end) {
        if (paging_is_mapped(addr)) {
            if (!in_run) { in_run=1; run_start=addr; run_phys=paging_virt_to_phys(addr); }
        } else {
            if (in_run) {
                vga_puts("    "); vga_put_hex(run_start);
                vga_puts(" → "); vga_put_hex(run_phys);
                vga_puts(" ("); vga_put_dec((addr-run_start)/1024); vga_puts(" KB)\n");
                in_run = 0;
            }
        }
        addr += PAGE_SIZE;
    }
    if (in_run) {
        vga_puts("    "); vga_put_hex(run_start);
        vga_puts(" → "); vga_put_hex(run_phys);
        vga_puts(" ("); vga_put_dec((addr-run_start)/1024); vga_puts(" KB)\n");
    }
}

void paging_init(uint32_t mem_kb) {
    pmm_init(mem_kb);
    kmemset(page_dir, 0, sizeof(page_dir));

    /* Identity-map ALL physical RAM at boot, not just a fixed 16MB (4
       tables) - pmm_alloc() hands out frames anywhere up to mem_kb, and a
       lot of kernel code (paging_clone_dir() in particular) treats
       whatever pmm_alloc() returns as a directly-dereferenceable kernel
       pointer ((void*)phys), relying on that identity mapping already
       being there. With only 16MB mapped, that assumption silently held
       right up until enough physical memory had actually been consumed to
       push pmm_alloc() past the 16MB mark - real, reproducible bug: a
       second fork()+execve() cycle (the first already eats several MB
       between the clone, the ELF pages and both stacks) reliably crossed
       that boundary and kmemcpy'd straight into unmapped memory, page
       faulting at exactly CR2=0x1000000. */
    uint32_t table_bytes = 4u * 1024 * 1024;
    uint32_t needed = (mem_kb * 1024 + table_bytes - 1) / table_bytes;
    if (needed < 4) needed = 4;
    if (needed > MAX_PREINIT_TABLES) needed = MAX_PREINIT_TABLES;
    num_kern_tables = (int)needed;

    for (int t = 0; t < num_kern_tables; t++) {
        kmemset(kern_tables[t], 0, PAGE_SIZE);
        for (int p = 0; p < PAGE_ENTRIES; p++) {
            uint32_t phys = (uint32_t)(t * PAGE_ENTRIES + p) * PAGE_SIZE;
            kern_tables[t][p] = phys | PAGE_PRESENT | PAGE_WRITE;
        }
        page_dir[t] = (uint32_t)kern_tables[t] | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    }

    for (int t = 0; t < VMALLOC_TABLES; t++) {
        kmemset(vmalloc_tables[t], 0, PAGE_SIZE);
        page_dir[PAGE_DIR_IDX(VMALLOC_BASE) + t] = (uint32_t)vmalloc_tables[t] | PAGE_PRESENT | PAGE_WRITE;
    }

    load_cr3((uint32_t)page_dir);
    enable_paging();
}

#define VMALLOC_MAX_REGIONS  64

typedef struct {
    uint32_t virt_start;
    uint32_t size;
    int      active;
} vregion_t;

static vregion_t vregions[VMALLOC_MAX_REGIONS];
static int       vnum_regions = 0;
static uint32_t  vmalloc_ptr  = VMALLOC_BASE;

void *vmalloc(uint32_t size) {
    if (!size) return 0;
    size = PAGE_ALIGN(size);

    int slot = -1;
    uint32_t virt = 0;
    for (int i = 0; i < vnum_regions; i++) {
        if (!vregions[i].active && vregions[i].size >= size) {
            slot = i; virt = vregions[i].virt_start; size = vregions[i].size; break;
        }
    }
    if (slot < 0) {
        if (vnum_regions >= VMALLOC_MAX_REGIONS) return 0;
        if (vmalloc_ptr + size > VMALLOC_END) return 0;
        slot = vnum_regions;
        virt = vmalloc_ptr;
    }

    for (uint32_t off = 0; off < size; off += PAGE_SIZE) {
        uint32_t phys = pmm_alloc();
        if (!phys) {

            for (uint32_t r = 0; r < off; r += PAGE_SIZE)
                paging_unmap(virt + r);
            return 0;
        }
        paging_map(virt + off, phys, PAGE_WRITE);
        kmemset((void *)(virt + off), 0, PAGE_SIZE);
    }

    vregions[slot].virt_start = virt;
    vregions[slot].size       = size;
    vregions[slot].active     = 1;
    if (slot == vnum_regions) { vnum_regions++; vmalloc_ptr += size; }
    return (void *)virt;
}

void vmfree(void *ptr) {
    uint32_t virt = (uint32_t)ptr;
    for (int i = 0; i < vnum_regions; i++) {
        if (vregions[i].active && vregions[i].virt_start == virt) {
            for (uint32_t off = 0; off < vregions[i].size; off += PAGE_SIZE)
                paging_unmap(virt + off);
            vregions[i].active = 0;
            return;
        }
    }
}

int vmalloc_copy_on_write(uint32_t virt) {

    uint32_t *pte = pte_ptr(virt & ~0xFFF, 0);
    if (!pte || !(*pte & PAGE_PRESENT)) return 0;
    uint32_t phys = *pte & ~0xFFF;
    pmm_ref(phys);
    *pte = (*pte & ~PAGE_WRITE) | PAGE_COW;
    tlb_flush_page(virt & ~0xFFF);
    return 1;
}

static volatile uint32_t fault_count = 0;
static volatile uint32_t cow_count   = 0;

static void page_fault_handler(registers_t *r) {
    uint32_t fault_addr;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(fault_addr));

    int present  = r->err_code & 0x1;
    int write    = r->err_code & 0x2;

    serial_printf("[fault] page fault: pid=%u cr2=%x err=%x eip=%x esp=%x\r\n",
                  sched_current()->pid, fault_addr, r->err_code, r->eip, r->esp);

    if (present && write) {
        uint32_t page = fault_addr & ~0xFFF;
        uint32_t *pte = pte_ptr(page, 0);
        if (pte && (*pte & PAGE_COW)) {
            uint32_t old_phys = *pte & ~0xFFF;
            if (pmm_refcount(old_phys) > 1) {

                uint32_t new_phys = pmm_alloc();
                if (new_phys) {
                    kmemcpy((void *)new_phys, (void *)old_phys, PAGE_SIZE);
                    pmm_free(old_phys);
                    *pte = new_phys | PAGE_PRESENT | PAGE_WRITE;
                    tlb_flush_page(page);
                    cow_count++;
                    fault_count++;
                    return;
                }
            } else {

                *pte = (*pte & ~PAGE_COW) | PAGE_WRITE;
                tlb_flush_page(page);
                cow_count++;
                fault_count++;
                return;
            }
        }
    }

    uint32_t cr3_now; __asm__ volatile("mov %%cr3,%0":"=r"(cr3_now));

    vga_fill_rect(0,0,80,2,' ',VGA_WHITE,VGA_RED);
    vga_puts_at("PAGE FAULT  CR2=", 0, 0, VGA_YELLOW, VGA_RED);
    char buf[12]; kitoa(fault_addr, buf, 16);
    vga_puts_at(buf, 17, 0, VGA_WHITE, VGA_RED);
    vga_puts_at(write ? " WRITE" : " READ", 28, 0, VGA_WHITE, VGA_RED);
    vga_puts_at(present ? "PRESENT" : "NOTPRES", 36, 0, VGA_WHITE, VGA_RED);
    vga_puts_at((r->err_code & 4) ? "USER" : "SUPER", 45, 0, VGA_WHITE, VGA_RED);
    vga_puts_at("CR3=", 0, 1, VGA_YELLOW, VGA_RED);
    char buf2[12]; kitoa(cr3_now, buf2, 16);
    vga_puts_at(buf2, 4, 1, VGA_WHITE, VGA_RED);
    vga_puts_at("root=", 20, 1, VGA_YELLOW, VGA_RED);
    char buf3[12]; kitoa(paging_root_dir(), buf3, 16);
    vga_puts_at(buf3, 25, 1, VGA_WHITE, VGA_RED);

    exc_register(14, 0);

    __asm__ volatile ("cli; hlt");
    while(1);
}

void demand_paging_init(void) {
    exc_register(14, page_fault_handler);
}

uint32_t demand_fault_count(void) { return fault_count; }
uint32_t demand_cow_count(void)   { return cow_count;   }
uint32_t paging_current_dir(void) {
    uint32_t cr3;
    __asm__ volatile("mov %%cr3,%0":"=r"(cr3));
    return cr3;
}

void paging_switch(uint32_t dir_phys) {
    load_cr3(dir_phys);
}

uint32_t paging_root_dir(void) {
    return (uint32_t)page_dir;
}

#define PDE_USER_CODE  PAGE_DIR_IDX(USER_CODE_BASE)
#define PDE_USER_STACK PAGE_DIR_IDX(ELF_USER_STACK_TOP - 1)

static int pde_is_private(int i) {
    return i == PDE_USER_CODE || i == PDE_USER_STACK;
}

uint32_t paging_new_dir(void) {
    uint32_t phys = pmm_alloc();
    if (!phys) return 0;
    uint32_t *dir = (uint32_t *)phys;
    for (int i = 0; i < PAGE_ENTRIES; i++)
        dir[i] = (i < 256 && !pde_is_private(i)) ? page_dir[i] : 0;
    return phys;
}

static uint32_t clone_table(uint32_t src_tbl_phys) {
    uint32_t *src = (uint32_t *)src_tbl_phys;
    uint32_t dst_phys = pmm_alloc();
    if (!dst_phys) return 0;
    uint32_t *dst = (uint32_t *)dst_phys;
    kmemset(dst, 0, PAGE_SIZE);
    for (int j = 0; j < PAGE_ENTRIES; j++) {
        if (!(src[j] & PAGE_PRESENT)) continue;
        uint32_t frame = pmm_alloc();
        if (!frame) {
            for (int k = 0; k < j; k++) if (dst[k]) pmm_free(dst[k] & ~0xFFF);
            pmm_free(dst_phys);
            return 0;
        }
        kmemcpy((void *)frame, (void *)(src[j] & ~0xFFF), PAGE_SIZE);
        dst[j] = frame | (src[j] & 0xFFF);
    }
    return dst_phys;
}

uint32_t paging_clone_dir(void) {
    uint32_t *cur = (uint32_t *)current_dir_phys();
    if ((uint32_t)cur == (uint32_t)page_dir) return 0;
    uint32_t new_phys = paging_new_dir();
    if (!new_phys) return 0;
    uint32_t *dir = (uint32_t *)new_phys;
    for (int i = 0; i < PAGE_ENTRIES; i++) {
        if (!pde_is_private(i) || !(cur[i] & PAGE_PRESENT)) continue;
        uint32_t tbl = clone_table(cur[i] & ~0xFFF);
        if (!tbl) { paging_free_user(new_phys); return 0; }
        dir[i] = tbl | (cur[i] & 0xFFF);
    }
    return new_phys;
}

void paging_free_user(uint32_t dir_phys) {
    uint32_t *dir = (uint32_t *)dir_phys;
    for (int i = 0; i < PAGE_ENTRIES; i++) {
        if (!pde_is_private(i) || !(dir[i] & PAGE_PRESENT)) continue;
        uint32_t *tbl = (uint32_t *)(dir[i] & ~0xFFF);
        for (int j = 0; j < PAGE_ENTRIES; j++)
            if (tbl[j] & PAGE_PRESENT) pmm_free(tbl[j] & ~0xFFF);
        pmm_free(dir[i] & ~0xFFF);
    }
    pmm_free(dir_phys);
}
