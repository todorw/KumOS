#include "kumos_libc.h"

int main(void) {
    printf("\n  Counting 0-9 with 300ms sleep (libc, ring 3):\n  ");
    for (int i = 0; i <= 9; i++) {
        printf("%d ", i);
        sleep(300);
    }
    printf("\n  Done! Uptime: %us\n\n", uptime());
    return 0;
}
