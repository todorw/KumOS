#include "pkg.h"
#include "kstring.h"
#include <stdint.h>

#define PKG_DECL(n) \
    extern uint8_t _binary_##n##_elf_start[]; \
    extern uint8_t _binary_##n##_elf_end[];

PKG_DECL(hello)
PKG_DECL(counter)
PKG_DECL(cat)
PKG_DECL(sysinfo)
PKG_DECL(kush)
PKG_DECL(ed)
PKG_DECL(vi)
PKG_DECL(top)
PKG_DECL(crond)
PKG_DECL(http)
PKG_DECL(grep)
PKG_DECL(tar)
PKG_DECL(wc)
PKG_DECL(sort)
PKG_DECL(uniq)
PKG_DECL(awk)
PKG_DECL(echo)

typedef struct {
    const char *cmd;
    const char *fname;
    const char *desc;
    uint8_t *start;
    uint8_t *end;
} pkg_entry_t;

#define PKG_ENTRY(n, f, d) { #n, f, d, _binary_##n##_elf_start, _binary_##n##_elf_end }

static const pkg_entry_t pkgs[] = {
    PKG_ENTRY(hello,   "HELLO.ELF",   "Hello world demo"),
    PKG_ENTRY(counter, "COUNTER.ELF", "Countdown demo"),
    PKG_ENTRY(cat,     "CAT.ELF",     "Print file (ring-3)"),
    PKG_ENTRY(sysinfo, "SYSINFO.ELF", "System info"),
    PKG_ENTRY(kush,    "KUSH.ELF",    "Userspace shell"),
    PKG_ENTRY(ed,      "ED.ELF",      "Line editor"),
    PKG_ENTRY(vi,      "VI.ELF",      "Visual editor"),
    PKG_ENTRY(top,     "TOP.ELF",     "Process monitor"),
    PKG_ENTRY(crond,   "CROND.ELF",   "Task scheduler"),
    PKG_ENTRY(http,    "HTTP.ELF",    "HTTP client"),
    PKG_ENTRY(grep,    "GREP.ELF",    "Pattern search"),
    PKG_ENTRY(tar,     "TAR.ELF",     "Archiver"),
    PKG_ENTRY(wc,      "WC.ELF",      "Word count"),
    PKG_ENTRY(sort,    "SORT.ELF",    "Sort lines"),
    PKG_ENTRY(uniq,    "UNIQ.ELF",    "Dedup lines"),
    PKG_ENTRY(awk,     "AWK.ELF",     "Text processor"),
    PKG_ENTRY(echo,    "ECHO.ELF",    "Read + echo a line"),
};

#define PKG_COUNT ((int)(sizeof(pkgs)/sizeof(pkgs[0])))

static int pkg_find(const char *name) {
    for (int i=0;i<PKG_COUNT;i++)
        if (kstrcmp(pkgs[i].cmd, name)==0) return i;
    return -1;
}

int pkg_count(void) { return PKG_COUNT; }

const char *pkg_name_at(int i)  { return (i>=0 && i<PKG_COUNT) ? pkgs[i].cmd   : ""; }
const char *pkg_fname_at(int i) { return (i>=0 && i<PKG_COUNT) ? pkgs[i].fname : ""; }
const char *pkg_desc_at(int i)  { return (i>=0 && i<PKG_COUNT) ? pkgs[i].desc  : ""; }

int pkg_is_installed(int i) {
    return i >= 0 && i < PKG_COUNT;
}

int pkg_install(const char *name) {
    return pkg_find(name) < 0 ? -1 : 0;
}

int pkg_remove(const char *name) {
    return pkg_find(name) < 0 ? -1 : -3;
}

int pkg_find_index(const char *name) { return pkg_find(name); }

int pkg_blob_at(int i, const uint8_t **start, const uint8_t **end) {
    if (i < 0 || i >= PKG_COUNT) return -1;
    *start = pkgs[i].start;
    *end   = pkgs[i].end;
    return 0;
}

static int fname_eq(const char *a, const char *b) {
    for (;; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
}

int pkg_blob_by_fname(const char *fname, const uint8_t **start, const uint8_t **end) {
    for (int i = 0; i < PKG_COUNT; i++) {
        if (fname_eq(pkgs[i].fname, fname)) {
            *start = pkgs[i].start;
            *end   = pkgs[i].end;
            return 0;
        }
    }
    return -1;
}
