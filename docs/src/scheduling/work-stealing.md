# Work Stealing

## Status

Work stealing distributes load across CPUs: an idle CPU steals tasks from a busy CPU's run queue when its own is empty.

## What Would Be Needed

Work stealing would allow an idle CPU to proactively steal tasks from a busy CPU's run queue:

1. When a CPU becomes idle (no runnable tasks), it scans other CPUs' run queues.
2. Finds a migratable task (respecting affinity, not running, not RT priority ≥ 90, not idle task).
3. Removes it from the victim CPU's run queue (under victim's lock).
4. Places it on the idle CPU's run queue (under idle CPU's lock).
5. Sends `VECTOR_IPI_RESCHEDULE` to the victim CPU if needed.

## Current State

- No idle-loop work stealing exists.
- No victim selection logic exists.
- No `VECTOR_IPI_RESCHEDULE` writer exists.
- The global migration list (`global_queue`) exists but is only used by `sched_migrate()` for explicit affinity changes, not for automatic work stealing.

## Design References

This document describes a design for work stealing but does not reflect the current implementation.

## Related Documents

- [load-balancing.md](load-balancing.md)
- [multicore-overview.md](multicore-overview.md)
- [per-core-runqueues.md](per-core-runqueues.md)
- [cpu-affinity.md](cpu-affinity.md)