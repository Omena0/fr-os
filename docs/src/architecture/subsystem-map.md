# Subsystem Map

This document describes the dependency relationships between all major subsystems. A subsystem may only depend on subsystems listed below it in the initialization order.

## Initialization Order and Dependencies

```
1. Physical Memory Manager (buddy allocator)
   └─ depends on: hardware memory map (from bootloader)

2. Virtual Memory Manager
   └─ depends on: physical memory manager

3. SLAB Allocator
   └─ depends on: virtual memory manager

4. Interrupt Handling (IDT setup, per-CPU routing)
   └─ depends on: none (runs before most subsystems)

5. CPU Scheduler (MLFQ + RT)
   └─ depends on: interrupt handling, virtual memory

6. Kernel Module System
   └─ depends on: virtual memory, SLAB, scheduler

7. VFS Core
   └─ depends on: virtual memory, SLAB

8. ext4 Driver (registered with VFS)
   └─ depends on: VFS, block device layer

9. IPC Subsystem (pipes, shared memory)
   └─ depends on: VFS (for pipe file descriptors), virtual memory

10. Device Driver Framework
    └─ depends on: interrupt handling, virtual memory, IPC

11. Networking Stack (IPv4/TCP)
    └─ depends on: virtual memory, interrupt handling, device driver framework

12. Security Subsystem (capabilities, namespace, seccomp)
    └─ depends on: scheduler (process tracking), VFS (credential files)

13. Syscall Dispatch Layer
    └─ depends on: all kernel subsystems (bridges userspace to all of the above)

14. Init System (PID 1, userspace)
    └─ depends on: syscall layer, VFS (rootfs), IPC
```

## Cross-Subsystem Communication

| From | To | Mechanism |
|---|---|---|
| Scheduler | Memory Manager | Allocates per-thread stacks and kernel objects |
| VFS | Block Driver | Submits bio requests via block device queue |
| IPC (pipes) | VFS | Pipe endpoints are file descriptors in the FD table |
| Networking | Interrupt Handler | NIC interrupt triggers packet receive path |
| Userspace Driver | Kernel Driver Core | Shared memory region + event notification queue |
| Init | Kernel | fork/exec/wait syscalls; service state via /proc-like interface |
| Security | Scheduler | Capability checks on context switch and privilege escalation |

## Forbidden Dependencies

The following dependency directions are explicitly prohibited to maintain subsystem isolation:

- Memory Manager must not call into the Scheduler (no allocation that blocks on scheduling)
- Interrupt handlers must not call into VFS or Networking (no blocking in IRQ context)
- The block device layer must not call into the networking stack
- The IPC subsystem must not call into the networking socket subsystem

## Subsystem Boundaries

Each subsystem exposes a well-defined internal API to other kernel subsystems. This API is distinct from the userspace syscall API. Internal APIs are not stable across kernel versions and may change freely. Only the syscall ABI (documented in [abi-stability.md](abi-stability.md)) is externally stable.
