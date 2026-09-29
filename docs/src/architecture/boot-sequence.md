# Boot Sequence

## Overview

The full boot sequence from BIOS power-on to the first userspace process (`init`) proceeds through five distinct phases.

## Phase 1 — BIOS and Stage 1 Bootloader

**Entry point**: BIOS loads the first 512 bytes of the boot disk (MBR) to physical address `0x7C00` and transfers control.

**Actions**:
1. Stage 1 code runs in real mode (16-bit).
2. Reads the partition table and locates the active partition.
3. Loads stage 2 bootloader from the filesystem start sectors into memory at `0x8000`.
4. Transfers control to stage 2.

**Memory state**: Real mode. Only the first 640 KB is usable. No interrupts reconfigured yet.

See [bootloader/stage1.md](../bootloader/stage1.md).

## Phase 2 — Stage 2 Bootloader

**Entry point**: `0x8000`, 16-bit real mode.

**Actions**:
1. Queries BIOS for the full physical memory map (INT 15h, E820).
2. Enables the A20 line.
3. Loads the Global Descriptor Table (GDT) and switches to 32-bit protected mode.
4. Sets up initial page tables and switches to 64-bit long mode.
5. Locates the kernel ELF binary on disk (fixed offset or simple filesystem scan).
6. Loads kernel ELF segments into the higher-half virtual address space (`0xFFFFFFFF80000000+`).
7. Passes a boot information structure to the kernel (memory map, VESA/framebuffer info, ACPI RSDP pointer).
8. Transfers control to the kernel entry point.

See [bootloader/stage2.md](../bootloader/stage2.md) and [bootloader/kernel-handoff.md](../bootloader/kernel-handoff.md).

## Phase 3 — Early Kernel Initialization

**Entry point**: `kernel_main()`, 64-bit long mode, running at higher-half address.

**Actions** (strict order):
1. Initialize kernel serial output for early diagnostics.
2. Parse the boot information structure (memory map, framebuffer).
3. Initialize the physical memory manager from the memory map.
4. Initialize the virtual memory manager and switch to the kernel page table.
5. Initialize the SLAB allocator.
6. Initialize the interrupt descriptor table (IDT) and configure the APIC.
7. Bring up the CPU scheduler.
8. Initialize the kernel module system.
9. Run compiled-in driver probes.
10. Mount the root filesystem.

All actions in this phase run on CPU 0, single-threaded, with interrupts disabled until the IDT is ready.

See [kernel/early-boot.md](../kernel/early-boot.md).

## Phase 4 — Secondary CPU Bring-Up (SMP)

**Trigger**: After phase 3, CPU 0 sends INIT/SIPI to all other CPUs via the APIC.

**Actions per secondary CPU**:
1. Each AP (Application Processor) starts in 16-bit real mode at a trampoline page.
2. Switches to 64-bit mode using the already-established page tables.
3. Initializes its own per-CPU data structures (run queue, interrupt stack, SLAB magazine).
4. Registers with the scheduler.
5. Enters the idle loop and becomes available for scheduling.

## Phase 5 — Init and Userspace

**Trigger**: After all CPUs are online and the root filesystem is mounted.

1. Kernel spawns the first userspace process: `/sbin/init` (PID 1) via `kernel_exec`.
2. `init` reads its service definition files, brings up required services in dependency order.
3. Services start: device drivers (userspace), networking, display server.
4. The GUI program starts when the display stack is ready.

See [userspace/init-system.md](../userspace/init-system.md).

## Timing Summary

| Phase | Expected duration (QEMU KVM) |
|---|---|
| Stage 1 | < 1 ms |
| Stage 2 | < 50 ms |
| Early kernel init | < 200 ms |
| SMP bring-up | < 100 ms |
| Init and services | < 2 s |
| GUI ready | < 3 s |
