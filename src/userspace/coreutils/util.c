#include "util.h"
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

void cu_perror(const char *prog, const char *what)
{
    if (what)
        fprintf(stderr, "%s: %s: %s\n", prog, what, strerror(errno));
    else
        fprintf(stderr, "%s: %s\n", prog, strerror(errno));
}

int cu_parse_int(const char *s, int def)
{
    int n = 0;
    int any = 0;

    if (!s)
        return def;
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (*s - '0');
        any = 1;
        s++;
    }
    return any ? n : def;
}

int cu_copy_fd(int in, int out)
{
    char buf[4096];

    for (;;) {
        ssize_t n = read(in, buf, sizeof(buf));
        size_t done = 0;

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return 0;
        while (done < (size_t)n) {
            ssize_t w = write(out, buf + done, (size_t)n - done);

            if (w < 0) {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            done += (size_t)w;
        }
    }
}

ssize_t cu_read_line(int fd, char *buf, size_t cap)
{
    size_t len = 0;

    if (cap == 0)
        return 0;
    while (len + 1 < cap) {
        char c;
        ssize_t n = read(fd, &c, 1);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            break;
        buf[len++] = c;
        if (c == '\n')
            break;
    }
    buf[len] = '\0';
    return (ssize_t)len;
}
