/* Helpers shared by the coreutils programs in this directory. */
#ifndef COREUTIL_UTIL_H
#define COREUTIL_UTIL_H

#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

/* Print "<prog>: [what: ]strerror(errno)". */
void cu_perror(const char *prog, const char *what);
int cu_parse_int(const char *s, int def);
/* Copy fd in to fd out until EOF. Returns 0 or -1. */
int cu_copy_fd(int in, int out);
/* Read one line (including '\n') into buf. Returns bytes, 0 at EOF, -1 error. */
ssize_t cu_read_line(int fd, char *buf, size_t cap);

#endif
