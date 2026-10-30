#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char **argv)
{
    struct timespec req, rem;

    if (argc < 2) {
        fprintf(stderr, "usage: sleep <seconds>\n");
        return 1;
    }
    req.tv_sec = atoi(argv[1]);
    req.tv_nsec = 0;
    if (nanosleep(&req, &rem) < 0) {
        perror("sleep");
        return 1;
    }
    return 0;
}
