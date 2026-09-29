# MLFQ Aging

## Problem: Starvation

Without aging, a continuous stream of high-priority threads can starve lower-level threads indefinitely. A CPU-bound process demoted to level 7 may never run if levels 0–6 are always non-empty.

## Solution: Periodic Priority Boost

The aging system periodically boosts all starved threads back to a higher MLFQ level. This is a bounded starvation guarantee: no thread can be starved for longer than the aging interval.

## Aging Algorithm

The aging daemon runs as a per-CPU kernel thread that wakes every **aging interval** (default: 500 ms, configurable at compile time via `SCHED_AGING_INTERVAL_MS`).

On each wakeup, for each MLFQ level 1–7 on this CPU:

1. Walk the thread list at this level.
2. For each thread: increment `starvation_ticks`.
3. If `starvation_ticks ≥ AGING_THRESHOLD` (default: 10 aging intervals = 5 seconds):
   - Move the thread to level 0 (or its `nice`-adjusted base level).
   - Reset `starvation_ticks` to 0.
   - Reset `level_cpu_time` to 0.
   - Log: `klog_debug("aging: boosted PID %d from level %d to 0", t->pid, old_level)`.

This is equivalent to the "boost" step in the classical MLFQ algorithm.

## Starvation Accounting

Each `struct task` contains:

```c
uint32_t starvation_ticks;   // incremented each aging interval while not running
uint64_t last_run_ns;        // TSC timestamp of last time this thread ran
```

A thread is considered starved if `(now - last_run_ns) > AGING_STARVATION_NS`.

## Interaction with CPU Accounting

After a boost, the thread's `level_cpu_time` is reset to zero. This ensures that a thread that was starved and boosted gets a full quota at its new level before being re-demoted.

## Interaction with I/O Boost

I/O wakeup boost (see [mlfq-cpu-accounting.md](mlfq-cpu-accounting.md)) and aging boost are independent mechanisms. A thread can receive both in the same scheduling cycle. In that case, the more favorable level (lower number) wins.

## Aging Daemon Implementation

The aging daemon runs as a per-CPU idle-priority kernel thread (`kthread`). It is woken by the LAPIC timer interrupt once per aging interval. It holds the per-CPU run queue lock only for the duration of the list walk and mutation. Other scheduling activity is not blocked for longer than one aging pass.

## Tuning

| Parameter | Default | Description |
|---|---|---|
| `SCHED_AGING_INTERVAL_MS` | 500 | How often the aging daemon runs |
| `SCHED_AGING_THRESHOLD` | 10 | Aging intervals before a boost |
| `SCHED_AGING_BOOST_LEVEL` | 0 | Target level after boost |

These are compile-time constants in `sched/mlfq.h`.

## Related Documents

- [mlfq.md](mlfq.md)
- [mlfq-cpu-accounting.md](mlfq-cpu-accounting.md)
- [mlfq-priority-queues.md](mlfq-priority-queues.md)
