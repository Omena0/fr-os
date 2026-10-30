#include "util.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int head_fd(int fd, int lines)
{
    char line[1024];

    while (lines-- > 0) {
        ssize_t n = cu_read_line(fd, line, sizeof(line));

        if (n <= 0)
            return n < 0 ? -1 : 0;
        fputs(line, stdout);
    }
    return 0;
}

int main(int argc, char **argv)
{
    int lines = 10;
    int i = 1;
    int rc = 0;

    if (i < argc && strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
        lines = cu_parse_int(argv[i + 1], 10);
        i += 2;
    }
    if (i >= argc)
        return head_fd(0, lines) < 0 ? 1 : 0;
    for (; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);

        if (fd < 0) {
            cu_perror("head", argv[i]);
            rc = 1;
            continue;
        }
        if (head_fd(fd, lines) < 0) {
            cu_perror("head", argv[i]);
            rc = 1;
        }
        close(fd);
    }
    return rc;
}
