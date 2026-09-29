# Kernel Handoff Protocol

## Contract Between Stage 2 and the Kernel

This document defines the exact state of the CPU and memory when stage 2 transfers control to `kernel_main`. This is a frozen interface: changes here require updating both the bootloader and the kernel.

## CPU State at `kernel_main` Entry

| Register / State | Value |
|---|---|
| `rdi` | Physical address of `BootInfo` structure |
| `rsi` | 0 (reserved, must be ignored) |
| `rsp` | Kernel early boot stack (`0xFFFF_FFFF_8010_0000`, 64 KB region) |
| `cs` | 64-bit code segment (GDT descriptor index 1, DPL=0) |
| `ds`, `es`, `ss` | 64-bit data segment (GDT descriptor index 2, DPL=0) |
| `cr0` | PE=1, PG=1, WP=1 |
| `cr3` | Boot PML4 physical address |
| `cr4` | PAE=1, PGE=1 |
| `EFER` | LME=1, LMA=1, NXE=1 |
| Interrupts | **Disabled** (IF=0) |
| Floating point | Uninitialized (kernel must not use FPU before `fpu_init`) |

## `BootInfo` Structure Layout

Version 1 layout (current):

```c
#define BOOT_MAGIC  0xB007B007
#define BOOT_VERSION 1

struct E820Entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;       // 1=usable, 2=reserved, 3=ACPI, 4=ACPI NVS
    uint32_t reserved;
};

struct Framebuffer {
    uint64_t phys_addr;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;      // bytes per scanline
    uint8_t  bpp;        // bits per pixel (typically 32)
    uint8_t  red_shift, red_mask;
    uint8_t  green_shift, green_mask;
    uint8_t  blue_shift, blue_mask;
};

struct BootInfo {
    uint32_t          magic;           // Must equal BOOT_MAGIC
    uint32_t          version;         // Must equal BOOT_VERSION
    struct E820Entry  memory_map[128]; // Up to 128 E820 entries
    uint32_t          memory_map_count;
    struct Framebuffer fb;
    uint64_t          rsdp_phys_addr;  // ACPI RSDP, 0 if not found
    uint64_t          kernel_phys_base;
    uint64_t          initrd_phys_addr;// 0 if no initrd
    uint64_t          initrd_size;
    uint8_t           _pad[64];        // Reserved for future fields
};
```

## Kernel Validation

`kernel_main` must validate the `BootInfo` structure on entry:

1. Check `magic == BOOT_MAGIC`. Panic with "invalid boot magic" if not.
2. Check `version == BOOT_VERSION`. Panic with "unsupported bootloader version" if not.
3. Verify `memory_map_count > 0`. Panic with "no memory map" if zero.

## Page Table Replacement

The kernel must establish its own page tables as soon as the physical and virtual memory managers are initialized. The boot page tables (set up by stage 2) are minimal and do not contain kernel heap regions or per-CPU mappings. The kernel switches CR3 to its own PML4 during `vmm_init`.

After the switch, the identity map from the boot tables is no longer present and the kernel runs entirely from its higher-half addresses.

## Related Documents

- [overview.md](overview.md)
- [stage2.md](stage2.md)
- [memory-map.md](memory-map.md)
- [kernel/early-boot.md](../kernel/early-boot.md)
