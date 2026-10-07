# Load Balancing

## Status

Load balancing migrates tasks across CPUs to keep them evenly loaded. The balancer runs periodically and on CPU wake/idle transitions.

## What Would Be Needed

A load balancer would:

1. Run periodically (e.g., every 4 ms via `sched_tick`) or when a CPU becomes idle.
2. Collect `load_avg` or `nr_running` from all online CPUs' run queues.
3. Detect imbalance: `max_load / avg_load > threshold` (e.g., 1.25).
4. Migrate tasks from the busiest CPU to the least loaded CPU.
5. Send `VECTOR_IPI_RESCHEDULE` to the destination CPU to trigger a reschedule.

## Current State

- No periodic balance pass exists in `sched_tick()`.
- No idle balance pass exists in the idle loop.
- No `load_avg` tracking exists (only `nr_running`).
- No IPI reschedule writer exists (`VECTOR_IPI_RESCHEDULE` is defined but unused).
- No cache affinity penalty tracking exists.
- No heterogeneous core awareness exists.

## Design References

The following documents describe a design for load balancing but do not reflect the current implementation:

- This document (load-balancing.md)
- [multicore-overview.md](multicore-overview.md)
- [work-stealing.md](work-stealing.md)
- [per-core-runqueues.md](per-core-runqueues.md)

## Related Documents

- [multicore-overview.md](multicore-overview.md)
- [work-stealing.md](work-stealing.md)
- [per-core-runqueues.md](per-core-runqueues.md)
- [cpu-affinity.md](cpu-affinity.md)