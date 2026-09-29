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
  SLAB Allocator
      — kernel object cache, fixed-size allocations
      — backed by virtual pages from VMM
  Virtual Memory Manager (VMM)
      — page-level virtual mapping (page tables, mmap, brk)
      — requests physical pages from PMM
  Physical Memory Manager (PMM) — Buddy Allocator
      — allocates physical page frames in power-of-2 blocks
      — manages physical memory zones (DMA, Normal, High)
  Raw RAM Layer
      — frame tracking, allocation metadata
      — NUMA node awareness
```

## Allocation Sizing Guide

| Size | Recommended Allocator |
|---|---|
| 1 byte – 512 bytes | SLAB (from appropriate size class) |
| 512 bytes – 8 KB | SLAB or `kmalloc` (backed by SLAB) |
| 8 KB – 1 MB | `vmalloc` (VMM, non-contiguous physical) |
| > 1 MB | `vmalloc` or direct buddy allocation |
| Userspace heap | `malloc` (libc) → `brk`/`mmap` syscalls |

## Design Principles

- **O(1) fast path**: Per-CPU SLAB magazines provide O(1) allocation without locks for the common case.
- **Fragmentation avoidance**: SLAB reuses freed objects before requesting new pages. Buddy coalesces free blocks. Virtual space is compacted by the memory compaction daemon.
- **Strict physical/virtual separation**: Kernel code never assumes physical contiguity for virtually allocated memory (except for DMA allocations).
- **NUMA awareness**: Physical allocations prefer the NUMA node local to the requesting CPU.
- **Huge page support**: 2 MB huge pages are used for kernel direct map and for userspace regions that use `MAP_HUGETLB`.

## Concurrency Strategy

| Layer | Locking |
|---|---|
| Per-CPU SLAB magazine | None (per-CPU, only accessed by local CPU) |
| SLAB global pool | Per-slab-cache spinlock |
| Buddy allocator | Per-zone spinlock |
| VMM (page table ops) | Per-process page table lock (or RCU for reads) |
| TLB shootdown | IPI broadcast (atomic, non-blocking) |

## Memory System Daemons

- **Memory compaction daemon**: Periodically defragments physical memory by moving pages to consolidate free blocks. See [memory-compaction.md](memory-compaction.md).
- **Page reclamation daemon (`kreclaimd`)**: Reclaims pages from page cache and anonymous memory under memory pressure. See [page-reclamation.md](page-reclamation.md).
- **Overcommit manager**: Enforces the configured overcommit policy. See [overcommit-policy.md](overcommit-policy.md).

## Related Documents

- [physical-allocator.md](physical-allocator.md)
- [buddy-allocator.md](buddy-allocator.md)
- [virtual-memory.md](virtual-memory.md)
- [slab-allocator.md](slab-allocator.md)
- [userspace-malloc.md](userspace-malloc.md)
- [per-cpu-caches.md](per-cpu-caches.md)
- [numa-policies.md](numa-policies.md)
- [huge-pages.md](huge-pages.md)
- [page-reclamation.md](page-reclamation.md)
- [memory-compaction.md](memory-compaction.md)
- [overcommit-policy.md](overcommit-policy.md)
