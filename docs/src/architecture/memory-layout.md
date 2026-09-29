# Memory Layout

## Physical Address Space

The physical address space layout is established by the BIOS E820 memory map and passed to the kernel by the stage 2 bootloader.

```
Physical Address Range        Usage
─────────────────────────     ─────────────────────────────────────────
0x0000_0000 – 0x0000_7BFF    Real mode IVT, BIOS data area
0x0000_7C00 – 0x0000_7DFF    Stage 1 bootloader (MBR, discarded)
0x0000_7E00 – 0x0007_FFFF    Stage 2 bootloader + early stack (discarded)
0x0008_0000 – 0x0009_FFFF    Extended BIOS data area (avoid)
0x000A_0000 – 0x000B_FFFF    VGA framebuffer (legacy, avoid)
0x000C_0000 – 0x000F_FFFF    BIOS ROM (avoid)
0x0010_0000 – ...             Free conventional memory (kernel + userspace)
...         – top             Free RAM for allocation
[MMIO regions]                Device memory (ACPI-reported, reserved)
```

The kernel physical memory manager only manages regions marked as `USABLE` by E820.

## Virtual Address Space (x86-64, 48-bit)

```
Virtual Address Range                   Usage
────────────────────────────────────    ────────────────────────────────────────
0x0000_0000_0000_0000                   NULL guard page (unmapped)
0x0000_0000_0001_0000 – 0x0000_7FFF_FFFF_FFFF   User address space (128 TB)
  0x0000_0000_0001_0000                 User text/data (executable load base)
  [random ASLR offset]                 User stack (grows down)
  [random ASLR offset]                 User heap (grows up, managed by malloc)
  [random ASLR offset]                 mmap() regions (anonymous, file-backed)
  0x0000_7FFF_FFFF_0000                VDSO mapping (fixed top of user space)
──────── canonical hole ────────────────────────────────────────────────────────
0xFFFF_8000_0000_0000 – 0xFFFF_FFFF_FFFF_FFFF   Kernel address space (128 TB)
  0xFFFF_8000_0000_0000                Physical memory direct map (up to 64 TB)
  0xFFFF_C000_0000_0000                vmalloc area (kernel virtual allocations)
  0xFFFF_FFFF_8000_0000                Kernel text + data (linked here)
  0xFFFF_FFFF_FF00_0000                Per-CPU data regions
  0xFFFF_FFFF_FFE0_0000                Kernel stacks (per-thread interrupt stacks)
```

## ASLR

ASLR is applied to three independent regions in the user address space:

- **Executable load base**: randomized within a 1 GB window (30 bits entropy for PIE executables).
- **Stack base**: randomized within a 256 MB window (22 bits entropy).
- **mmap base**: randomized within a 1 TB window; each `mmap` call uses a fresh random offset from this base.

The kernel itself uses KASLR: the kernel image is loaded at a random offset within the higher-half window, selected at boot time and not changed during runtime.

## Kernel Heap

Kernel dynamic memory comes in two forms:

- **SLAB allocator**: For fixed-size kernel objects (inodes, task structs, etc.). Backed by virtual pages from the vmalloc area.
- **`vmalloc()`**: For large, variable-size kernel allocations. Sourced from the vmalloc area.

Neither is exposed to userspace.

## Related Documents

- [memory/overview.md](../memory/overview.md)
- [memory/virtual-memory.md](../memory/virtual-memory.md)
- [memory/physical-allocator.md](../memory/physical-allocator.md)
- [security/aslr.md](../security/aslr.md)
