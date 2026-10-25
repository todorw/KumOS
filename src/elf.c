
#include "elf.h"
#include "paging.h"
#include "fat12.h"
#include "fs.h"
#include "vga.h"
#include "kstring.h"
#include "kmalloc.h"
#include "sched.h"
#include "gdt.h"
#include "pkg.h"
#include <stdint.h>

#define USER_CS  0x1B
#define USER_DS  0x23

int elf_validate(const elf32_hdr_t *hdr) {
    if (hdr->magic   != ELF_MAGIC) return -1;
    if (hdr->class   != 1)         return -2;
    if (hdr->machine != EM_386)    return -2;
    if (hdr->type    != ET_EXEC)   return -2;
    if (hdr->phnum   == 0)         return -2;
    return 0;
}

elf_load_result_t elf_load_mem(const void *buf, uint32_t bufsz) {
    elf_load_result_t res = {0, 0, 0, 0};
    const uint8_t *data = (const uint8_t *)buf;

    if (bufsz < sizeof(elf32_hdr_t)) { res.error = -1; return res; }

    const elf32_hdr_t *hdr = (const elf32_hdr_t *)data;
    if (elf_validate(hdr) < 0) { res.error = -2; return res; }

    res.entry     = hdr->entry;
    res.load_base = 0xFFFFFFFF;
    res.load_end  = 0;

    for (int i = 0; i < hdr->phnum; i++) {
        uint32_t phoff = hdr->phoff + i * hdr->phentsize;
        if (phoff + sizeof(elf32_phdr_t) > bufsz) continue;

        const elf32_phdr_t *ph = (const elf32_phdr_t *)(data + phoff);
        if (ph->type != PT_LOAD || ph->memsz == 0) continue;

        uint32_t seg_end = ph->vaddr + ph->memsz;
        if (ph->vaddr < USER_CODE_BASE || seg_end > USER_CODE_END || seg_end < ph->vaddr ||
            ph->filesz > ph->memsz || ph->offset > bufsz || ph->filesz > bufsz - ph->offset) {
            res.error = -2; return res;
        }

        uint32_t vstart = ph->vaddr & ~0xFFF;
        uint32_t vend   = PAGE_ALIGN(seg_end);
        for (uint32_t va = vstart; va < vend; va += PAGE_SIZE) {
            if (!paging_is_mapped(va)) {
                uint32_t phys = pmm_alloc();
                if (!phys) { res.error = -3; return res; }
                kmemset((void *)phys, 0, PAGE_SIZE);
                paging_map(va, phys, PAGE_USER | PAGE_WRITE);
            } else {
                paging_map(va, paging_virt_to_phys(va) & ~0xFFF, PAGE_USER | PAGE_WRITE);
            }
        }

        if (ph->filesz) kmemcpy((void *)ph->vaddr, data + ph->offset, ph->filesz);
        if (ph->memsz > ph->filesz)
            kmemset((void *)(ph->vaddr + ph->filesz), 0, ph->memsz - ph->filesz);

        if (ph->vaddr < res.load_base) res.load_base = ph->vaddr;
        if (seg_end > res.load_end) res.load_end = seg_end;
    }

    if (res.load_base == 0xFFFFFFFF) { res.error = -2; return res; }
    if (res.entry < res.load_base || res.entry >= res.load_end) { res.error = -2; return res; }
    return res;
}

#define ELF_MAX_SIZE (128 * 1024)
static uint8_t elf_load_buf[ELF_MAX_SIZE];

static int elf_fetch(const char *filename, const uint8_t **out, uint32_t *len) {
    const uint8_t *s, *e;
    if (pkg_blob_by_fname(filename, &s, &e) == 0) {
        *out = s; *len = (uint32_t)(e - s);
        return 0;
    }
    int n = fat12_mounted() ? fat12_read(filename, elf_load_buf, ELF_MAX_SIZE) : -1;
    if (n <= 0) {
        fs_file_t *mf = fs_find(filename);
        if (!mf) return -1;
        n = (int)mf->size;
        if (n > ELF_MAX_SIZE) n = ELF_MAX_SIZE;
        kmemcpy(elf_load_buf, mf->data, (uint32_t)n);
    }
    *out = elf_load_buf; *len = (uint32_t)n;
    return 0;
}

elf_load_result_t elf_load_disk(const char *filename) {
    elf_load_result_t res = {0,0,0,-4};
    const uint8_t *img; uint32_t len;
    if (elf_fetch(filename, &img, &len) != 0) return res;
    return elf_load_mem(img, len);
}

int elf_validate_disk(const char *filename) {
    const uint8_t *img; uint32_t len;
    if (elf_fetch(filename, &img, &len) != 0) return -1;
    if (len < sizeof(elf32_hdr_t)) return -1;
    return elf_validate((const elf32_hdr_t *)img);
}

uint32_t elf_user_stack(int argc, const char *const *argv) {
    uint32_t top  = ELF_USER_STACK_TOP;
    uint32_t base = top - ELF_USER_STACK_SIZE;

    paging_unmap_range(base, top);
    for (uint32_t va = base; va < top; va += PAGE_SIZE) {
        uint32_t phys = pmm_alloc();
        if (!phys) return 0;
        kmemset((void *)phys, 0, PAGE_SIZE);
        paging_map(va, phys, PAGE_WRITE | PAGE_USER);
    }

    if (argc < 0) argc = 0;
    if (argc > ELF_MAX_ARGS) argc = ELF_MAX_ARGS;
    uint32_t ptrs[ELF_MAX_ARGS + 1];
    uint32_t sp = top;
    for (int i = argc - 1; i >= 0; i--) {
        uint32_t len = (uint32_t)kstrlen(argv[i]) + 1;
        if (len > ELF_MAX_ARG_LEN) len = ELF_MAX_ARG_LEN;
        sp -= len;
        kmemcpy((void *)sp, argv[i], len - 1);
        *(char *)(sp + len - 1) = 0;
        ptrs[i] = sp;
    }
    ptrs[argc] = 0;
    sp &= ~3u;
    for (int i = argc; i >= 0; i--) { sp -= 4; *(uint32_t *)sp = ptrs[i]; }
    uint32_t argv_addr = sp;
    sp -= 4; *(uint32_t *)sp = argv_addr;
    sp -= 4; *(uint32_t *)sp = (uint32_t)argc;
    sp -= 4; *(uint32_t *)sp = 0;
    return sp;
}

uint32_t elf_user_frame(uint32_t *kstack_top, uint32_t entry, uint32_t user_esp) {
    extern void user_entry_trampoline(void);
    uint32_t *sp = kstack_top;
    *--sp = USER_DS;
    *--sp = user_esp;
    *--sp = 0x202;
    *--sp = USER_CS;
    *--sp = entry;
    for (int i = 0; i < 8; i++) *--sp = 0;
    *--sp = USER_DS;
    *--sp = (uint32_t)user_entry_trampoline;
    *--sp = 0x202;
    for (int i = 0; i < 4; i++) *--sp = 0;
    return (uint32_t)sp;
}

static void elf_placeholder(void) {
    for (;;) __asm__ volatile ("hlt");
}

static inline uint32_t elf_irq_save(void) {
    uint32_t f;
    __asm__ volatile ("pushfl; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void elf_irq_restore(uint32_t f) {
    __asm__ volatile ("push %0; popfl" :: "r"(f) : "memory");
}

int elf_run(const char *fname, int argc, const char *const *argv, int tty) {
    uint32_t fl = elf_irq_save();
    int pid = -1;
    uint32_t dir = 0, old = paging_current_dir();
    uint32_t *kstack = 0;

    const uint8_t *img; uint32_t len;
    if (elf_fetch(fname, &img, &len) != 0) goto out;
    dir = paging_new_dir();
    if (!dir) goto out;
    kstack = kmalloc(SCHED_STACK_SIZE);
    if (!kstack) goto out;

    paging_switch(dir);
    elf_load_result_t r = elf_load_mem(img, len);
    uint32_t user_esp = r.error == 0 ? elf_user_stack(argc, argv) : 0;
    paging_switch(old);
    if (!user_esp) goto out;

    const char *name = argc > 0 ? argv[0] : fname;
    pid = sched_spawn(name, elf_placeholder, 0);
    if (pid < 0) goto out;
    task_t *t = sched_get_task(pid);
    kfree(t->stack);
    t->stack         = kstack;
    t->stack_size    = SCHED_STACK_SIZE;
    t->esp           = elf_user_frame((uint32_t *)((uint8_t *)kstack + SCHED_STACK_SIZE), r.entry, user_esp);
    t->page_dir_phys = dir;
    if (tty >= 0) t->tty = tty;
    kstack = 0;
    dir = 0;
out:
    if (kstack) kfree(kstack);
    if (dir) paging_free_user(dir);
    elf_irq_restore(fl);
    return pid;
}

void elf_print_info(const void *buf, uint32_t bufsz) {
    if (bufsz < sizeof(elf32_hdr_t)) {
        vga_puts("  Buffer too small for ELF header\n"); return;
    }
    const elf32_hdr_t *hdr = (const elf32_hdr_t *)buf;
    if (elf_validate(hdr) < 0) {
        vga_puts("  Not a valid x86 ELF32 executable\n"); return;
    }
    const uint8_t *data = (const uint8_t *)buf;

    vga_set_color(VGA_YELLOW, VGA_BLACK);
    vga_puts("\n  === ELF Info ===\n\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_puts("  Type:     ELF32 x86 Executable\n");
    vga_puts("  Entry:    0x"); vga_put_hex(hdr->entry); vga_putchar('\n');
    vga_puts("  Segments: "); vga_put_dec(hdr->phnum); vga_putchar('\n');

    for (int i = 0; i < hdr->phnum; i++) {
        uint32_t off = hdr->phoff + i * hdr->phentsize;
        if (off + sizeof(elf32_phdr_t) > bufsz) break;
        const elf32_phdr_t *ph = (const elf32_phdr_t *)(data + off);
        if (ph->type != PT_LOAD) continue;
        vga_puts("  LOAD  vaddr=0x"); vga_put_hex(ph->vaddr);
        vga_puts("  filesz="); vga_put_dec(ph->filesz);
        vga_puts("  memsz=");  vga_put_dec(ph->memsz);
        vga_puts("  flags=");
        if (ph->flags & PF_R) vga_putchar('R');
        if (ph->flags & PF_W) vga_putchar('W');
        if (ph->flags & PF_X) vga_putchar('X');
        vga_putchar('\n');
    }
    vga_putchar('\n');
}