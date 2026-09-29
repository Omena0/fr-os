# SLAB Allocator

## Overview

The SLAB allocator provides efficient fixed-size object allocation for the kernel. It reduces fragmentation and allocation overhead for frequently allocated objects (inodes, task structs, VMAs, network buffers, etc.) by maintaining pools of pre-initialized objects.

## Architecture

The SLAB allocator has three levels:

```
kmalloc(size)
    ↓
[size class selection → appropriate slab cache]
    ↓
Per-CPU magazine (hot path, no lock)
    ↓
Per-NUMA slab partial/full list (under slab cache lock)
    ↓
Buddy allocator (page allocation, under zone lock)
```

## Slab Caches

A slab cache is created for each type of kernel object:

```c
struct slab_cache *kmem_cache_create(
    const char *name,      // e.g., "task_struct"
    size_t      obj_size,  // object size in bytes
    size_t      align,     // alignment requirement
    unsigned    flags,     // SLAB_ZERO, SLAB_HWCACHE_ALIGN, etc.
    void (*ctor)(void *)   // optional constructor, called on new objects
);
```

Predefined caches for common kernel objects are created during `slab_init()`. The `kmalloc` API uses a set of power-of-2 generic caches:

```
kmalloc-8, kmalloc-16, kmalloc-32, kmalloc-64, kmalloc-128,
kmalloc-256, kmalloc-512, kmalloc-1024, kmalloc-2048, kmalloc-4096
```

## Slab Structure

A slab is one or more contiguous physical pages containing:

- A `struct slab` header (at the beginning of the page or out-of-line).
- A freelist of available objects (encoded as a linked list of indices).
- The objects themselves (contiguous, with `align` padding).

```c
struct slab {
    struct slab_cache *cache;  // owning cache
    uint32_t free_count;       // number of free objects in this slab
    uint32_t total_count;      // total object capacity
    uint16_t freelist;         // index of first free object
    // followed by: uint8_t objs[total_count * obj_size]
};
```

States: **full** (no free objects), **partial** (some free), **empty** (all free — can be returned to buddy allocator).

## Per-CPU Magazine (Fast Path)

Each CPU has a per-cache **magazine** — a small array of pre-allocated objects:

```c
struct slab_magazine {
    uint32_t count;            // number of objects currently in magazine
    uint32_t capacity;         // magazine size (e.g., 64 objects)
    void    *objects[64];      // the objects
};
```

Allocation fast path:

```
if magazine->count > 0:
    return magazine->objects[--magazine->count]  // O(1), no lock
```

Deallocation fast path:

```
if magazine->count < magazine->capacity:
    magazine->objects[magazine->count++] = obj    // O(1), no lock
```

When the magazine empties/fills: swap with the per-CPU depot (a loaded/empty magazine pair). If the depot has no spare: go to the global slab cache under lock to refill.

## SLAB vs. SLUB

This allocator is conceptually based on the SLAB design. A simplified implementation without per-NUMA node lists and without per-object debugging metadata is sufficient for initial implementation. More sophisticated SLUB-like per-CPU slabs may be adopted in a future optimization pass.

## Allocation Lifecycle

```
kmem_cache_alloc(cache):
  1. Check per-CPU magazine → O(1), no lock
  2. Swap magazine from depot if empty → O(1), per-CPU spinlock
  3. Allocate from partial slab → slab cache lock
  4. If no partial slabs: allocate new slab from buddy → zone lock
  5. Return object

kmem_cache_free(cache, obj):
  1. Return to per-CPU magazine → O(1), no lock
  2. If magazine full: swap with depot → per-CPU spinlock
  3. If depot also full: flush a full magazine to the slab cache → slab cache lock
```

## Destructor and Constructor

If the cache was created with a `ctor`, it is called once when a new slab is allocated. The constructor initializes invariant fields. When an object is freed, only variable fields (non-invariant state) need to be reset — the constructor's work persists. This reduces initialization overhead on the allocation path.

## Related Documents

- [physical-allocator.md](physical-allocator.md)
- [per-cpu-caches.md](per-cpu-caches.md)
- [virtual-memory.md](virtual-memory.md)
- [kernel/kernel-libraries.md](../kernel/kernel-libraries.md)
