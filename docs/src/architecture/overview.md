# Architecture Overview

> **Fr OS** is the project. **Fr Core** is the kernel. Everything on this page is
> about the whole system unless it says otherwise. Where a page describes a
> component, it names that component: `Fr Boot` (bootloader), `Fr Init` (first
> userspace process), `Fr Libc` (C runtime), `Fr Userland` (programs). The names
> are macros in `src/include/version.h`.

## System Philosophy

This OS is a POSIX-compatible, Linux-like operating system targeting x86-64 hardware, running under QEMU with KVM acceleration. It is designed with three primary constraints:

1. **Correctness**: Full POSIX semantics — process model, file descriptor abstraction, signal handling, IPC, and socket API must behave identically to specification.
2. **Performance**: Every subsystem defines an explicit fast path optimized for the common case. Slow paths are explicitly documented and bounded.
3. **Isolation**: Subsystem boundaries are strict. A failure in one subsystem (especially userspace drivers) must not cascade into kernel corruption or system panic.

## Structural Layers

```
┌─────────────────────────────────────────────────┐
│  Userspace Applications (GUI, shell, services)  │
├─────────────────────────────────────────────────┤
│  POSIX libc (syscall wrappers, stdlib, pthreads)│
├─────────────────────────────────────────────────┤
│  Syscall Boundary (versioned ABI)               │
├──────────────┬───────────────┬──────────────────┤
│  Scheduler   │ Memory Manager│  VFS / Filesystem│
│  (MLFQ + RT) │ (buddy→malloc)│  (ext4 + cache)  │
├──────────────┼───────────────┼──────────────────┤
│  IPC         │  Networking   │  Device Drivers  │
│  (pipe, shm) │  (IPv4/TCP)   │  (hybrid model)  │
├──────────────┴───────────────┴──────────────────┤
│  Core Kernel (IRQ, context switch, modules)     │
├─────────────────────────────────────────────────┤
│  Bootloader (stage1 + stage2)                   │
├─────────────────────────────────────────────────┤
│  Hardware / QEMU                                │
└─────────────────────────────────────────────────┘
```

## Core Components

| Component | Role |
|---|---|
| Bootloader | BIOS/MBR stage1, protected-mode stage2, kernel ELF loader |
| Kernel | Monolithic core: IRQ, scheduling, memory, VFS, IPC, networking, security |
| Module System | Loadable kernel modules with versioned symbols and runtime lifecycle |
| Init System | PID 1; service lifecycle, dependency ordering, restart policy |
| libc | POSIX-compatible C library backed entirely by kernel syscalls |
| Userspace drivers | Per-device userspace processes communicating via shared memory + event queues |
| GUI program | Minimal windowing application running on the framebuffer |

## Design Principles

- **Monolithic kernel with hybrid drivers**: The kernel is monolithic for performance, but device drivers beyond minimal core functionality run in userspace with isolated fault domains.
- **Strict ABI versioning**: The syscall ABI is versioned. Userspace binaries compiled against version N continue to work after kernel updates within the same major version.
- **Zero-copy where possible**: Pipes use ring-buffer based channels. Networking uses low-copy buffer strategies. Shared memory IPC avoids copies entirely.
- **Preemptible kernel**: The kernel is fully preemptible outside of critical sections. Interrupt latency is bounded.
- **Default-deny security**: Processes start with a minimal capability set. No ambient authority.

## Related Documents

- [subsystem-map.md](subsystem-map.md) — dependency graph of all subsystems
- [abi-stability.md](abi-stability.md) — syscall ABI versioning and stability guarantees
- [posix-compliance.md](posix-compliance.md) — POSIX conformance scope
- [performance-mandate.md](performance-mandate.md) — system-wide performance requirements
- [boot-sequence.md](boot-sequence.md) — full boot flow from BIOS to init
- [memory-layout.md](memory-layout.md) — physical and virtual address space layout
- [security-model.md](security-model.md) — threat model and security architecture
