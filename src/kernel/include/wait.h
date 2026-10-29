/*
 * wait.h — bounded waiting, in one place.
 *
 * Three different kinds of wait appear in this kernel and they were each
 * hand-rolled before this header existed:
 *
 *   - polling a device register with a timeout, before the scheduler runs;
 *   - polling with a timeout where other work could proceed meanwhile;
 *   - sleeping, which means "give the CPU to someone else".
 *
 * All three need a deadline, and a deadline must be derived from a clock, not
 * from an iteration count. A loop count is not a timeout: it is a guess about
 * how fast the loop runs, and under emulation a port access can be thousands of
 * times slower than on the host, so the same count is a 1 ms wait on hardware
 * and a 30 s hang under TCG (the 8042 init hit exactly this).
 *
 * Timestamps here are TSC cycles. The TSC is used directly rather than
 * ktime_mono_ns() because these waits run during early boot, before
 * ktime_init() has captured its base, and because the only operation needed is
 * "has this much elapsed", which a cycle counter answers without calibration
 * when the CPU frequency has not been measured yet.
 */
#ifndef WAIT_H
#define WAIT_H

#include <stdbool.h>
#include <types.h>

/* A poll predicate: true means the wait is over. */
typedef bool (*wait_pred_fn)(void *ctx);

/* TSC cycles for `us` microseconds, using the measured frequency when there is
 * one and a conservative fallback when there is not. */
u64 wait_us_to_tsc(u64 us);

/* Busy-wait for `us` microseconds. Valid before the scheduler exists. */
void wait_spin_us(u64 us);

/*
 * Poll `pred(ctx)` until it returns true or `timeout_us` elapses. Busy-waits
 * between polls with a short pause so a fast device does not make the loop
 * hammer port I/O.
 *
 * Returns true if the predicate became true, false on timeout.
 */
bool wait_poll(wait_pred_fn pred, void *ctx, u64 timeout_us);

/*
 * Poll like wait_poll(), but sleep between polls so the CPU runs something
 * else. Intended for a wait long enough that spinning wastes a quantum.
 *
 * Falls back to busy-polling when no scheduler is running yet (early boot), so
 * it is safe to call from either context. Returns true if the predicate became
 * true, false on timeout.
 */
bool wait_poll_sleep(wait_pred_fn pred, void *ctx, u64 timeout_us);

/*
 * Sleep for `us` microseconds, yielding to the scheduler when there is one.
 * Before the scheduler exists this is a bounded busy-wait: blocking there
 * would deadlock, because the mechanism that would wake the caller has not
 * been created yet.
 */
void wait_sleep_us(u64 us);

/*
 * True once a task and a run queue exist on this CPU, i.e. sleeping will not
 * deadlock and will actually schedule.
 */
bool wait_scheduler_ready(void);

#endif /* WAIT_H */