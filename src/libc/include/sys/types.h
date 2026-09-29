/*
 * sys/types.h — POSIX type aliases.
 *
 * These are the fixed-width aliases the kernel ABI uses, re-exposed under the
 * POSIX names so libc and user programs can talk about off_t, pid_t and
 * ssize_t without reaching for the kernel's internal types.
 */
#ifndef SYS_TYPES_H
#define SYS_TYPES_H

#include <stdint.h>
#include <stddef.h>

typedef int64_t  off_t;
typedef int32_t  pid_t;
typedef uint32_t mode_t;
typedef uint32_t useconds_t;
typedef int64_t  ssize_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;

#endif /* SYS_TYPES_H */