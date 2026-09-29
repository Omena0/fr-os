# Per-CPU Allocator Caches

## Overview

Per-CPU caches are the primary hot-path optimization in the memory allocation subsystem. They allow O(1) allocation and deallocation without any lock acquisition for the common case, by maintaining a small cache of free objects local to each CPU.

## Levels of Per-CPU Caching

Per-CPU caches exist at two levels:

### 1. SLAB Magazine (Kernel)

Each SLAB cache has a per-CPU magazine — see [slab-allocator.md](slab-allocator.md) for details.

- **Capacity**: 64 objects per cache per CPU (configurable per cache).
- **Replenishment**: When empty, swap with a loaded depot magazine or refill from the global slab.
- **Flush**: When full, swap with an empty depot magazine or flush to the global slab.
- **Lock**: No lock on the magazine itself; depot swap uses a per-CPU lock.

### 2. Physical Page Cache (PMM)

The PMM maintains a per-CPU cache of order-0 (single) physical pages:

```c
struct percpu_page_cache {
    struct page *pages[PERCPU_PAGE_CACHE_SIZE];  // PERCPU_PAGE_CACHE_SIZE = 128
    int          count;
    int          high;   // high watermark: 128
    int          batch;  // refill/flush batch size: 32
};
```

Allocation:

- If `count > 0`: pop `pages[--count]` — O(1), no lock.
- If `count == 0`: call buddy allocator with `batch=32` pages under zone lock. Refill cache.

Deallocation:

- If `count < high`: push `pages[count++] = page` — O(1), no lock.
- If `count >= high`: flush `batch=32` pages to buddy allocator under zone lock.

This reduces the frequency of zone lock acquisitions by a factor of `batch`.

## Cache Coherence

Per-CPU caches are only accessed by the owning CPU. However, the kernel must handle the case where a thread migrates to a different CPU mid-allocation:

- Allocation and deallocation are not atomic. Migration during an allocation is safe because the magazine and page cache operations are each individually atomic from the perspective of other CPUs (they don't reference shared state until they need to touch the global pool).
- If a thread migrates while holding a partially-set-up allocation: this is handled by the preemption model. Preemption is disabled during the critical allocation section (magazine pop/push). Migration cannot occur during this window.

## Cache Drain on CPU Hotplug

When a CPU is taken offline:

1. Flush all SLAB magazines for that CPU to their respective global slab pools (under slab cache lock).
2. Flush the PMM per-CPU page cache to the buddy allocator (under zone lock).
3. The CPU's per-CPU memory region is not freed (it may be needed when the CPU comes back online).

## Tuning

| Parameter | Default | Effect |
|---|---|---|
| `SLAB_MAGAZINE_SIZE` | 64 | Magazine capacity per SLAB cache per CPU |
| `PERCPU_PAGE_CACHE_SIZE` | 128 | Max pages in the PMM per-CPU cache |
| `PERCPU_PAGE_BATCH` | 32 | Pages moved per buddy allocator access |

These are compile-time constants. They may be made runtime-tunable in a future release via the kernel parameter interface.

## Related Documents

- [slab-allocator.md](slab-allocator.md)
- [physical-allocator.md](physical-allocator.md)
- [buddy-allocator.md](buddy-allocator.md)
- [numa-policies.md](numa-policies.md)
