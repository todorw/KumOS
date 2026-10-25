#include "kumos_libc.h"

int main(void) {
    printf("\n  [cat] README.TXT from FAT12 disk:\n\n");
    int fd = open("README.TXT");
    if (fd < 0) { puts("  File not found."); return 1; }
    char buf[512];
    int n = fread(fd, buf, 511);
    if (n > 0) { buf[n] = 0; fputs(buf); }
    close(fd);
    printf("\n\n  (%d bytes read)\n\n", n);
    return 0;
}
