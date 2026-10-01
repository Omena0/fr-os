# Bootloader Overview

> This page is about **Fr Boot**, the bootloader. Fr Boot's job ends when it
> transfers control to **Fr Core**, the kernel, at the kernel's link address in
> its own page tables. It is not responsible for anything after that, and it
> cannot report a fault raised by the kernel.

## Role

The bootloader is responsible for:
1. Transitioning the CPU from BIOS 16-bit real mode to 64-bit long mode.
2. Loading the kernel ELF binary from disk into the higher-half virtual address space.
3. Collecting system information (memory map, framebuffer configuration, ACPI pointers) and packaging it into a boot information structure.
4. Transferring control to the kernel entry point.

## Architecture

The bootloader is split into two stages to work within the constraints of the MBR boot process.

```
Stage 1 (512 bytes, runs in real mode)
  └─ Stage 2 (runs in real mode → protected mode → long mode)
       └─ Kernel (ELF, loaded into higher-half address space)
```

## Stage 1

- Fits within the 512-byte MBR sector (446 bytes of code + 64-byte partition table + 2-byte signature).
- Written entirely in x86 assembly.
- Responsibility: load stage 2 from a known disk location and jump to it.
- No filesystem awareness — stage 2 is stored at a fixed sector offset following the MBR.

See [stage1.md](stage1.md).

## Stage 2

- Larger; loaded by stage 1 into low memory (`0x8000`).
- Performs the full mode transition sequence: real → protected → long mode.
- Queries BIOS E820 for physical memory map.
- Contains a minimal FAT32 or raw sector reader to locate the kernel image.
- Loads and parses the kernel ELF binary.
- Constructs the `BootInfo` structure.
- Jumps to kernel entry.

See [stage2.md](stage2.md).

## Boot Information Structure

The stage 2 bootloader passes a `BootInfo` structure to the kernel. Its layout is fixed and versioned:

```c
struct BootInfo {
    uint32_t magic;              // BOOT_MAGIC = 0xB007B007
    uint32_t version;            // Structure version
    struct E820Map memory_map;   // Physical memory map
    struct Framebuffer fb;       // Framebuffer geometry and address
    uint64_t rsdp_addr;          // ACPI RSDP physical address
    uint64_t kernel_load_base;   // Physical address where kernel was loaded
    uint64_t initrd_addr;        // Initial ramdisk address (if present)
    uint64_t initrd_size;
};
```

## Constraints

- The bootloader must not use or modify memory above `0x8_0000` before loading the kernel (this range is used for the kernel load target).
- The bootloader leaves the CPU in 64-bit long mode with paging enabled using a minimal identity + higher-half page table.
- All BIOS calls must be completed before switching to protected mode (BIOS services are unavailable in protected/long mode).

## Related Documents

- [stage1.md](stage1.md)
- [stage2.md](stage2.md)
- [memory-map.md](memory-map.md)
- [kernel-handoff.md](kernel-handoff.md)
- [multiboot.md](multiboot.md)
- [serial-output.md](serial-output.md)
- [build.md](build.md)
