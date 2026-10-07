# Scheduling Overview

## Goals

The CPU scheduler is responsible for deciding which thread runs on which CPU at any given time. The scheduler must:

- Provide good **interactivity** for latency-sensitive tasks (UI, shells, audio).
- Provide good **throughput** for CPU-bound batch workloads.
- Support real-time and deadline scheduling for time-critical workloads.
- Scale efficiently across CPUs with per-CPU run queues and a global migration list.

## Scheduler Classes

The scheduler implements four classes, evaluated in strict priority order:

```
Priority (highest to lowest):
  ┌────────────────────────────────────┐
  │  Deadline Class (SCHED_DEADLINE)   │  EDF — earliest absolute deadline first
  │  (runtime, deadline, period)       │  Admission control via utilization sum
  └────────────────────────────────────┘
  ┌────────────────────────────────────┐
  │  Real-Time Class (RT)              │  SCHED_FIFO / SCHED_RR
  │  (strict priority, FIFO/RR)        │  RT bandwidth reserve (95%)
  └────────────────────────────────────┘
  ┌────────────────────────────────────┐
  │  MLFQ Class (normal threads)       │  Dynamic priority, aging, I/O boost
  │  (levels 0–7, level 0 = highest)  │  SCHED_NORMAL, SCHED_BATCH, SCHED_IDLE
  └────────────────────────────────────┘
```

A thread belongs to exactly one class. Class membership is set at thread creation and may be changed by privileged code (`sched_setscheduler` with `CAP_SYS_NICE` for RT/Deadline).

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

- **Strict priority**: higher `rt_priority` always runs first (0-99, matching POSIX).
- **Same priority**: FIFO ordering (SCHED_FIFO) or round-robin (SCHED_RR).
- **RT bandwidth reserve**: RT tasks together get 95% of CPU time per period.

See [realtime-scheduler.md](realtime-scheduler.md).

## Deadline Scheduler (EDF)

Deadline threads are scheduled above RT threads. Within the Deadline class:

- **EDF policy**: earliest absolute deadline runs first.
- **Admission control**: new deadline task rejected if sum of (runtime/period) > 95%.
- **Parameters**: `runtime_ns`, `deadline_ns`, `period_ns` with `runtime ≤ deadline ≤ period`.

See [deadline-scheduling.md](deadline-scheduling.md).

## Multicore Operation

Each CPU has its own per-CPU run queue (`sched_runqueues[cpu]`). Threads are scheduled locally. A **global migration list** (`global_queue`) is used for cross-CPU task placement:

- `sched_migrate()` places a task on the global list; the destination CPU's `sched_drain_global()` adopts it.
- `sched_set_affinity()` restricts a task to a CPU mask; migrates if necessary.
- **No load balancer** is implemented — there is no periodic or idle balancing pass.
- **No work stealing** is implemented — idle CPUs do not steal from busy CPUs.
- **No IPI reschedule** is implemented — `VECTOR_IPI_RESCHEDULE` (vector 241) is defined but no writer exists.

See [multicore-overview.md](multicore-overview.md) and [per-core-runqueues.md](per-core-runqueues.md).

## Thread Model

Both kernel threads and user threads use the same `struct task` and the same scheduler. There is no separate kernel thread scheduler. Kernel threads simply have no user address space; they run in the kernel page table context.

## Fast Path

Scheduler hot path (`schedule()`):

1. Check deadline queue (O(1) via `deadline_count`).
2. Check RT priority array (O(1) via bitmap `ctz`).
3. Check MLFQ levels (O(1) via `mlfq_bitmap` `ctz`).
4. Call `switch_context(current, next)`.

Total time on the hot path: O(1), no global locks on the scheduling path.

## Preemption Guard

`schedule()` in `sched.c` guards with `if (per_cpu(preempt_count) != 0) return;` to prevent rescheduling from interrupt context. This fixes MEGA_AUDIT finding 2.11.

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