# Memory Layout

This document describes what `Fr Core` actually does. Where a previous version
of this file described a design, the design is not in the tree; the discrepancies
are called out in [Not Implemented](#not-implemented) at the bottom rather than
left implicit.

Every address below is a constant you can grep for. The ones in the tables come
from `src/include/boot.h`, `src/kernel/include/vmm.h`, `src/kernel/include/pmm.h`
and `src/kernel/process.c`.

## Physical Address Space

The physical address space layout is established by the BIOS E820 memory map and
passed to the kernel by stage 2 in `struct bootinfo`.

```
Physical Address Range        Usage
─────────────────────────     ─────────────────────────────────────────
0x0000_0000 – 0x0000_7BFF    Real mode IVT, BIOS data area
0x0000_7C00 – 0x0000_7DFF    Stage 1 (MBR) and the drive byte at 0x7DFC
0x0000_7E00 – 0x0000_FFFF    Stage 1 real-mode stack
0x0000_8000 – 0x0000_DFFF    Stage 2 image
0x0000_E000 – 0x0001_FFFF    Stage 2 32-bit stack (72 KiB, grows down)
0x0002_0000 – 0x0002_0FFF    Firmware bounce window (one 512-byte sector)
0x0002_1000 – 0x0002_2FFF    Program-header and VBE scratch
0x0009_0000 – 0x0009_0BFF    E820 map, up to 128 entries of 24 bytes
0x0009_1000 – 0x0009_10E7    struct bootinfo
0x0010_0000 – 0x002C_7FFF    The kernel image (268 KB today; 0x1D0000 max)
0x002D_0000 – 0x002D_FFFF    Stage 2's bootstrap page tables
0x0010_0000 – ...            Free conventional memory
[MMIO regions]               Device memory (E820 type 2/3/4, reserved)
```

The PMM manages only frames an E820 entry marks `USABLE` (type 1), and then
removes three ranges from that set before handing it to the buddy allocator:

- `[0, 1 MiB)` — the bootloader is still running out of it (`main.c`).
- `0x100000 .. phys(_ebss)` — the kernel image, derived from the link-time
  `_ebss` (`pmm.c`'s `reserve_kernel_image()`).
- the PMM's own metadata arena: the presence bitmaps and the page array
  (`pmm.c`).

Stage 2's bootstrap page tables at `0x2D0000` are reserved by `main.c` after
`pmm_init()` returns, because they sit above the image end and are reachable by
the allocator.

## Virtual Address Space (x86-64, 48-bit)

```
Virtual Address Range                          Usage
────────────────────────────────────────────   ─────────────────────────────
0x0000_0000_0000_0000                         NULL guard page (unmapped)
0x0000_0000_0001_0000 – 0x0000_7FFF_FFFF_FFFF   User address space (128 TB)
  0x0000_0000_0001_0000                       ELF load base (load_bias = 0)
  0x0000_0001_0000_0000                       brk heap base (MM_BRK_BASE)
  0x0000_0002_0000_0000                       mmap region base (MM_MMAP_BASE)
  0x0000_0007_FFF7_FF00_0000                  mmap region limit
  0x0000_0007_FFFF_FF00_0000                  Initial user stack top
──────── canonical hole ───────────────────────────────────────────────────────
0xFFFF_8000_0000_0000 – 0xFFFF_FFFF_FFFF_FFFF   Kernel address space (128 TB)
  0xFFFF_8000_0000_0000                       Direct map, 0 – 4 GiB
  0xFFFF_8000_0400_0000                       (direct map ends here)
  0xFFFF_C000_0000_0000                       vmalloc area
  0xFFFFFFFF_8000_0000                       Kernel window, +16 MiB
  0xFFFFFFFF_8019_1300                       Per-CPU data
  0xFFFFFFFF_801C_8000                       Kernel stack top (32 KiB)
```

The user half is bounded above by `USER_ADDRESS_MAX` (`0x0000800000000000`). The
stack occupies the top 8 MiB of it (`USER_STACK_SIZE`); `mmap` allocates upwards
from `MM_MMAP_BASE` and stops at the stack base. There is no free-space search
over already-mapped holes: `mm_find_free()` starts above everything handed out
so far, so a fragmented address space runs out rather than filling gaps.

### The direct map is 4 GiB, not 64 TB

`PHYS_DIRECT_MAP` is `0xFFFF800000000000` and `phys_to_virt(p)` is
`PHYS_DIRECT_MAP + p`, so the mapping is an identity map over whatever the
direct-map constructor chose to cover. That constructor uses
`DIRECT_MAP_BYTES = (4ULL << 30)` (`src/kernel/vmm.c`) — **4 GiB**. The boot log
says the extent on every run, and which of two page sizes it used:

```
vmm: direct map 0-4 GiB, 1 GiB pages      <- CPUID.0x80000001 EDX bit 26 set
vmm: direct map 0-4 GiB, 2 MiB pages      <- bit 26 clear
```

`map_direct_map()` picks between them at `src/kernel/vmm.c:221`: 1 GiB pages as
PDPT entries with `PS=1` when `cpu_features.has_1gb_pages` is set, and 2 MiB
pages through a PD otherwise. The 2 MiB path is the fallback and the 4 KiB path
is deliberately not used at all — a 4 GiB machine mapped with 4 KiB pages needs
2048 page-table pages and 2048 TLB entries, so almost every access to ordinary
memory would be a page walk; with 2 MiB pages it needs four and four, and with
1 GiB pages one.

`ZONE_HIGH` exists and is populated on a machine with more than 4 GiB of RAM, and
the PMM will hand out frames above 4 GiB as a fallback from `ZONE_NORMAL`. Those
frames are *not* reachable through the direct map: anything that dereferences
`phys_to_virt()` of one — `GFP_ZERO`'s memset, `pt_alloc_zeroed`, `slab_new`,
`clone_table` — faults. On a 4 GiB reference machine the zone is empty and this
is latent.

This was audit finding **#35**, a three-way disagreement between this document,
`vmm.c` and `pmm.h`. **The code is authoritative and the documents were both
wrong**: `DIRECT_MAP_BYTES` in `src/kernel/vmm.c` decides, and the header text
in `src/kernel/include/pmm.h` describing `ZONE_HIGH` as "reachable through the
direct map" has been corrected to say the opposite. Both corrections are landed
(2026-10-02). The hazard itself is unchanged: a ZONE_HIGH allocation is still
permitted, still handed out, and still faults in every consumer that touches it.

One thing did survive that resolution and was corrected later the same day: this
page also stated the page size, and it was wrong independently of the extent. The
extent was a three-way disagreement; the page size was this page alone against the
code. Resolving a disagreement by choosing the authoritative side and marking the
item done does not re-derive the other two sides — it only settles the question
that was asked. The 4 KiB/2 MiB/1 GiB choice was a separate question that nobody
had asked yet, and it took until `has_1gb_pages` was finally assigned
(`cpu_features.c:218`) before anyone could notice that the 1 GiB path had never
run.

### The kernel window

The kernel is linked at `KERNEL_VIRT_BASE = 0xFFFFFFFF80000000` and stage 2
copies the image to physical `KERNEL_LANDING_ADDR = 0x100000`. The two are not
the same offset and the difference is 1 MiB, so every higher-half address
translates as:

    phys = (virt - KERNEL_VIRT_BASE) + KERNEL_LANDING_ADDR

The window is 16 MiB, 4 KiB pages, reached through **PML4 511 / PDPT 510**. The
4 KiB granularity is forced by geometry, not preference: `0x100000` is not 2 MiB
aligned, so the first 2 MiB cannot be a 2 MiB page.

## Kernel Heap

Kernel dynamic memory comes in two forms:

- **SLAB allocator** (`kmalloc`/`kfree`): fixed-size kernel objects, backed by
  order-0 buddy pages, with a per-CPU magazine in front of each cache.
- **`vmalloc()`/`vfree()`**: large, variable-size allocations from the vmalloc
  area at `0xFFFFC00000000000`.

Neither is exposed to userspace.

## Not Implemented

These were claimed by earlier versions of this page and are not in the tree. They
are listed so nobody writes code against them.

- **ASLR — there is none.** Not for the executable load base, not for the stack,
  not for `mmap`. Every one of those is at a fixed address: `exec_load_and_run()`
  calls `elf_load()` with `load_bias == 0`, the initial user stack top is
  `0x00007FFFFFFFF000`, and `mmap` starts at `MM_MMAP_BASE`
  (`0x0000200000000000`). There is no randomisation of any kind.
- **KASLR — there is none.** The kernel image is loaded at its link address
  every time. `KASLR_SLIDE_BITS` is defined in `boot.h` and nothing uses it.
- **A 64 TB direct map** — it is 4 GiB. See above.
- **A VDSO** at `0x0000_7FFF_FFFF_0000` — there is no vDSO; that address is just
  the top of the range `USER_ADDRESS_MAX` leaves usable.
- **Per-thread interrupt stacks** at `0xFFFFFFFF_FFE0_0000` — there is one
  kernel stack per task, allocated from vmalloc, and no separate IST stack (IST
  pointers are zero, with a comment in `gdt.c` explaining that a vmalloc'd page
  is not representable in the 32-bit IST field).
- **NX on the direct map or the kernel window** — both are mapped RWX. See
  `security/nx-enforcement.md`.

## Related Documents

- [memory/overview.md](../memory/overview.md)
- [memory/virtual-memory.md](../memory/virtual-memory.md)
- [memory/physical-allocator.md](../memory/physical-allocator.md)
- [security/aslr.md](../security/aslr.md)
