# Multicore Scheduling Overview

## Architecture

The OS uses a **distributed scheduling model**: each CPU has its own run queue and schedules from it independently. There is no global run queue. Threads are assigned to a CPU and remain there unless actively migrated.

## Per-CPU Run Queues

Each CPU maintains a `struct runqueue` in per-CPU memory:

```
CPU 0 RunQueue     CPU 1 RunQueue     ... CPU N RunQueue
  RT array           RT array                RT array
  MLFQ [0]           MLFQ [0]               MLFQ [0]
  MLFQ [1]           MLFQ [1]               MLFQ [1]
  ...                ...                    ...
  MLFQ [7]           MLFQ [7]               MLFQ [7]
  current task       current task           current task
  idle task          idle task              idle task
```

Per-CPU storage eliminates false sharing: each CPU's hot data is on its own cache lines.

## Initial Thread Placement

When a new thread is created via `fork()` or `clone()`:

1. The kernel selects the **least loaded CPU** (lowest `nr_running` count) as the initial target.
2. The thread is placed on that CPU's run queue.
3. If the spawning process has a CPU affinity mask, only CPUs in the mask are considered.

## Interactivity: Cache-Aware Placement

For latency-sensitive (interactive) threads waking from I/O, the scheduler prefers to place the thread on the **same CPU it last ran on** (its "home CPU"), provided:

- The home CPU is within the thread's affinity mask.
- The home CPU's run queue is not overloaded (load < 2× average).

This maximizes cache reuse: the thread's data is likely still warm in the home CPU's L1/L2 caches.

## Heterogeneous CPU Support

For systems with heterogeneous CPUs (e.g., performance cores vs. efficiency cores), the scheduler maintains a **core weight model**:

- Each CPU is assigned a weight (1 = efficiency core, 2 = performance core, by example).
- Load balancing normalizes thread counts by weight rather than raw thread count.
- RT and high-MLFQ-level threads are preferentially placed on high-weight CPUs.
- Background (low MLFQ level) threads are preferentially placed on low-weight CPUs.

On QEMU with uniform vCPUs, all weights are 1 (homogeneous).

## Cross-CPU Wakeup

When thread A wakes thread B (e.g., via pipe write, mutex unlock, condition signal):

- If B was last running on a different CPU than A: B is placed on its home CPU's run queue.
- If B's home CPU is overloaded: select the least loaded CPU in B's affinity mask.
- The target CPU receives a reschedule IPI (vector 241) if the woken thread is higher priority than what is currently running there.

## Related Documents

- [per-core-runqueues.md](per-core-runqueues.md)
- [load-balancing.md](load-balancing.md)
- [cpu-affinity.md](cpu-affinity.md)
- [work-stealing.md](work-stealing.md)
- [overview.md](overview.md)
