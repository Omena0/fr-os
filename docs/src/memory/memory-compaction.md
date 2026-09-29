# Memory Compaction

## Overview

Memory compaction is the process of defragmenting physical memory by moving pages to consolidate free space. Over time, physical memory becomes fragmented: many small free regions rather than a few large contiguous ones. Compaction moves page-reclaimable pages to lower physical addresses to create large free blocks at higher addresses.

## Why Compaction

The buddy allocator requires physically contiguous pages for order > 0 allocations. Fragmentation makes it impossible to satisfy large allocations (order 9 for 2 MB huge pages) even when total free memory is sufficient. Compaction enables:

- 2 MB huge page allocation.
- Large DMA-contiguous buffers.
- Order-8+ kernel allocations.

## Compaction Daemon (`kcompactd`)

A kernel thread (`kcompactd`) runs continuously at idle priority. It is woken when:

- A large allocation fails due to fragmentation.
- The system is otherwise idle (opportunistic compaction).

`kcompactd` is per-NUMA-node: one thread per node.

## Compaction Algorithm

Compaction scans memory in two directions simultaneously:

```
Low address  →→→→  [migration scanner]
                        ↑ moves pages ↓
             [free scanner]  ←←←←  High address
```

1. **Free scanner** (from high addresses): Scans downward for free page blocks. Adds them to a "free target" list.
2. **Migration scanner** (from low addresses): Scans upward for movable pages (clean page cache pages, anonymous pages with swap, pages marked `__GFP_MOVABLE`).
3. For each movable page found by the migration scanner:
   a. Allocate a replacement page from the free scanner's list (from high addresses).
   b. Copy the page content.
   c. Update all references to the old page (page table entries, page cache index) to point to the new page.
   d. Release the old page to the buddy allocator (now at a lower address).
4. Scanners stop when they meet in the middle.

Result: movable pages are concentrated at low addresses; free blocks are consolidated at high addresses. The next large allocation succeeds from the high-address free region.

## Movable vs. Non-Movable Pages

| Page type | Movable? |
|---|---|
| Anonymous pages (with swap) | Yes |
| Clean page cache pages | Yes (just re-read from disk if needed) |
| Dirty page cache pages | Yes (but costly — requires write-back first) |
| Kernel SLAB objects | No (unless slab is empty and can be freed as a whole) |
| Locked pages (`mlock`) | No |
| DMA-locked buffers | No |

Compaction only moves movable pages. Non-movable pages act as "obstacles" that limit how much compaction can achieve.

## Compaction Cost

Compaction is I/O intensive (may trigger dirty page write-back). It uses CPU to copy page data. The compaction daemon limits its run time to `COMPACTION_MAX_BUDGET_MS` (default: 20 ms) per wakeup to avoid impacting interactive workloads.

When compaction is triggered synchronously (by a failing large allocation, `GFP_NORETRY` not set), it runs without a budget limit but at the allocating process's priority.

## Interaction with THP

The transparent huge page promotion path calls the compaction subsystem when a 2 MB-aligned allocation fails. Compaction attempts to free a 2 MB contiguous region at an aligned address before THP promotion.

## Related Documents

- [buddy-allocator.md](buddy-allocator.md)
- [page-reclamation.md](page-reclamation.md)
- [huge-pages.md](huge-pages.md)
- [physical-allocator.md](physical-allocator.md)
