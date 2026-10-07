# Memory Management Overview

## Allocator Hierarchy

Memory management is structured as a layered hierarchy. Each layer serves a different granularity and purpose:

```
Userspace
  malloc()          — userspace heap allocator (libc)
      ↕ sys_mmap / sys_brk (syscall boundary)
Kernel
  Virtual Memory Allocator (VMA)
      — manages virtual address space regions per process
  kmalloc (per-CPU caches backed by buddy allocator)
      — kernel object cache, fixed-size allocations
      — backed by physical pages from PMM
  Virtual Memory Manager (VMM)
      — page-level virtual mapping (page tables, mmap, brk)
      — requests physical pages from PMM
  Physical Memory Manager (PMM) — Buddy Allocator
      — allocates physical page frames in power-of-2 blocks
      — manages physical memory zones (DMA, Normal, High)
  Raw RAM Layer
      — frame tracking, allocation metadata
```

## Allocation Sizing Guide

| Size | Recommended Allocator |
|---|---|
| 1 byte – 512 bytes | `kmalloc` (from appropriate size class) |
| 512 bytes – 8 KB | `kmalloc` (larger size classes) |
| 8 KB – 1 MB | `vmalloc` (VMM, non-contiguous physical) |
| > 1 MB | `vmalloc` or direct buddy allocation |
| Userspace heap | `malloc` (libc) → `brk`/`mmap` syscalls |

## Design Principles

- **O(1) fast path**: Per-CPU `kmalloc` caches absorb the common case, so the
  per-object cost is a push or pop rather than a list walk. The cache path
  does take a spinlock — it is not lock-free — but it is the cheapest of the
  tiers and it is what keeps an interrupt that allocates mid-operation
  from handing the same pointer to two callers.
- **Fragmentation avoidance**: The buddy allocator coalesces free blocks with
  its buddy on free. There is no compaction daemon and no page reclamation daemon.
- **Strict physical/virtual separation**: `vmalloc` never assumes physical
  contiguity; only `pmm_alloc_dma_range()` does, and it exists for DMA.
- **NUMA awareness**: none. `struct page` has no `numa_node`, and every zone is
  derived purely from the physical address.
- **Huge pages**: the 4 GiB direct map is built with 2 MiB pages. User mappings
  are all 4 KiB, and `MAP_HUGETLB` does nothing.

## Concurrency Strategy

| Layer | Locking |
|---|---|
| Per-CPU kmalloc cache | One spinlock (irqsave) per CPU per cache — **not** lock-free |
| Buddy allocator | Per-zone spinlock |
| PMM frame metadata bitmaps | Unsynchronised after `pmm_init()` |
| VMM page-table updates | **None.** `invlpg` flushes whichever address space CR3 currently names, and nothing protects the walk |
| TLB shootdown | Not implemented; there is no IPI and no second CPU |

## Memory System Daemons

None. There is no compaction daemon, no page reclamation daemon (`kreclaimd`)
and no overcommit manager. The documents that describe them —
[memory-compaction.md](memory-compaction.md),
[page-reclamation.md](page-reclamation.md),
[overcommit-policy.md](overcommit-policy.md) — describe a design, not this tree.

## Related Documents

- [physical-allocator.md](physical-allocator.md)
- [buddy-allocator.md](buddy-allocator.md)
- [virtual-memory.md](virtual-memory.md)
- [kmalloc.md](kmalloc.md)
- [userspace-malloc.md](userspace-malloc.md)
- [per-cpu-caches.md](per-cpu-caches.md)
- [numa-policies.md](numa-policies.md)
- [huge-pages.md](huge-pages.md)
- [page-reclamation.md](page-reclamation.md)
- [memory-compaction.md](memory-compaction.md)
- [overcommit-policy.md](overcommit-policy.md)