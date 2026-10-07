# Deadline Scheduling (SCHED_DEADLINE) — EDF

## Status

Deadline scheduling (EDF — Earliest Deadline First) is **implemented**.

## Overview

Deadline scheduling is a scheduler class for tasks with strict timing requirements. A thread using deadline scheduling specifies a **period** and a **deadline** for its work unit. The scheduler attempts to ensure the thread completes each work unit before its deadline.

## Deadline Parameters

A deadline-mode thread specifies three parameters via `sched_setattr(tid, SCHED_DEADLINE, &attr)`:

| Parameter | Description |
|---|---|
| `runtime_ns` | How much CPU time the thread needs per period |
| `deadline_ns` | When within the period the thread must finish (relative to period start) |
| `period_ns` | How often the thread activates |

Constraint: `runtime_ns ≤ deadline_ns ≤ period_ns`.

Example: An audio callback that needs 2 ms of CPU every 10 ms with a 5 ms deadline:

```
runtime_ns  = 2,000,000
deadline_ns = 5,000,000
period_ns   = 10,000,000
```

## Scheduling Policy (EDF — Earliest Deadline First)

Among deadline-mode threads, the thread with the **earliest absolute deadline** runs first. This is the EDF (Earliest Deadline First) policy, which is theoretically optimal for single-CPU scheduling.

Absolute deadline = time of last period activation + `deadline_ns`.

Within a single CPU:

- Deadline threads run before `SCHED_FIFO`/`SCHED_RR` threads (highest RT sub-class).
- Among deadline threads: earliest deadline first.
- Ties: arbitrary ordering.

## Admission Control

Before a deadline thread is allowed to run, the scheduler performs **admission control**:

```
sum_of_utilizations = Σ (runtime_i / period_i) for all deadline threads on this CPU
new_utilization = new_thread.runtime_ns / new_thread.period_ns

if sum_of_utilizations + new_utilization > DEADLINE_MAX_UTIL_PCT (default: 95%):
    return -EBUSY   // reject
```

This ensures the system does not become overloaded with deadline threads and that the 5% RT bandwidth reserve is maintained.

## Deadline Miss Handling

If a thread fails to complete its `runtime_ns` quota before `deadline_ns`:

- The thread continues running (no forced preemption at deadline).
- A **deadline miss event** is recorded and delivered as a signal.
- The miss is counted in per-thread statistics (future: `/proc/<pid>/sched`).

The scheduler does not attempt recovery or rescheduling to compensate for missed deadlines. If a thread consistently misses deadlines, it should be reconfigured (more CPU time, lower frequency) or the system is overloaded.

## Implementation Details

Per-CPU run queue contains a deadline list:

```c
struct runqueue {
    // ...
    struct list_head deadline;  // ordered by abs_deadline, earliest first
    u32 deadline_count;
    u32 deadline_util;          // summed utilization in percent
};
```

- The list is kept sorted by `abs_deadline` (earliest first) so dequeue is O(1) `list_first_entry`.
- `deadline_util` tracks the sum of (runtime/period) * 100 for admission control.
- On enqueue: `abs_deadline = now_ticks() * 1_000_000 + deadline_ns` (converting ticks to ns).
- On tick: `rt_consumed` is incremented; when it reaches `rt_runtime`, the task is throttled until the next period.

## Syscall Interface

```c
struct sched_deadline_attr {
    uint64_t runtime_ns;
    uint64_t deadline_ns;
    uint64_t period_ns;
};
// Set via:
sched_setattr(tid, SCHED_DEADLINE, &attr);
// Requires CAP_SYS_NICE
```

## Related Documents

- [realtime-scheduler.md](realtime-scheduler.md)
- [overview.md](overview.md)
- [syscalls/scheduling-syscalls.md](../syscalls/scheduling-syscalls.md)