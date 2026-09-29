# Hardened Memory Allocator

## Overview

The hardened allocator adds security properties to the standard memory allocator (both kernel `kmalloc` and userspace `malloc`) to increase resistance to heap exploitation techniques.

## Security Properties

| Property | Description |
|---|---|
| Guard pages | Non-accessible pages adjacent to allocations (catch out-of-bounds access) |
| Freelist randomization | Free objects are returned in random order (prevents reliable heap shaping) |
| Use-after-free detection | Freed memory is poisoned; access detected via guard or poison values |
| Double-free detection | Second free of same pointer is detected and aborted |
| Canary values | Random canary words stored at the end of each heap object |
| Zero-on-free | Freed memory is zeroed to prevent information leakage through freed memory reuse |
| Allocation size jitter | Object size is rounded up to a random alignment within the size class |

## Guard Pages (Large Allocations)

For allocations ≥ 16 KB (direct mmap path in userspace malloc):

1. Allocate `size + 2 * PAGE_SIZE` from the VMM.
2. Mark the first and last pages `PROT_NONE` (guard pages).
3. Return a pointer to the start of the usable region (between the guards).

Any buffer overflow or underflow that reaches the guard page triggers a page fault → `SIGSEGV`.

## Canary Words (Small Allocations)

For SLAB/magazine allocations in the kernel, and for tcache/arena allocations in userspace:

- A random 8-byte canary is stored immediately after each allocated object (in the padding bytes of the size class).
- On free: the canary is verified before the object is returned to the free list.
- If the canary is corrupted: abort with a detailed error message.

```c
struct slab_obj_header {
    uint64_t canary;    // random value set at allocation
    // ... object data follows ...
    uint64_t canary_end; // same random value at the end
};
```

## Freelist Randomization

Instead of always returning the most recently freed object (LIFO — predictable for an attacker), the allocator selects a random free object within the magazine/tcache:

```c
void *alloc_from_cache(struct per_cpu_cache *cache) {
    int idx = csprng_next() % cache->count;
    void *obj = cache->objects[idx];
    cache->objects[idx] = cache->objects[--cache->count];
    return obj;
}
```

This prevents an attacker from reliably controlling which specific address they receive.

## Poison Values

On free, every byte of the freed object is written with a poison value (`0xDE`). An access to a freed object will read the poison value, likely causing a crash before arbitrary behavior occurs. A read-triggered crash is easier to detect than silent memory corruption.

## Double-Free Detection

Before adding a freed object back to the free list, a hash set of recently freed addresses is consulted:

```c
bool double_free_check(void *ptr) {
    return hashset_contains(&recently_freed, ptr);
}
```

If `ptr` is already in the set: abort with "double free detected at 0x...".

## Kernel vs Userspace

| Feature | Kernel (kmalloc/SLAB) | Userspace (malloc) |
|---|---|---|
| Guard pages | Only for kmalloc ≥ 4 pages | Yes, for mmap-backed allocs |
| Canaries | Per-slab | Per-tcache-entry |
| Freelist randomization | Per-CPU magazine | Per-tcache |
| Zero-on-free | Always | Always |
| Poison | Debug builds | Release builds |

## Related Documents

- [stack-canaries.md](stack-canaries.md)
- [aslr.md](aslr.md)
- [nx-enforcement.md](nx-enforcement.md)
- [memory/slab-allocator.md](../memory/slab-allocator.md)
- [memory/userspace-malloc.md](../memory/userspace-malloc.md)
