# Buddy Allocator

## Overview

The buddy allocator is the physical page allocation algorithm used by the PMM. It manages physical pages in power-of-2 blocks ("orders"), allowing efficient allocation of contiguous physical memory and O(log N) coalescing of freed blocks.

## Order System

`BUDDY_MAX_ORDER` is 11 (`src/kernel/include/pmm.h`), so there are **12** order
levels, order 0 through order 11:

| Order | Pages | Size |
|---|---|---|
| 0 | 1 page | 4 KB |
| 1 | 2 pages | 8 KB |
| 2 | 4 pages | 16 KB |
| 3 | 8 pages | 32 KB |
| 4 | 16 pages | 64 KB |
| 5 | 32 pages | 128 KB |
| 6 | 64 pages | 256 KB |
| 7 | 128 pages | 512 KB |
| 8 | 256 pages | 1 MB |
| 9 | 512 pages | 2 MB |
| 10 | 1024 pages | 4 MB |
| 11 | 2048 pages | 8 MB |

Each zone has 12 free lists (`struct list_head free_list[BUDDY_MAX_ORDER + 1]`),
one per order, and its `free_bitmap` has 12 meaningful bits. This constant is
defined once, in `pmm.h`; `pmm.c` does not define it a second time.

## Allocation Algorithm

To allocate `2^order` contiguous pages:

1. Check the free list at the requested order.
2. If non-empty: pop a block from the list. Done. — O(1) best case.
3. If empty: find the next non-empty order `k > order`.
4. Pop a block of order `k`.
5. Split: repeatedly split the block in half, inserting the upper half into the free list at each order, until order `order` is reached.
6. Return the lower half at the target order.

Worst case: O(MAX_ORDER) splits = O(10) iterations.

## Deallocation / Coalescing

To free a block at a given order:

1. Compute the "buddy" block address: XOR the block's physical address with `(1 << order) * PAGE_SIZE`.
2. Check if the buddy is free (by checking `struct page.order == order` and `PG_buddy` flag).
3. If buddy is free: remove buddy from free list. Merge both blocks into an order+1 block.
4. Repeat from step 2 with the merged block (at order+1) up to `MAX_ORDER`.
5. Insert the final block into the appropriate free list.

Amortized cost: O(1). Worst case: O(MAX_ORDER) = O(10).

## Free List Bitmask

For O(1) detection of which orders have free blocks, each zone maintains a bitmask:

```c
uint16_t free_bitmap;   // bit N set = order N free list is non-empty
```

Finding the smallest non-empty order ≥ `requested_order`:

```c
uint16_t mask = ~((1 << requested_order) - 1);
unsigned bits = free_bitmap & mask;
if (bits == 0)
        return -1;                      /* nothing at or above the order */
int order = __builtin_ctz(bits);
```

The zero test is not optional. `__builtin_ctz(0)` is undefined behaviour, and on
a zone with no free block at or above the requested order this is exactly the
value it would be handed. `first_order_at_least()` in `pmm.c` tests `mask == 0`
before the call.

## Zone Locking

Each zone has a single spinlock protecting its free lists and bitmask. The lock is held only for the duration of the allocation or deallocation (typically < 20 instructions). Preemption is disabled while holding the lock.

The lock is a performance bottleneck at high allocation rates. Per-CPU caches (see [per-cpu-caches.md](per-cpu-caches.md)) mitigate this by batching allocations.

## Fragmentation

The buddy system reduces external fragmentation compared to naive first-fit allocators, but does not eliminate it:

- **Internal fragmentation**: A request for 5 pages must use an order-3 block (8 pages) — 3 pages wasted.
- **External fragmentation**: Over time, free blocks become split, making large contiguous allocations harder. Mitigated by the memory compaction daemon.

## Related Documents

- [physical-allocator.md](physical-allocator.md)
- [per-cpu-caches.md](per-cpu-caches.md)
- [memory-compaction.md](memory-compaction.md)
- [huge-pages.md](huge-pages.md)
