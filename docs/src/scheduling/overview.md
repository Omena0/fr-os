# Scheduling Overview

## Goals

The CPU scheduler is responsible for deciding which thread runs on which CPU at any given time. The scheduler must:

- Provide good **interactivity** for latency-sensitive tasks (UI, shells, audio).
- Provide good **throughput** for CPU-bound batch workloads.
- Provide **predictability** for real-time tasks with strict priority requirements.
- Scale efficiently across all available CPUs without global lock contention.

## Scheduler Classes

The scheduler is organized into two classes, evaluated in strict priority order:

```
Priority (highest to lowest):
  ┌────────────────────────────────────┐
  │  Real-Time Class (RT)              │  Always preempts MLFQ threads
  │  (strict priority, FIFO/deadline)  │
  └────────────────────────────────────┘
  ┌────────────────────────────────────┐
  │  MLFQ Class (normal threads)       │  Dynamic priority, aging
  │  (levels 0–7, level 0 = highest)  │
  └────────────────────────────────────┘
```

A thread belongs to exactly one class. Class membership is set at thread creation and may be changed by privileged code (`sched_setscheduler` with `CAP_SYS_NICE` for RT).

## MLFQ — Multi-Level Feedback Queue

The MLFQ is the primary scheduler for normal workloads. Key behaviors:

- **New threads** start at MLFQ level 0 (highest interactive priority).
- **CPU-bound threads** are demoted to lower levels as they consume full time quanta.
- **I/O-bound threads** stay at high levels because they block before exhausting their quota.
- **Aging** periodically boosts starved threads back to a higher level.
- **I/O completion boost**: threads that wake from I/O wait receive a temporary priority boost.

See [mlfq.md](mlfq.md) for the full design.

## Real-Time Scheduler

Real-time threads are scheduled above all MLFQ threads. Within the RT class:

- **Strict priority**: higher `rt_priority` always runs first.
- **Same priority**: FIFO ordering (first to be made runnable runs first).
- **Deadline mode**: optional; thread specifies a period and deadline; the scheduler attempts to meet the deadline.

See [realtime-scheduler.md](realtime-scheduler.md).

## Multicore Operation

Each CPU has its own per-core run queue. Threads are scheduled locally. A global load balancer periodically migrates threads between CPUs when imbalance is detected.

See [multicore-overview.md](multicore-overview.md).

## Thread Model

Both kernel threads and user threads use the same `struct task` and the same scheduler. There is no separate kernel thread scheduler. Kernel threads simply have no user address space; they run in the kernel page table context.

## Fast Path

Scheduler hot path (`schedule()`):

1. Dequeue the next RT thread from the current CPU's RT priority array (O(1) via bitmap priority scan).
2. If no RT thread is runnable, dequeue from the current CPU's MLFQ head (O(1)).
3. Call `switch_context(current, next)`.

Total time on the hot path: O(1), no global locks.

## Related Documents

- [mlfq.md](mlfq.md)
- [mlfq-priority-queues.md](mlfq-priority-queues.md)
- [mlfq-aging.md](mlfq-aging.md)
- [mlfq-cpu-accounting.md](mlfq-cpu-accounting.md)
- [realtime-scheduler.md](realtime-scheduler.md)
- [deadline-scheduling.md](deadline-scheduling.md)
- [multicore-overview.md](multicore-overview.md)
- [per-core-runqueues.md](per-core-runqueues.md)
- [load-balancing.md](load-balancing.md)
- [cpu-affinity.md](cpu-affinity.md)
- [work-stealing.md](work-stealing.md)
