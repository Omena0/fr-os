
# OS — Project Documentation

A POSIX-compatible, Linux-like operating system designed for QEMU, implementing a complete kernel, userspace environment, and system services from scratch.

---

## Quick Start (QEMU)

```bash
make all
./run.sh
```

See [build/qemu-setup.md](build/qemu-setup.md) for full QEMU configuration details.

---

## System Overview

The OS is structured as a monolithic kernel with optional loadable modules, a hybrid device driver architecture (minimal kernel-space core + userspace drivers), a full POSIX userspace, and an in-tree init system. All subsystems define an explicit fast path, a fallback path, and a concurrency strategy.

Target: x86-64, multi-core (tested on QEMU with KVM, 18 vCPUs, 4 GB RAM).

---

## Documentation Index

| Directory | Description |
|---|---|
| [architecture/](architecture/overview.md) | Top-level system design, subsystem relationships, ABI contracts, POSIX compliance scope, and the performance mandate |
| [bootloader/](bootloader/overview.md) | Two-stage bootloader: stage1 (MBR/BIOS), stage2 (protected mode, kernel load, handoff) |
| [kernel/](kernel/overview.md) | Core kernel: interrupt handling, context switching, privilege levels, module system, panic, logging |
| [scheduling/](scheduling/overview.md) | CPU scheduler: MLFQ (priority queues, aging, CPU accounting), real-time class, multicore load balancing, work-stealing |
| [memory/](memory/overview.md) | Allocator hierarchy: buddy allocator → SLAB → virtual memory → userspace malloc; NUMA, huge pages, reclamation |
| [filesystem/](filesystem/overview.md) | VFS abstraction layer, ext4-compatible implementation (journaling, extents), page/inode/dentry caches |
| [ipc/](ipc/overview.md) | IPC primitives: pipes (zero-copy ring-buffer), shared memory (with sync), namespace isolation |
| [drivers/](drivers/overview.md) | Hybrid driver model: kernel-space core + userspace drivers; display, input, storage; hotplug and isolation |
| [security/](security/overview.md) | User/group model, capabilities, syscall filtering, ASLR, NX, stack canaries, namespaces |
| [networking/](networking/overview.md) | IPv4/TCP stack, BSD socket API, raw sockets, low-copy buffering, userspace networking offload |
| [syscalls/](syscalls/overview.md) | Stable versioned ABI, low-latency dispatch path, per-domain syscall tables (process, memory, file, IPC, socket, scheduling) |
| [userspace/](userspace/overview.md) | Init system, service lifecycle, POSIX libc (IO, memory, threading), GUI program |
| [debugging/](debugging/overview.md) | Kernel tracing, event logging, performance counters, serial diagnostics, observability API |
| [build/](build/overview.md) | Toolchain setup, Makefile structure, QEMU configuration, cross-compilation, testing |

---

## Key Design Decisions

- **Language**: Kernel and bootloader in C (C11) + x86-64 assembly. Userspace in C with POSIX libc.
- **ABI**: Syscall ABI is versioned and stable; userspace code never needs to be rebuilt for kernel updates within a major version.
- **Scheduler**: Unified scheduling model — kernel threads and user threads are scheduled identically by the MLFQ. Real-time threads occupy a separate priority class above all MLFQ queues.
- **Memory**: Physical → virtual boundary is strictly enforced. Userspace never addresses physical memory directly.
- **Drivers**: Userspace drivers communicate with the kernel via shared memory regions and event queues mapped through a stable driver ABI. A crash in a userspace driver does not panic the kernel.
- **Security**: Default-deny capability model. All processes start with a minimal capability set; privileges are explicitly granted, never inherited implicitly.
- **Networking**: The TCP/IP stack lives in kernel space but exposes a userspace networking offload interface for high-performance applications.

---

## Component Dependency Map

```tree
Hardware
  └─ Bootloader (stage1 → stage2)
       └─ Kernel (ELF loaded by stage2)
            ├─ Memory Management (buddy → SLAB → VM → malloc)
            ├─ Scheduler (MLFQ + RT + multicore)
            ├─ Interrupt Handling → Context Switching
            ├─ Syscall Dispatch → POSIX ABI
            ├─ VFS → ext4 → page cache
            ├─ IPC (pipes, shared memory)
            ├─ Device Driver Framework → drivers (display, input, storage)
            ├─ Networking (IPv4/TCP/socket)
            └─ Init (PID 1) → userspace services → GUI
```

---

## POSIX Compliance Scope

Full compliance targets: process model (`fork`, `exec`, `wait`, `clone`), file descriptor abstraction, signal semantics, POSIX IO, POSIX threads (pthreads), and socket API. See [architecture/posix-compliance.md](architecture/posix-compliance.md) for the full conformance table.

---

## Performance Mandate

Every subsystem documents a **fast path**, a **degraded path**, and a **concurrency strategy**. See [architecture/performance-mandate.md](architecture/performance-mandate.md) for the system-wide performance requirements and per-subsystem targets.
