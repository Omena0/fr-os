# Load Balancing

## Overview

The load balancer ensures CPU time is evenly distributed across all available CPUs by migrating threads between per-CPU run queues when imbalance is detected. It operates without a global lock.

## Trigger Conditions

Load balancing is triggered in two ways:

1. **Periodic** (pull): Each CPU runs a load balance pass once every `SCHED_BALANCE_INTERVAL_MS` (default: 4 ms). This is triggered by the scheduler tick.
2. **Idle** (pull): When a CPU becomes idle (no runnable threads), it immediately attempts to steal threads from the busiest CPU rather than waiting for the periodic interval.

## Imbalance Detection

An imbalance is defined as:

```
max_load / avg_load > BALANCE_THRESHOLD  (default: 1.25)
```

Where `load` is the smoothed `load_avg` from each CPU's run queue.

Detailed algorithm:

1. Collect `load_avg` for all online CPUs.
2. Compute `avg_load = total_load / nr_cpus`.
3. Find `busiest_cpu` (highest `load_avg`) and `idle_cpu` (this CPU, which is idle or least loaded).
4. If `busiest_cpu.load_avg > idle_cpu.load_avg * BALANCE_THRESHOLD`: migrate.

## Thread Migration

When the load balancer decides to migrate a thread from CPU B to CPU A:

1. Lock CPU B's run queue.
2. Find a migratable thread at the lowest MLFQ level on CPU B (to minimize impact on interactive threads).
3. Check that the thread's affinity mask allows CPU A.
4. Remove the thread from CPU B's run queue.
5. Unlock CPU B's run queue.
6. Lock CPU A's run queue.
7. Place the thread on CPU A's run queue (at its current MLFQ level).
8. Unlock CPU A's run queue.
9. If CPU A is idle: send reschedule IPI (vector 241) to CPU A.

Number of threads migrated per balance pass: at most `max(1, (busiest_load - avg_load) / 2)` threads, capped at 8 per pass to avoid oscillation.

## Migration Restrictions

A thread may not be migrated if:

- It is currently running (`TASK_RUNNING` and `rq.current == thread`).
- Its CPU affinity mask does not include the destination CPU.
- It is an RT thread at or above priority 90 (critical RT threads are assumed to be intentionally pinned).
- It is the idle thread.

## Cache Affinity Penalty

To avoid thrashing, threads that were recently migrated incur a **cache penalty**:

- Within `SCHED_CACHE_HOT_NS` (default: 5 ms) of their last context switch, threads are not migrated.
- This prevents the load balancer from moving a thread that has just warmed up caches on its current CPU.

## Heterogeneous Core Awareness

On heterogeneous CPU systems:

- Load is normalized by core weight: `normalized_load = nr_running / core_weight`.
- Balance target is equal normalized load, not equal thread count.
- CPU-intensive threads (low MLFQ level) are preferentially migrated to high-weight cores.
- Background threads (high MLFQ level) are preferentially migrated to efficiency cores.

## Related Documents

- [multicore-overview.md](multicore-overview.md)
- [per-core-runqueues.md](per-core-runqueues.md)
- [work-stealing.md](work-stealing.md)
- [cpu-affinity.md](cpu-affinity.md)
