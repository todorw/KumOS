#include "kumos_libc.h"

int main(void) {
    printf("\n");
    printf("  ========== KumOS System Info ==========\n\n");
    printf("  OS:      KumOS v%s\n", KUMOS_VERSION);
    printf("  Arch:    x86 (i686)\n");
    printf("  PID:     %d\n", getpid());
    printf("  Uptime:  %u seconds\n", uptime());

    time_t t;
    if (gettime(&t) == 0)
        printf("  Time:    %04d-%02d-%02d %02d:%02d:%02d\n",
               t.year, t.month, t.day, t.hour, t.minute, t.second);

    printf("\n  Listing disk files:\n");
    char dirbuf[512];
    /* SYS_LISTDIR wires to sc_vfs_readdir(path, buf, sz) - it needs a
       path as the first argument, not just (buf, sz). Was missing it
       entirely, shifting every argument by one: the real path string
       address ended up in the "size" slot and 512 ended up being read
       as the OUTPUT BUFFER POINTER (i.e. writing directory entries to
       literal virtual address 0x200), corrupting unrelated memory. */
    int n = _syscall(SYS_LISTDIR, (int)"/disk", (int)dirbuf, 512);
    if (n > 0) {
        dirbuf[n] = 0;

        char *p = dirbuf;
        while (*p) {
            char *nl = strchr(p, '\n');
            if (nl) *nl = 0;
            printf("    %s\n", p);
            p = nl ? nl+1 : p + strlen(p);
        }
    }
    printf("\n");
    return 0;
}