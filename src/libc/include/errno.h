/*
 * errno.h — POSIX errno.
 *
 * errno is a thread-local int, not a plain global, so a signal handler or a
 * nested syscall failure in one thread cannot clobber another thread's error
 * state. The kernel returns -errno through the syscall ABI; libc converts that
 * back into a positive value and stores it here.
 */
#ifndef ERRNO_H
#define ERRNO_H

#include <uapi/errno.h>

extern __thread int __errno;

#define errno (__errno)

#endif /* ERRNO_H */