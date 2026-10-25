#include "kumos_libc.h"

int main(void) {
    printf("\n");
    printf("  +------------------------------------------+\n");
    printf("  |   Hello from KumOS userspace!            |\n");
    printf("  |   Real ELF binary, ring 3, libc printf   |\n");
    printf("  +------------------------------------------+\n\n");
    printf("  PID:    %d\n", getpid());
    printf("  Uptime: %u seconds\n\n", uptime());
    time_t t;
    if (gettime(&t) == 0)
        printf("  Date:   %04d-%02d-%02d  %02d:%02d:%02d\n\n",
               t.year, t.month, t.day, t.hour, t.minute, t.second);
    printf("  Press any key to exit...\n");
    getkey();
    return 0;
}
