# Bootloader Memory Map

## Physical Memory Regions Used by the Bootloader

The bootloader uses the following physical memory regions during boot. All regions below `0x10_0000` are freed back to the kernel's physical allocator after boot (marked as reclaimable in the E820 map processing pass).

```
Physical Address     Size       Usage
──────────────────   ────────   ─────────────────────────────────────────
0x0000_0000          1 KB       Real-mode IVT (BIOS — do not touch)
0x0000_0400          256 B      BIOS data area (do not touch)
0x0000_0500          4 KB       E820 memory map table (built by stage 2)
0x0000_7C00          512 B      Stage 1 bootloader (MBR)
0x0000_7000          4 KB       Boot stack (real mode)
0x0000_8000          96 KB      Stage 2 bootloader binary
0x0002_0000          4 × 4 KB   Initial page tables (PML4, PDPT, PD, PT)
0x0008_0000          384 KB     Extended BIOS data area / VGA buffers (avoid)
0x0010_0000          varies     Kernel ELF binary (loaded here physically)
0x0100_0000+         free       Available physical RAM for kernel use
```

## E820 Memory Map Processing

The E820 map is collected in stage 2 and stored at `0x500`. The kernel's physical memory manager reads this map during early initialization (`pmm_init`) and:

1. Marks all regions of type 1 (usable) as free.
2. Marks all other regions as reserved or ACPI-specific.
3. Reclaims bootloader regions (stage 1, stage 2, boot page tables, boot stack, E820 table) as free after the kernel's own page tables are established.

## Initial Page Table Layout

The boot page tables established by stage 2 are temporary. The kernel replaces them with its own page tables during early initialization. The boot tables provide:

- **Identity map**: `0x0 → 0x0` for the first 4 GB (needed so stage 2 code continues running after enabling paging).
- **Higher-half direct map**: `0xFFFF_8000_0000_0000 → 0x0` for the first 64 GB.
- **Kernel image map**: `0xFFFF_FFFF_8000_0000 → [kernel load physical base]` for a 512 MB window.

The identity map is removed after the kernel establishes its own page tables.

## Framebuffer

If VESA BIOS extensions (VBE) are available, stage 2 queries the framebuffer mode before entering protected mode. The framebuffer physical address, width, height, and bits-per-pixel are stored in `BootInfo.fb` and used by the kernel's early framebuffer driver for pre-GUI display output.

## Related Documents

- [overview.md](overview.md)
- [stage2.md](stage2.md)
- [architecture/memory-layout.md](../architecture/memory-layout.md)
