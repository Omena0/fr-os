# Bootloader Build

## Source Layout

```
src/boot/
├── stage1.asm          x86 real-mode assembly, exactly 512 bytes
├── stage2/
│   ├── entry.asm       Real-mode entry, A20, mode transition stubs
│   ├── e820.c          E820 memory map collection
│   ├── elf_loader.c    ELF64 loader
│   ├── serial.c        COM1 serial output
│   ├── vbe.c           VESA BIOS Extensions framebuffer query
│   ├── boot_info.h     BootInfo struct definition (shared with kernel)
│   └── linker.ld       Stage 2 linker script
```

## Toolchain Requirements

| Tool | Version | Purpose |
|---|---|---|
| `nasm` | ≥ 2.15 | Stage 1 assembly |
| `i686-elf-gcc` | ≥ 12.0 | Stage 2 C (real-mode 16-bit and 32-bit sections) |
| `x86_64-elf-gcc` | ≥ 12.0 | Stage 2 long-mode C sections |
| `x86_64-elf-ld` | ≥ 2.38 | Stage 2 linking |
| `objcopy` | ≥ 2.38 | Strip ELF headers for raw binary output |

## Build Commands

```makefile
# Stage 1
stage1.bin: src/boot/stage1.asm
    nasm -f bin -o $@ $<
    @test $$(wc -c < $@) -eq 512 || (echo "Stage 1 is not 512 bytes"; exit 1)

# Stage 2 (simplified; actual build splits 16-bit and 64-bit sections)
stage2.bin: src/boot/stage2/
    $(MAKE) -C src/boot/stage2 stage2.bin

# Disk image assembly
boot.img: stage1.bin stage2.bin kernel.elf
    dd if=/dev/zero of=$@ bs=1M count=64
    dd if=stage1.bin of=$@ conv=notrunc
    dd if=stage2.bin of=$@ bs=512 seek=1 conv=notrunc
    dd if=kernel.elf of=$@ bs=512 seek=128 conv=notrunc
```

## Sector Layout on Disk Image

```
Sector 0         Stage 1 (MBR, 512 bytes)
Sectors 1–127    Stage 2 (up to 63.5 KB)
Sector 128+      Kernel ELF binary
```

These offsets are hardcoded in both stage 1 (reads stage 2 from sector 1) and stage 2 (reads kernel from sector 128). They are defined as constants in `src/boot/config.h`, which is included by both the bootloader and the build system.

## Verification

After building, verify the disk image:

```bash
# Check stage 1 signature
hexdump -C boot.img | head -2   # Last 2 bytes of sector 0 must be 55 AA

# Check stage 2 entry magic (optional debug marker)
hexdump -C boot.img | sed -n '2p'

# Check kernel ELF magic at sector 128
dd if=boot.img bs=512 skip=128 count=1 | hexdump -C | head -1
# Should show: 7f 45 4c 46 (ELF magic)
```

## QEMU Fast Path (Skipping Bootloader)

For rapid kernel development, use QEMU's `-kernel` flag with the kernel ELF directly (Multiboot2 mode). See [multiboot.md](multiboot.md) and [build/qemu-setup.md](../build/qemu-setup.md).

## Related Documents

- [overview.md](overview.md)
- [stage1.md](stage1.md)
- [stage2.md](stage2.md)
- [build/toolchain.md](../build/toolchain.md)
- [build/makefile-structure.md](../build/makefile-structure.md)
