# Build System Overview

## What This Section Covers

This section documents the build system: how to set up the toolchain, compile the OS, run it under QEMU, and run tests.

## Prerequisites

- **NASM** ≥ 2.15 (for stage-1 bootloader assembly)
- **i686-elf-gcc** (for stage-2 bootloader 32-bit C code)
- **x86_64-elf-gcc** (for kernel C code)
- **x86_64-elf-ld** (for kernel linking)
- **QEMU** ≥ 6.0 (`qemu-system-x86_64`)
- **Make** ≥ 4.3

## Top-Level Targets

| Target | Description |
|---|---|
| `make all` | Build everything: bootloader, kernel, userspace, disk image |
| `make clean` | Remove all build artifacts |
| `make run` | Build and launch in QEMU |
| `make iso` | Build a bootable ISO image |
| `make test` | Run unit tests |
| `make check` | Run full test suite (unit + integration) |

## Output Artifacts

```
build/
  stage1.bin       — 512-byte MBR bootloader
  stage2.elf       — Stage-2 bootloader ELF
  kernel.elf       — Kernel ELF (debug symbols included)
  kernel.bin       — Kernel stripped binary
  libc.so.1        — C library
  ld.so            — Dynamic linker
  initrd.tar       — Initial RAM disk (programs + libraries)
  disk.img         — Bootable disk image (MBR + kernel + initrd)
  os.iso           — Bootable ISO (El Torito)
```

## Documents in This Section

| Document | Description |
|---|---|
| [toolchain.md](toolchain.md) | Cross-compiler setup |
| [makefile-structure.md](makefile-structure.md) | Makefile layout and targets |
| [qemu-setup.md](qemu-setup.md) | QEMU configuration and flags |
| [cross-compilation.md](cross-compilation.md) | Sysroot and cross-compilation details |
| [testing.md](testing.md) | Unit and integration testing |
