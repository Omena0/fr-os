#include <stdio.h>
#include <time.h>

int main(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
        perror("uptime");
        return 1;
    }
    printf("up %ld s\n", (long)ts.tv_sec);
    return 0;
}
