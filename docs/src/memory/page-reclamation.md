# Page Reclamation

## Overview

The page reclamation subsystem (`kreclaimd`) frees physical memory under pressure by reclaiming pages from the page cache (clean file-backed pages) and, if necessary, writing dirty pages to disk (anonymous memory swap or dirty file pages).

## When Reclamation Runs

Reclamation is triggered when:

1. **Low memory watermark** (`ZONE_LOW_WATERMARK`): the buddy allocator falls below a threshold. Reclamation runs asynchronously in `kreclaimd`.
2. **Critically low memory** (`ZONE_MIN_WATERMARK`): the buddy allocator falls below a minimum. Reclamation runs synchronously in the allocation path (direct reclaim) before returning an error.

Watermarks are set at boot based on total system memory:

```
ZONE_MIN = max(128 pages, total_pages * 0.01)
ZONE_LOW = ZONE_MIN * 2
ZONE_HIGH = ZONE_LOW * 3
```

When the free page count drops below `ZONE_LOW`, `kreclaimd` is woken.

## Page Categories

Pages are classified by reclaimability:

| Category | Reclaimable? | How |
|---|---|---|
| Clean page cache (unmodified file data) | Yes (easy) | Simply discard — can be re-read from disk |
| Dirty page cache (modified file data) | Yes (costly) | Write to disk, then discard |
| Anonymous memory (no swap configured) | No | Cannot reclaim without swap |
| Kernel memory (SLAB objects, kernel stacks) | Limited | Only via SLAB shrink callback |
| Locked pages (`mlock`) | No | |
| Anonymous memory (swap configured) | Yes (costly) | Write to swap partition, then discard |

## LRU Lists

The reclamation subsystem maintains two LRU lists per zone:

- **Active list**: Recently accessed pages.
- **Inactive list**: Older pages, candidates for reclamation.

Pages are placed on the active list when first added to the page cache. The LRU daemon periodically moves pages from the active to inactive list based on access frequency (tracked via page table `Accessed` bits).

When reclaiming:

1. Scan the inactive list.
2. For each page: check the `Accessed` bit. If recently accessed: promote back to active list (false positive reclaim avoided).
3. For clean pages: remove from page cache, return to buddy allocator.
4. For dirty pages: add to write-back queue. Write to disk. Then reclaim.

## Reclaim Watermark Response

```
Free pages     State                Action
─────────────────────────────────────────────────────
> HIGH         Normal               kreclaimd sleeps
LOW – HIGH     Low memory           kreclaimd runs, gentle reclaim
MIN – LOW      Pressure             kreclaimd runs aggressively
< MIN          Critical             Direct reclaim in allocation path
0              OOM                  OOM killer
```

## OOM Killer

If reclamation cannot free enough memory to satisfy an allocation, the Out-Of-Memory killer selects a process to terminate:

1. Compute an `oom_score` for each process: proportional to resident memory size, inversely proportional to runtime (recently started processes score higher).
2. Exclude: init, kernel threads, processes with `CAP_SYS_ADMIN`.
3. Kill the process with the highest score by sending `SIGKILL`.
4. Wait for the process to exit, then retry the failed allocation.

OOM kill events are logged at `KLOG_WARN` level with the killed process's name and PID.

## Fast Path

- Normal allocation (above `LOW`): no reclaim — `kreclaimd` is sleeping, zero overhead.
- Reclaim (below `LOW`): asynchronous in `kreclaimd` — no impact on allocation latency.
- Direct reclaim (below `MIN`): allocation latency increases by the time needed to write one or more pages to disk — worst case several hundred milliseconds.

## Related Documents

- [physical-allocator.md](physical-allocator.md)
- [buddy-allocator.md](buddy-allocator.md)
- [memory-compaction.md](memory-compaction.md)
- [overcommit-policy.md](overcommit-policy.md)
- [filesystem/page-cache.md](../filesystem/page-cache.md)
