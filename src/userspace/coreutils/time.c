#include <stdio.h>
#include <time.h>

int main(void)
{
    struct timespec mono, real;

    if (clock_gettime(CLOCK_MONOTONIC, &mono) < 0 ||
        clock_gettime(CLOCK_REALTIME, &real) < 0) {
        perror("time");
        return 1;
    }
    printf("realtime:   %ld.%09ld\n", (long)real.tv_sec, (long)real.tv_nsec);
    printf("monotonic:  %ld.%09ld\n", (long)mono.tv_sec, (long)mono.tv_nsec);
    return 0;
}
