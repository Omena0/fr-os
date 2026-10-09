/*
 * time.h — time types and the time functions libc provides.
 *
 * struct timespec and struct timeval are NOT defined here. They already exist
 * in <uapi/syscall.h>, which is the kernel's ABI header, and redefining them
 * here is not a harmless duplicate: the two definitions are the wire format
 * between user space and the kernel, so a struct that compiles in user space
 * but is a different size on the kernel side is a silent memory corruption
 * rather than a build error. One definition, in the header the kernel and libc
 * both include, is the only way to keep them provably identical.
 *
 * tv_sec and the sub-second field are both 64-bit. glibc uses long for tv_sec
 * and long for tv_nsec on 64-bit targets, which is the same thing, but the
 * kernel ABI pins them to fixed-width types so the layout does not depend on
 * the compiler's data model.
 */
#ifndef TIME_H
#define TIME_H

#include <stddef.h>
#include <stdint.h>

#include <uapi/syscall.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLOCKS_PER_SEC 1000000000UL

#define TIME_UTC 0

typedef uint32_t clockid_t;

/*
 * Clock ids. The values are the kernel's, not glibc's: they are part of the
 * syscall ABI, and using glibc's numbering would silently select the wrong
 * clock rather than fail.
 *
 * Only the first three are served. The kernel runs one counter and ignores the
 * clock id, so CLOCK_THREAD_CPUTIME_ID (no per-thread accounting),
 * CLOCK_MONOTONIC_RAW (no separate raw counter) and CLOCK_BOOTTIME (which would
 * have to exclude suspend time, and there is no suspend) have no honest answer
 * here: clock_gettime rejects them with EINVAL rather than returning the
 * monotonic clock under a name that promises something else.
 */
#define CLOCK_REALTIME        0
#define CLOCK_MONOTONIC        1
#define CLOCK_PROCESS_CPUTIME_ID    2
#define CLOCK_THREAD_CPUTIME_ID    3
#define CLOCK_MONOTONIC_RAW    4
#define CLOCK_BOOTTIME        7

extern int  clock_gettime(clockid_t clk_id, struct timespec *tp);
extern int  clock_getres(clockid_t clk_id, struct timespec *res);
extern long time(long *t);
extern int  nanosleep(const struct timespec *req, struct timespec *rem);
extern int  gettimeofday(struct timeval *tv, void *tz);

/* `struct timespec` has no member functions, so a C++ caller including only
 * this header would not get them; the include above is enough. */
#ifdef __cplusplus
}
#endif

#endif /* TIME_H */
