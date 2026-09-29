# Huge Pages

## Overview

Huge pages are memory pages larger than the standard 4 KB base page. Using huge pages for memory-intensive applications reduces TLB pressure (fewer TLB entries cover more memory) and can significantly improve performance for workloads with large working sets.

## Supported Huge Page Sizes

On x86-64 with 4-level paging:

| Level | Size | Name |
|---|---|---|
| PD entry (2 MB page) | 2 MB | Huge page (default huge page size) |
| PDPT entry (1 GB page) | 1 GB | Gigantic page |

2 MB huge pages are supported. 1 GB gigantic pages require physically contiguous 1 GB regions — only feasible on systems with large amounts of RAM and are not a primary target.

## Kernel Use of Huge Pages

The kernel uses 2 MB huge pages for:

- **Direct physical map** (`0xFFFF_8000_0000_0000`): The entire physical memory direct map uses 2 MB mappings where possible, reducing the number of PD entries and TLB entries needed for kernel data access.
- **vmalloc huge regions**: Large `vmalloc` allocations (> 2 MB) that are aligned use 2 MB page table entries.

## Userspace Huge Pages

Userspace can request huge page mappings via:

### `mmap` with `MAP_HUGETLB`

```c
void *buf = mmap(NULL, 256 * 1024 * 1024,
                 PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                 -1, 0);
```

This creates a VMA backed by 2 MB physical pages. The kernel allocates huge pages from the huge page pool on demand (on first access, via page fault).

### Transparent Huge Pages (THP)

The OS supports **Transparent Huge Pages**: the kernel automatically promotes standard 4 KB anonymous VMAs to use 2 MB backing pages when:

- The VMA spans at least 2 MB of contiguous virtual space.
- The VMA is 2 MB-aligned.
- A contiguous 2 MB run of physical pages is available (sourced from the buddy allocator at order 9).

THP promotion happens in the page fault handler or as a background optimization pass (part of the memory compaction daemon). THP can be configured per-process via `madvise(MADV_HUGEPAGE / MADV_NOHUGEPAGE)`.

## Huge Page Pool

Pre-allocated huge pages are maintained in a dedicated pool:

- Pool size: configurable at boot via kernel parameter `hugepages=N` (default: 0).
- Pages are allocated at boot from the buddy allocator at order 9 (512 × 4 KB = 2 MB).
- The pool is fixed-size; pages are not returned to the buddy allocator unless explicitly released.

The pool can also be grown at runtime via the observability/configuration interface.

## Fragmentation and Huge Pages

Huge page allocation requires a physically contiguous 2 MB run. As physical memory fragments over time, the availability of free 512-page contiguous runs decreases. The memory compaction daemon (see [memory-compaction.md](memory-compaction.md)) works to consolidate free memory specifically to make huge page allocation possible.

## Related Documents

- [buddy-allocator.md](buddy-allocator.md)
- [virtual-memory.md](virtual-memory.md)
- [memory-compaction.md](memory-compaction.md)
- [physical-allocator.md](physical-allocator.md)
