/*
 * errno.h — POSIX errno.
 *
 * errno is an alias for __errno, a thread-local. The kernel loads the image's
 * PT_TLS (elf.c) and libc's startup installs the thread pointer with
 * arch_prctl(ARCH_SET_FS) before anything can reach this variable
 * (src/libc/src/crt1.c); the kernel returns -errno through the syscall ABI and
 * libc converts that back into a positive value and stores it here.
 */
#ifndef ERRNO_H
#define ERRNO_H

#include <uapi/errno.h>

/*
 * EOVERFLOW is missing from the shared header. It is spelled out here, behind
 * #ifndef, with the Linux value, so that libc can report an arithmetic overflow
 * in fread/fwrite's element-count product. src/include/uapi/errno.h is not
 * libc's to edit and its absence is posted to the board; when the shared header
 * grows the number, this falls away and the shared value wins.
 */
#ifndef EOVERFLOW
#define EOVERFLOW 75
#endif

extern __thread int __errno;

#define errno (__errno)

#endif /* ERRNO_H */