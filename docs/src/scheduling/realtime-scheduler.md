# Real-Time Scheduler

## Overview

The real-time (RT) scheduler class handles threads that require deterministic, low-latency scheduling. RT threads always preempt MLFQ threads. No MLFQ thread runs while any RT thread is runnable on the same CPU.

## RT Priority Model

RT threads have a static priority value in the range [0, 99]. Higher values mean higher priority (opposite convention from `nice`):

- Priority 99: Highest RT priority.
- Priority 0: Lowest RT priority (but still above all MLFQ threads).

## Scheduling Policy

Within the RT class, scheduling is **strict priority preemptive**:

1. The highest-priority runnable RT thread always runs.
2. Ties (same priority): FIFO ordering — the thread that became runnable first runs first.
3. An RT thread runs until it:
   - Blocks (I/O, mutex, sleep).
   - Yields (`sched_yield()`).
   - Is preempted by a higher-priority RT thread.
   - (Optional) Exhausts its deadline-mode time budget.

Unlike MLFQ, RT threads are **never demoted**. An RT thread at priority 50 will always preempt an RT thread at priority 49.

## RT Thread Creation

A thread is placed in the RT class via:

```c
struct sched_param param = { .sched_priority = 80 };
sched_setscheduler(tid, SCHED_FIFO, &param);
```

Requires `CAP_SYS_NICE`.

POSIX scheduling policies exposed:

- `SCHED_FIFO`: Strict priority FIFO as described above.
- `SCHED_RR`: Round-robin among threads of the same priority (each thread gets a fixed 10 ms quantum before the next same-priority RT thread runs). Still preempts all lower-priority RT threads.

## Data Structure

RT threads use the per-CPU `rt_prio_array`:

```c
struct rt_prio_array {
    uint64_t bitmap;                // 100-bit bitmask (two uint64_t for 128 bits)
    struct list_head queue[100];    // one FIFO queue per priority level
};
```

Dequeue: `ctzll(bitmap)` gives the highest non-empty priority — O(1).

## Throttling (RT Bandwidth)

An unconstrained RT thread can starve the entire system. To prevent this, RT bandwidth throttling limits RT threads to a configurable fraction of CPU time:

- `RT_RUNTIME_US`: RT threads may run for at most this many microseconds per period.
- `RT_PERIOD_US`: Period length (default: 1,000,000 µs = 1 second).
- Default: RT threads get 950,000 µs / 1,000,000 µs = 95% of CPU time.
- Remaining 5% is always available to MLFQ threads (prevents complete starvation).

When an RT thread exceeds its runtime budget for the period, it is throttled: removed from the RT class for the remainder of the period and placed in MLFQ level 0 until the next period begins.

Throttling can be disabled per-system for embedded or real-time use cases (requires kernel build option).

## CPU Affinity

RT threads support CPU affinity (see [cpu-affinity.md](cpu-affinity.md)). By default, RT threads can run on any CPU. For hard real-time workloads, pinning an RT thread to a dedicated CPU eliminates all migration overhead.

## Related Documents

- [overview.md](overview.md)
- [deadline-scheduling.md](deadline-scheduling.md)
- [mlfq-priority-queues.md](mlfq-priority-queues.md)
- [syscalls/scheduling-syscalls.md](../syscalls/scheduling-syscalls.md)
