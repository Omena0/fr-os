/*
 * wait.c — bounded waiting and yielding.
 *
 * The deadline is always a TSC timestamp. See wait.h for why the TSC rather
 * than the calibrated monotonic clock: these helpers must work during early
 * boot, before ktime_init() and before the scheduler exists.
 */
#include <wait.h>
#include <io.h>
#include <types.h>
#include <stdbool.h>
#include <sched.h>
#include <task.h>
#include <cpu_features.h>

/* A CPU is always faster than this; it is a floor, not an estimate, so a wait
 * cannot be suspiciously short when the frequency was never measured. */
#define WAIT_FALLBACK_KHZ 1000000ULL      /* 1 GHz */
#define WAIT_POLL_PAUSE_US 50

u64 wait_us_to_tsc(u64 us)
{
    extern struct cpu_features cpu_features;
    u64 khz = cpu_features.tsc_khz ? cpu_features.tsc_khz : WAIT_FALLBACK_KHZ;

    /* khz cycles per millisecond == cycles per 1000 us. */
    return (us * khz) / 1000ULL;
}

bool wait_scheduler_ready(void)
{
    /*
     * sched_init() allocates this CPU's run queue and publishes it. Before
     * that, sched_sleep_ns() has no sleeper list to join and no task to
     * switch away from.
     */
    return sched_runqueues[this_cpu_id()] != NULL && current_task() != NULL;
}

void wait_spin_us(u64 us)
{
    u64 deadline = rdtsc() + wait_us_to_tsc(us);

    while ((s64)(rdtsc() - deadline) < 0)
        io_wait();
}

bool wait_poll(wait_pred_fn pred, void *ctx, u64 timeout_us)
{
    u64 deadline = rdtsc() + wait_us_to_tsc(timeout_us);

    for (;;) {
        if (pred(ctx))
            return true;
        if ((s64)(rdtsc() - deadline) >= 0)
            return false;
        io_wait();
    }
}

bool wait_poll_sleep(wait_pred_fn pred, void *ctx, u64 timeout_us)
{
    if (!wait_scheduler_ready())
        return wait_poll(pred, ctx, timeout_us);

    u64 deadline = rdtsc() + wait_us_to_tsc(timeout_us);

    for (;;) {
        if (pred(ctx))
            return true;

        u64 now = rdtsc();

        if ((s64)(now - deadline) >= 0)
            return false;

        u64 left_cycles = deadline - now;
        u64 khz = cpu_features.tsc_khz ? cpu_features.tsc_khz
                                       : WAIT_FALLBACK_KHZ;
        u64 left_us = (left_cycles * 1000ULL) / khz;

        /* Do not oversleep the deadline. */
        sched_sleep_ns((left_us < WAIT_POLL_PAUSE_US ? left_us
                                                     : WAIT_POLL_PAUSE_US)
                       * 1000ULL);
    }
}

void wait_sleep_us(u64 us)
{
    if (wait_scheduler_ready())
        sched_sleep_ns(us * 1000ULL);
    else
        wait_spin_us(us);
}