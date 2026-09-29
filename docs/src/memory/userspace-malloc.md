# Userspace malloc

## Overview

The userspace memory allocator (`malloc`, `free`, `calloc`, `realloc`) is implemented in libc. It manages the userspace heap using virtual memory obtained from the kernel via `brk` and `mmap` syscalls.

## Design

The libc allocator uses a segregated-fit design with size classes, per-thread caches, and a global heap managed by the kernel's virtual address space:

```
malloc(size)
    ↓
[size class determination]
    ↓
Per-thread cache (tcache) → fastest path, no lock
    ↓
Per-arena free list → arena lock (one arena per thread up to max)
    ↓
sys_brk or sys_mmap → kernel virtual memory
```

This design is similar in spirit to jemalloc/tcmalloc but simplified for initial implementation.

## Size Classes

| Size Range | Granularity | Source |
|---|---|---|
| 1 – 128 bytes | 8 bytes | tcache / arena small bin |
| 129 – 512 bytes | 16 bytes | arena small bin |
| 513 – 4096 bytes | 64 bytes | arena large bin |
| 4097 – 64 KB | 256 bytes | arena large bin |
| > 64 KB | page-aligned | Direct `mmap` (returned to OS on free) |

Large allocations (> 64 KB) are mapped directly with `mmap(MAP_ANONYMOUS)` and unmapped on `free`. This avoids heap fragmentation for large objects.

## Thread Cache (tcache)

Each thread has a thread-local cache (tcache):

- Holds freed objects per size class (up to `TCACHE_MAX_COUNT = 64` per class).
- Allocation: pop from tcache — O(1), no lock.
- Deallocation: push to tcache — O(1), no lock.
- When tcache is full for a size class: flush half the objects to the thread's arena.

tcache is thread-local (`__thread struct tcache`), accessed via TLS. No synchronization needed.

## Arenas

An arena is an allocation pool with its own free lists and heap region. Multiple arenas exist to reduce lock contention:

- One arena per thread, up to `MALLOC_MAX_ARENAS` (default: 4× CPU count).
- Each thread is assigned an arena at first allocation (round-robin).
- If a thread's arena is locked, try another arena rather than blocking.

Arena internals:

- **Bins**: Per-size-class free lists.
- **Top chunk**: Unreserved heap space at the top of the arena's `brk` region.
- **mmap'd chunks**: Large direct-mapped allocations (tracked separately).

## Heap Growth

When an arena needs more space:

1. First, try `sys_brk` to extend the heap — O(1) system call.
2. If `brk` fails (address space conflict) or the request is large: use `sys_mmap(MAP_ANONYMOUS)`.
3. The new region is added to the arena as a new chunk.

## Security

- **Guard regions**: Each arena requests extra pages at chunk boundaries that are marked `PROT_NONE`. Overflow into guard pages causes a SIGSEGV.
- **Zeroing on free**: Freed memory is zeroed before returning to the free list (prevents information leakage via use-after-free reads).
- **Canaries**: Per-chunk canary value written at allocation, checked on free (detects heap corruption).

## `calloc` and `realloc`

- `calloc(n, size)`: Calls `malloc(n * size)` with overflow check. Zeroes the result (pages from `mmap` are already zero; for tcache objects, zero explicitly).
- `realloc(ptr, size)`: If the chunk can be extended in place: return `ptr`. Otherwise: `malloc(size)`, `memcpy`, `free(ptr)`.

## Related Documents

- [virtual-memory.md](virtual-memory.md)
- [per-cpu-caches.md](per-cpu-caches.md)
- [security/hardened-allocator.md](../security/hardened-allocator.md)
- [syscalls/memory-syscalls.md](../syscalls/memory-syscalls.md)
