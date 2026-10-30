#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(void)
{
    struct timespec a, b;
    long ns;
    void *p;

    clock_gettime(CLOCK_MONOTONIC, &a);
    p = malloc(256 * 1024);
    if (!p) {
        printf("malloc failed\n");
        return 1;
    }
    memset(p, 0x5a, 256 * 1024);
    free(p);
    clock_gettime(CLOCK_MONOTONIC, &b);
    ns = (b.tv_sec - a.tv_sec) * 1000000000L + (b.tv_nsec - a.tv_nsec);
    printf("malloc+free 256 KiB: %ld us\n", ns / 1000);
    return 0;
}
