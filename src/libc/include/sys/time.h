/*
 * sys/time.h — time types and gettimeofday.
 *
 * The timespec and timeval structs are defined in the kernel ABI; this header
 * re-exports them under the POSIX names and adds the gettimeofday wrapper.
 */
#ifndef SYS_TIME_H
#define SYS_TIME_H

#include <uapi/syscall.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

struct timeval;
int gettimeofday(struct timeval *tv, void *tz);

#ifdef __cplusplus
}
#endif

#endif /* SYS_TIME_H */