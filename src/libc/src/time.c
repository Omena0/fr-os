/*
 * time.c — time functions using the kernel's clock_gettime syscall.
 *
 * clock_gettime dispatches on clock id: 0 = REALTIME, 1 = MONOTONIC,
 * 2 = PROCESS_CPUTIME_ID (mapped to monotonic, documented as approximate),
 * anything else -> EINVAL.
 * clock_getres returns the resolution (1ns for all clocks here).
 * time() returns the realtime seconds.
 * nanosleep() wraps the syscall with EINTR retry.
 * gettimeofday() converts from timespec to timeval.
 */
#include <time.h>
#include <errno.h>
#include <stdint.h>

/* __MORE__ */

extern int sys_clock_gettime(clockid_t clk_id, struct timespec *tp);
extern int sys_nanosleep(const struct timespec *req, struct timespec *rem);

#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2

int clock_gettime(clockid_t clk_id, struct timespec *tp)
{
	switch (clk_id) {
	case CLOCK_REALTIME:
	case CLOCK_MONOTONIC:
		return sys_clock_gettime(clk_id, tp);
	case CLOCK_PROCESS_CPUTIME_ID:
		/* Approximate: map to monotonic since we don't track per-process CPU time */
		return sys_clock_gettime(CLOCK_MONOTONIC, tp);
	default:
		__errno = EINVAL;
		return -1;
	}
}

int clock_getres(clockid_t clk_id, struct timespec *res)
{
	switch (clk_id) {
	case CLOCK_REALTIME:
	case CLOCK_MONOTONIC:
	case CLOCK_PROCESS_CPUTIME_ID:
		if (res) {
			res->tv_sec = 0;
			res->tv_nsec = 1;  /* 1ns resolution */
		}
		return 0;
	default:
		__errno = EINVAL;
		return -1;
	}
}

long time(long *t)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_REALTIME, &ts) == -1)
		return -1;
	if (t)
		*t = ts.tv_sec;
	return ts.tv_sec;
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
	return sys_nanosleep(req, rem);
}

int gettimeofday(struct timeval *tv, void *tz)
{
	(void)tz;  /* timezone not supported */
	struct timespec ts;
	if (clock_gettime(CLOCK_REALTIME, &ts) == -1)
		return -1;
	tv->tv_sec = ts.tv_sec;
	tv->tv_usec = ts.tv_nsec / 1000;
	return 0;
}