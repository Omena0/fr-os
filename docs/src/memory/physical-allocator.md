# Physical Memory Allocator

## Overview

The Physical Memory Manager (PMM) is the lowest-level memory allocator. It manages physical page frames (4 KB each) and provides them to the Virtual Memory Manager, DMA allocators, and kernel subsystems that need physically contiguous memory.

## Initialization

The PMM is initialized during early boot from the E820 memory map:

1. Parse E820 entries. Only entries of type `USABLE` (type 1) contribute free
   frames; everything else is reserved by construction, because the presence
   bitmap is memset to "reserved" and only the usable pass clears it.
2. Reserve, before the free lists are built:
   - the PMM's own metadata arena (presence bitmaps and the page array), sized
     from the frame count;
   - the kernel image, `[KERNEL_LANDING_ADDR, phys(_ebss))`, derived from the
     link-time `_ebss`.
3. Align frame tracking to page boundaries.
4. Build the buddy allocator free lists from all usable frames, skipping the
   reserved ones.
5. Report usable and free frames to the kernel log.

Two more ranges are reserved by the *caller*, after `pmm_init()` returns:
`[0, 1 MiB)`, which the bootloader is still executing from, and stage 2's own
bootstrap page tables at `0x2D0000`–`0x2DFFFF`, which sit just above the kernel
image.

### Reading the log

`pmm_total()` and `pmm_free()` return **frame counts**, not byte counts. Use
`pmm_total_bytes()` and `pmm_free_bytes()` when you want bytes; converting by
shifting by 20 is the mistake this file's header once described.

## Memory Zones

Physical memory is divided into zones to satisfy different allocation requirements:

| Zone | Physical Range | Purpose |
|---|---|---|
| `ZONE_DMA` | 0 – 16 MB | Legacy ISA DMA — device buffers for old hardware |
| `ZONE_NORMAL` | 16 MB – 4 GB | Standard kernel and user page frames |
| `ZONE_HIGH` | > 4 GB | Extended memory. **Not reachable through the direct map**, which covers only 4 GiB — a `ZONE_HIGH` frame dereferenced through `phys_to_virt()` faults. On a 4 GiB machine the zone is empty and this is latent. |

Allocation requests specify which zone they require via `gfp_t` flags:

- `GFP_DMA`: Must allocate from `ZONE_DMA`.
- Default: Prefer `ZONE_NORMAL`, fall back to `ZONE_HIGH` if exhausted.

## Core API

```c
// Allocate 2^order contiguous physical pages
struct page *pmm_alloc_pages(int order, gfp_t flags);

// Allocate a single page (order=0)
struct page *pmm_alloc_page(gfp_t flags);

// Free pages (must match original order)
void pmm_free_pages(struct page *page, int order);

// Convert page descriptor to physical address
phys_addr_t page_to_phys(struct page *page);

// Convert physical address to page descriptor
struct page *phys_to_page(phys_addr_t addr);
```

## `struct page`

Each physical page has a `struct page` descriptor in the kernel's direct map:

```c
struct page {
    atomic_t      refcount;      // reference count
    uint32_t      flags;         // PG_dirty, PG_locked, PG_slab, etc.
    uint8_t       order;         // current buddy order (0 if in use)
    uint8_t       zone;          // which zone this page belongs to
    uint16_t      numa_node;     // NUMA node ID
    struct list_head buddy_list; // link in buddy free list (when free)
    union {
        void      *slab_ptr;     // pointer to slab (if PG_slab)
        pgoff_t    index;        // page cache index (if in page cache)
    };
};
```

The `struct page` array is allocated at boot and covers all physical pages in the system. For a 4 GB system: 4 GB / 4 KB = 1,048,576 pages × 64 bytes = 64 MB of page descriptors (in the direct map).

## Fast Path

Single-page allocation (`pmm_alloc_page`):

1. Check per-CPU page cache (see [per-cpu-caches.md](per-cpu-caches.md)).
2. If cache non-empty: pop one page — O(1), no lock.
3. If cache empty: allocate a batch from the buddy allocator (order-0 free list) under zone spinlock. Refill per-CPU cache. — O(log MAX_ORDER) worst case.

## Related Documents

- [buddy-allocator.md](buddy-allocator.md)
- [per-cpu-caches.md](per-cpu-caches.md)
- [numa-policies.md](numa-policies.md)
- [virtual-memory.md](virtual-memory.md)
