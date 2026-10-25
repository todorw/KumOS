#ifndef ELF_H
#define ELF_H

#include <stdint.h>

#define ELF_MAGIC     0x464C457F
#define ET_EXEC       2
#define EM_386        3
#define PT_LOAD       1
#define PF_X          0x1
#define PF_W          0x2
#define PF_R          0x4

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  class;
    uint8_t  data;
    uint8_t  version;
    uint8_t  os_abi;
    uint8_t  pad[8];
    uint16_t type;
    uint16_t machine;
    uint32_t version2;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} elf32_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
} elf32_phdr_t;

typedef struct {
    uint32_t entry;
    uint32_t load_base;
    uint32_t load_end;
    int      error;
} elf_load_result_t;

elf_load_result_t elf_load_mem(const void *buf, uint32_t bufsz);

elf_load_result_t elf_load_disk(const char *filename);

int elf_validate_disk(const char *filename);

#define ELF_MAX_ARGS    15
#define ELF_MAX_ARG_LEN 128

int      elf_run(const char *fname, int argc, const char *const *argv, int tty);
uint32_t elf_user_stack(int argc, const char *const *argv);
uint32_t elf_user_frame(uint32_t *kstack_top, uint32_t entry, uint32_t user_esp);

int elf_validate(const elf32_hdr_t *hdr);

void elf_print_info(const void *buf, uint32_t bufsz);

#endif
