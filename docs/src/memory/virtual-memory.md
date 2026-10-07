# Virtual Memory Manager

## Overview

The Virtual Memory Manager (VMM) manages per-process virtual address spaces. It maps virtual pages to physical frames, handles page faults, and provides the `mmap`/`munmap`/`mprotect` syscall interface[^intel-sdm-vol3a][^amd-apm-vol2].

## Virtual Memory Areas (VMAs)

Each process has a set of Virtual Memory Areas (VMAs) describing the layout of its address space. A VMA is a contiguous range of virtual pages with uniform properties:

```c
struct vma {
    uintptr_t   start;         // start virtual address (page-aligned)
    uintptr_t   end;           // end virtual address (exclusive, page-aligned)
    uint32_t    prot;          // PROT_READ | PROT_WRITE | PROT_EXEC
    uint32_t    flags;         // MAP_PRIVATE | MAP_SHARED | MAP_ANONYMOUS
    struct file *file;         // backing file (NULL for anonymous)
    off_t        offset;       // offset into backing file
    struct list_head list;     // linked list of VMAs
    struct rb_node rb_node;    // red-black tree node (for fast lookup by address)
};
```

VMAs are stored in a list ordered by ascending start address, which makes
`mm_find_vma` a binary search. Lookup is by ascending start address, and the
page-fault path needs "the VMA containing this address".

## Address Space Structure

```c
struct address_space {
    phys_addr_t        pgd;            // physical address of PML4
    spinlock_t         lock;
    struct list_head   vma_list;       // VMAs ordered by ascending start
    uint64_t           vma_count;

    virt_addr_t start_code, end_code;
    virt_addr_t start_data, end_data;
    virt_addr_t start_brk, brk;        // current heap break
    virt_addr_t stack_top;

    virt_addr_t mmap_base, mmap_next;  // randomised mmap base (ASLR policy)
    volatile s32 refcount;             // threads sharing it hold references
};
```

One `struct address_space` per process. Threads within the same process share it.

VMAs are a **sorted list, not a red-black tree**. A first implementation of this
document specified an `rb_root` alongside the list; the struct in
`src/kernel/include/vmm.h` has no tree, and `mm_find_vma()` binary-searches the
sorted list instead. The list is kept sorted on insert, so lookup is O(log n) with
no extra allocation and no second index to keep consistent — which is the whole
reason to have picked one over the other. If lookup ever shows up in a profile,
adding the tree is a change to `mm_find_vma` alone.

## Boot Order

Virtual memory is initialised **before** physical memory, and the order is forced
rather than chosen:

1. `vmm_init()` — builds the direct map, the kernel window, and the kernel PML4.
2. `vmm_switch_to_kernel_pgd()` — replaces the bootloader's tables. `phys_to_virt()` resolves from here on.
3. `pmm_init()` — places its bitmaps and page array through `phys_to_virt()`.

`pmm_init()` cannot come first: it dereferences `PHYS_DIRECT_MAP + phys` for its
metadata, and that address does not resolve until the direct map exists. Running
the other way round faults on the first `memset` of the page array, before
anything has printed a character.

`vmm_init()` needs no allocator, which is what breaks the apparent circularity.
Its first page tables come from a static pool inside the kernel image, which the
bootloader's identity mapping already covers. The pool is physically inside the
image, so `pmm_reserve_range()` on the low megabyte keeps it out of the buddy
allocator permanently.

The kernel window is an **identity map with 4 KiB pages**, not 2 MiB. The image
shares 2 MiB boundaries with ordinary allocatable memory, so a 2 MiB mapping there
would make physical 1–16 MiB writable through the kernel window, and a stray
store would corrupt a buddy block the allocator believes is free. The direct map
uses 1 GiB or 2 MiB pages, where no such hazard exists.

## Design Scope

The target virtual memory design includes:

- **Recursive page-table map.** The kernel uses a kernel window at PML4[511] for page table access. The recursive-map layout (PML4[510] pointing to the PML4 itself) is documented in `vmm.h` and its helpers compile.
- **File-backed VMAs.** A VMA with a non-NULL `file` reads from the page cache on fault.
- **Copy-on-write.** A shared page is tracked; a write to one produces a private copy.
- **TLB shootdown.** An IPI propagates page-table changes to all CPUs; `vmm_map_page` and `vmm_unmap_page` invalidate TLBs remotely as well as locally.
- **`vfree` reclamation.** It marks a block free and returns both the mapping and the physical frames to the allocator.

## `mmap` Implementation

`sys_mmap(addr, len, prot, flags, fd, offset)`:

1. Select a virtual address (use `addr` if `MAP_FIXED`, else find a free region from `aslr_base_mmap`).
2. Validate range does not conflict with existing VMAs.
3. Allocate a `struct vma` and insert into tree and list.
4. For `MAP_ANONYMOUS`: pages are not allocated yet — demand-paged on first access.
5. File-backed: pages are allocated from the page cache on fault.
6. COW: a write to a shared page allocates a private copy.
6. Return the virtual address.

Pages are not physically allocated until the process accesses them (demand paging via page fault handler).

## Page Fault Handling

On a page fault (`#PF`, vector 14)[^intel-sdm-interrupts]:

1. Get fault address from CR2.
2. Look up the VMA for the fault address (binary search over the sorted list).
3. If no VMA: return failure, and the caller delivers SIGSEGV.
4. If VMA found and anonymous: allocate a physical page (`pmm_alloc_page`), zero
   it, insert the page table entry, and return to retry the instruction. A write
   to a VMA without `VM_WRITE` fails rather than silently being granted write.
   File-backed and COW faults are handled by the page-cache and COW paths above.

## TLB Shootdown

*Not implemented. See "Not Yet Implemented" above; this is the plan for when a
second CPU exists.*

When a page table entry is modified (unmapped, permission changed), all CPUs that may have the old entry in their TLB must be notified. This is done via TLB shootdown IPIs[^intel-sdm-vol3a][^amd-apm-vol2]:

1. Modify the page table entry.
2. Flush the local TLB for the affected address (`invlpg`).
3. Send TLB shootdown IPI (vector 240) to all CPUs sharing this address space.
4. Each receiving CPU executes `invlpg` for the affected address.
5. The originating CPU waits (via per-CPU ack counter) until all CPUs have flushed.

Shootdown is batched: multiple address invalidations are collected and sent in a single IPI when possible.

## `brk` System Call

`sys_brk(new_brk)`: Extend or shrink the process heap.

- Extending: create a new anonymous VMA from the old `brk` to `new_brk` (or extend the existing heap VMA).
- Shrinking: remove the excess VMA range and unmap the pages.
- Returns the new `brk` value on success.

## Related Documents

- [physical-allocator.md](physical-allocator.md)
- [slab-allocator.md](slab-allocator.md)
- [userspace-malloc.md](userspace-malloc.md)
- [huge-pages.md](huge-pages.md)
- [security/aslr.md](../security/aslr.md)

## References

- [Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A — Paging][intel-sdm-vol3a]
- [Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A — Interrupts and Exceptions][intel-sdm-interrupts]
- [AMD64 Architecture Programmer's Manual, Volume 2 — System Programming][amd-apm-vol2]
- [Linux Kernel Documentation — Memory Management][linux-mm]

[intel-sdm-vol3a]: https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html#vol3a "Intel SDM Volume 3A: System Programming Guide, Part 1"
[intel-sdm-interrupts]: https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html#vol3a "Intel SDM Volume 3A: Interrupts and Exceptions"
[amd-apm-vol2]: https://www.amd.com/en/developer/architecture-programmer-manuals.html "AMD64 Architecture Programmer's Manual Volume 2"
[linux-mm]: https://www.kernel.org/doc/html/latest/core-api/memory-allocation.html "Linux Kernel Memory Management API"
