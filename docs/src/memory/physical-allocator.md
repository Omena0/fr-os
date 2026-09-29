# Physical Memory Allocator

## Overview

The Physical Memory Manager (PMM) is the lowest-level memory allocator. It manages physical page frames (4 KB each) and provides them to the Virtual Memory Manager, DMA allocators, and kernel subsystems that need physically contiguous memory.

## Initialization

The PMM is initialized during early boot from the E820 memory map:

1. Parse E820 entries. Entries of type `USABLE` contribute free frames.
2. Reserve frames for: kernel image, boot page tables, `BootInfo` structure.
3. Align frame tracking to page boundaries.
4. Build the buddy allocator free lists from all usable frames.
5. Report total free frames to the kernel log.

## Memory Zones

Physical memory is divided into zones to satisfy different allocation requirements:

| Zone | Physical Range | Purpose |
|---|---|---|
| `ZONE_DMA` | 0 – 16 MB | Legacy ISA DMA — device buffers for old hardware |
| `ZONE_NORMAL` | 16 MB – 4 GB | Standard kernel and user page frames |
| `ZONE_HIGH` | > 4 GB | Extended memory (mapped via direct map for 64-bit kernel) |

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
