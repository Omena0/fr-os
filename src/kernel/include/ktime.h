/*
 * ktime.h — kernel timekeeping.
 *
 * Two clocks, one stops watch:
 *   ktime_mono_ns()      monotonic since ktime_init(), TSC-backed when the
 *                        CPU frequency was measured, PIT ticks otherwise.
 *   ktime_realtime_ns()  Unix time: the CMOS RTC read once at init plus the
 *                        monotonic delta since then.
 *
 * The RTC is the only wall-clock source a PC guest has; QEMU sets it from the
 * host at start-up (UTC by default). The TSC gives the resolution the 100 Hz
 * PIT cannot, and it is calibrated against the PIT, so the oscillator in the
 * chain is the PIT's 1.193182 MHz crystal.
 */
#ifndef KTIME_H
#define KTIME_H

#include <types.h>

/* Capture the RTC and the clocksource origin. Call once, after the timer is
 * running and before user space needs the time. */
void ktime_init(void);

/* Nanoseconds since ktime_init(). */
u64 ktime_mono_ns(void);

/* Seconds since the Unix epoch, with nanosecond resolution. Falls back to
 * uptime if the RTC could not be read. */
u64 ktime_realtime_ns(void);

#endif
