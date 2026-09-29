# Deadline Scheduling

## Overview

Deadline scheduling is an optional mode within the RT scheduler class. A thread using deadline scheduling specifies a **period** and a **deadline** for its work unit. The scheduler attempts to ensure the thread completes each work unit before its deadline.

## Deadline Parameters

A deadline-mode thread specifies three parameters:

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

Among deadline-mode RT threads, the thread with the **earliest absolute deadline** runs first. This is the EDF (Earliest Deadline First) policy, which is theoretically optimal for single-CPU scheduling.

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

if sum_of_utilizations + new_utilization > DEADLINE_MAX_UTIL (default: 0.95):
    return -EBUSY   // reject
```

This ensures the system does not become overloaded with deadline threads and that the 5% RT bandwidth reserve is maintained.

## Deadline Miss Handling

If a thread fails to complete its `runtime_ns` quota before `deadline_ns`:

- The thread continues running (no forced preemption at deadline).
- A **deadline miss event** is recorded and delivered to the thread as `SIGXCPU` (optional, configurable).
- The miss is counted in per-thread statistics (`/proc/<pid>/sched`).

The scheduler does not attempt recovery or rescheduling to compensate for missed deadlines. If a thread consistently misses deadlines, it should be reconfigured (more CPU time, lower frequency) or the system is overloaded.

## Multicore and Deadline

Deadline scheduling is per-CPU-local. When a deadline thread is created on a multi-CPU system:

- It may be placed on any CPU (unless affinity is set).
- Admission control is checked only for the target CPU.
- The scheduler does not globally balance deadline threads across CPUs.

For hard real-time scenarios, pin deadline threads to dedicated CPUs via `sched_setaffinity`.

## Activation

A deadline thread that is sleeping is activated (woken) by:

- Expiry of its period timer (most common).
- An explicit `sched_yield()` after completing a work unit within its runtime budget.

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
