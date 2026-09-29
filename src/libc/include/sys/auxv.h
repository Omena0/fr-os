/*
 * sys/auxv.h — ELF auxiliary vector access.
 *
 * The kernel builds the auxv as an array of unsigned long pairs terminated by
 * AT_NULL and passes it on the stack at process entry. libc stashes a pointer
 * to it in __libc_auxv at _start time; this header exposes getauxval, which
 * walks that array on demand.
 */
#ifndef SYS_AUXV_H
#define SYS_AUXV_H

#include <uapi/syscall.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AT_SYSINFO_EHDR 33

long getauxval(long type);

#ifdef __cplusplus
}
#endif

#endif /* SYS_AUXV_H */