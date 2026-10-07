# SLAB Allocator

## Overview

The SLAB allocator provides efficient fixed-size object allocation for the kernel. It reduces fragmentation and allocation overhead for frequently allocated objects (inodes, task structs, VMAs, network buffers, etc.) by maintaining pools of pre-initialized objects.

The implementation in `src/kernel/kmalloc.c` is a two-tier allocator:

- **kmalloc caches (up to 4 KiB)**: Power-of-two size classes (8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096 bytes) served from per-CPU magazines backed by slab pages.
- **vmalloc (larger than 4 KiB)**: Direct page-backed allocations from the vmalloc region.

## Architecture

```
kmalloc(size)
    ↓
[size class selection → appropriate kmalloc cache]
    ↓
Per-CPU magazine (fast path, spinlock_irqsave)
    ↓
Cache partial/full slab lists (under cache lock)
    ↓
Buddy allocator (page allocation, under zone lock)
```

## Slab Caches

A slab cache is created for each size class during `kmalloc_init()`:

```c
static const size_t kcache_size[] = {
    8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096,
};
#define NUM_KCACHE  (sizeof(kcache_size) / sizeof(kcache_size[0]))
```

Each cache is described by `struct kmalloc_cache`:

```c
struct kmalloc_cache {
    size_t size;              // object size (e.g., 8, 16, ...)
    unsigned order;           // buddy order of one slab (0 for 4 KiB)
    size_t slab_bytes;        // PAGE_SIZE << order
    size_t obj_off;           // first object offset past slab header
    uint32_t per_slab;        // objects per slab
    struct list_head partial; // slabs with free objects
    struct list_head full;    // slabs with no free objects
    spinlock_t lock;          // protects partial/full lists
};
```

Slab caches are **not** created on demand with a `kmem_cache_create` API. The kernel uses only the fixed set of power-of-two generic caches above. There is no constructor/ctor support, no named caches (like "task_struct"), and no `SLAB_ZERO`/`SLAB_HWCACHE_ALIGN` flags.

## Slab Structure

A slab is a single buddy-allocated page (4 KiB, order 0) carved into objects:

```c
struct kmalloc_slab {
    struct list_head node;    // on the cache's partial or full list
    struct page *page;        // the buddy block, for pmm_free_pages()
    uint32_t total;           // objects in this slab
    uint32_t free;            // objects on the freelist
    uint32_t first;           // index of first free object, or SLAB_NONE
    uint32_t class_idx;       // the cache this slab was carved for
};
```

The free list is threaded through the objects themselves as a chain of `uint32_t` indices (not pointers), saving 12 bytes per object vs an intrusive list — this is what makes the 8-byte class possible.

States: **full** (no free objects), **partial** (some free), **empty** (all free — returned to buddy allocator).

## Per-CPU Magazine (Fast Path)

Each CPU has a per-cache **magazine** — a small array of pre-allocated objects:

```c
struct kmalloc_magazine {
    uint32_t count;           // number of objects currently in magazine
    uint32_t capacity;        // magazine size (64 objects)
    void    *objects[64];     // the objects
};
```

Allocation fast path:

```
if magazine->count > 0:
    return magazine->objects[--magazine->count]  // O(1), spinlock_irqsave
```

Deallocation fast path:

```
if magazine->count < magazine->capacity:
    magazine->objects[magazine->count++] = obj    // O(1), spinlock_irqsave
```

When the magazine empties/fills: swap with the per-CPU depot (a loaded/empty magazine pair). If the depot has no spare: go to the global slab cache under lock to refill.

The magazine path uses `spinlock_irqsave` (not lock-free) because an interrupt allocating mid-push would observe a half-updated magazine.

## Allocation Lifecycle

```
kmalloc(size):
  1. Check per-CPU magazine → O(1), spinlock_irqsave
  2. If empty: refill magazine from cache (slab's worth, up to 8 objects) → cache lock
  3. If no partial slabs: allocate new slab from buddy → zone lock
  4. Return object

kfree(ptr, size):
  1. Determine tier by pointer alignment: page-aligned → vmalloc, else slab
  2. For slab: `size` selects first candidate cache; slab header's class_idx decides truth
  3. Return to per-CPU magazine → O(1), spinlock_irqsave
  4. If magazine full: flush to cache under lock → cache lock
  5. If cache full: release slab to buddy → zone lock
```

The `size` argument of `kfree()` is advisory — it selects a first candidate cache, but the slab header's `class_idx` decides the truth. A block is returned to the class it was carved from, never to the class the caller happened to name.

## Related Documents

- [physical-allocator.md](physical-allocator.md)
- [per-cpu-caches.md](per-cpu-caches.md)
- [virtual-memory.md](virtual-memory.md)
- [kernel/kernel-libraries.md](../kernel/kernel-libraries.md)
