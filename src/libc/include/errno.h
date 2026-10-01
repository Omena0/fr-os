/*
 * errno.h — POSIX errno.
 *
 * errno is an alias for __errno, an ordinary global. It is deliberately *not*
 * declared __thread: this kernel neither allocates a TLS block (elf.c ignores
 * PT_TLS) nor provides any way for a process to set the FS base (there is no
 * arch_prctl in the syscall ABI), so an %fs-relative access reads address 0.
 * See src/libc/src/errno.c. The kernel returns -errno through the syscall ABI;
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

extern int __errno;

#define errno (__errno)

#endif /* ERRNO_H */