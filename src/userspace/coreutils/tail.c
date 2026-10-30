#include "util.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int tail_fd(int fd, int lines)
{
    size_t cap = 65536, len = 0;
    char *buf = malloc(cap);

    if (!buf)
        return -1;
    for (;;) {
        ssize_t n = read(fd, buf + len, cap - len);

        if (n < 0) {
            free(buf);
            return -1;
        }
        if (n == 0)
            break;
        len += (size_t)n;
        if (len == cap) {
            char *nb = realloc(buf, cap * 2);

            if (!nb) {
                free(buf);
                return -1;
            }
            buf = nb;
            cap *= 2;
        }
    }

    size_t start = len;
    for (int seen = 0; start > 0; ) {
        start--;
        if (buf[start] == '\n' && start + 1 < len && ++seen >= lines) {
            start++;
            break;
        }
    }
    if (start < len)
        fwrite(buf + start, 1, len - start, stdout);
    free(buf);
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
        return tail_fd(0, lines) < 0 ? 1 : 0;
    for (; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);

        if (fd < 0) {
            cu_perror("tail", argv[i]);
            rc = 1;
            continue;
        }
        if (tail_fd(fd, lines) < 0) {
            cu_perror("tail", argv[i]);
            rc = 1;
        }
        close(fd);
    }
    return rc;
}
