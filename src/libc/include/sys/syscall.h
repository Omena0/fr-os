/*
 * sys/syscall.h — libc's view of the syscall ABI.
 *
 * This is just the kernel ABI re-exported under the POSIX <sys/syscall.h>
 * name so that libc source files can include it without reaching for the
 * kernel's uapi path directly. The syscall numbers, struct layouts and
 * error convention all live in <uapi/syscall.h>; this header exists so the
 * libc side of the contract can be found by a reader that starts from libc.
 */
#ifndef SYS_SYSCALL_H
#define SYS_SYSCALL_H

#include <uapi/syscall.h>

#endif /* SYS_SYSCALL_H */