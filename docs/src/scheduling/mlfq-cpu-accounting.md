# MLFQ CPU Accounting

## Purpose

CPU accounting tracks how much CPU time each thread has consumed at its current MLFQ level. This drives two behaviors:

1. **Demotion**: A thread that exhausts its quota at the current level is moved to the next lower level.
2. **I/O boost**: A thread that wakes from I/O wait is recognized as I/O-bound and receives a temporary priority advantage.

## Per-Thread Accounting Fields

```c
struct task {
    // ...
    int       mlfq_level;          // current MLFQ level (0 = highest)
    uint64_t  level_cpu_time_ns;   // nanoseconds spent at this level (not reset on block)
    uint64_t  quantum_ns;          // current level's quantum in nanoseconds
    uint64_t  last_sched_ns;       // TSC-derived timestamp when thread was last scheduled
    uint64_t  total_cpu_time_ns;   // lifetime CPU time (for /proc and accounting)
    uint8_t   io_boost;            // temporary I/O boost flag
};
```

## Demotion Logic

At every scheduler tick (end of time slice or preemption):

```
cpu_used = now - task->last_sched_ns
task->level_cpu_time_ns += cpu_used
task->total_cpu_time_ns += cpu_used

if task->level_cpu_time_ns >= task->quantum_ns:
    if task->mlfq_level < MLFQ_LEVELS - 1:
        task->mlfq_level++
        task->quantum_ns = mlfq_quantum[task->mlfq_level]
        task->level_cpu_time_ns = 0
        task->io_boost = 0
    // else: already at lowest level, stays there
```

This is evaluated when the running thread's time slice expires (timer interrupt) or when the thread is preempted by a higher-priority thread.

## I/O Boost

When a thread wakes from an I/O wait (read, write, disk, network, pipe):

```
if task->io_boost == 0 and task->mlfq_level > 0:
    task->mlfq_level--              // move up one level
    task->io_boost = 1              // prevent cascading boosts
    task->level_cpu_time_ns = 0     // reset quota at new level
```

The `io_boost` flag prevents a thread from climbing levels indefinitely through rapid I/O cycling. One I/O wakeup → one level increase, then the thread must earn its place or be demoted normally.

## Gaming Prevention

The key property is that `level_cpu_time_ns` is **not reset when the thread voluntarily blocks**. A CPU-bound thread that calls `sleep(1ms)` before its quantum expires does not reset its CPU time counter. Only explicit demotion (when the full quantum is consumed) or an aging boost resets `level_cpu_time_ns`.

This prevents the classic MLFQ gaming strategy of "block briefly before quantum expires to reset the timer."

## Per-Thread CPU Time Reporting

`total_cpu_time_ns` is accumulated without bound. It is used for:

- `/proc/<pid>/stat` CPU time reporting (in clock ticks, via `ns_to_ticks()`).
- The debugging `top`-like utility.
- Kernel tracing events.

## Scheduler Tick

The LAPIC timer fires at the tick rate (default: 1000 Hz = 1 ms per tick). Each tick:

1. Increment `rq->clock_ns` by the tick interval.
2. Update CPU accounting for the current thread.
3. Check if the thread's quantum is exhausted.
4. If exhausted: demote thread, enqueue it, select next thread.
5. If not: continue running.

At 1000 Hz, the overhead per tick is bounded to the accounting update and a comparison — no list scans on the common case.

## Related Documents

- [mlfq.md](mlfq.md)
- [mlfq-aging.md](mlfq-aging.md)
- [mlfq-priority-queues.md](mlfq-priority-queues.md)
- [debugging/performance-counters.md](../debugging/performance-counters.md)
