# Kernel Overview

> This page is about **Fr Core**, the kernel. Fr Core is one component of
> **Fr OS**; it is not the system. A panic in Fr Core does not mean Fr Boot or
> the firmware are down, and the kernel's own diagnostics say "Fr Core" for
> exactly that reason. `KERNEL_VERSION_STRING` is `FR_CORE_NAME " " KERNEL_VERSION`.

## Architecture

The kernel is a **monolithic kernel** with the following characteristics:

- All core subsystems (memory, scheduler, VFS, networking, IPC) run in ring 0.
- Device driver functionality is split: a minimal core driver runs in ring 0, the bulk of driver logic runs in ring 3 as a userspace process.
- The kernel is **fully preemptible** outside of critical sections (spinlocks, interrupt handlers).
- Kernel threads and user threads share the same scheduling infrastructure.

## Source Layout

```
src/kernel/
├── main.c              kernel_main() — early init sequence
├── boot/               Early boot stubs, multiboot2 shim
├── arch/x86_64/        Architecture-specific code
│   ├── idt.c           Interrupt Descriptor Table setup
│   ├── gdt.c           Global Descriptor Table
│   ├── apic.c          Local and I/O APIC management
│   ├── context.asm     Context switch assembly stub
│   ├── syscall.asm     SYSCALL/SYSRET entry point
│   └── cpu.c           Per-CPU data, CPU feature detection
├── mm/                 Memory management
├── sched/              Scheduler
├── fs/                 VFS + ext4
├── ipc/                Pipes and shared memory
├── net/                Networking stack
├── drivers/            Kernel-side driver core
├── security/           Capabilities, seccomp, namespaces
├── module/             Loadable kernel module system
└── lib/                Internal kernel utility library
```

## Core Kernel Components

| Component | File(s) | Description |
|---|---|---|
| Interrupt handling | `arch/x86_64/idt.c`, `apic.c` | IDT, APIC routing, per-CPU interrupt stacks |
| Context switching | `arch/x86_64/context.asm` | Save/restore GPRs, SIMD, scheduling metadata |
| Privilege levels | `arch/x86_64/gdt.c` | GDT segments, TSS, ring transitions |
| Syscall dispatch | `arch/x86_64/syscall.asm` | SYSCALL entry, dispatch table, SYSRET |
| Module system | `module/` | ELF module loader, symbol table, versioning |
| Panic system | `lib/panic.c` | Panic handler, diagnostic dump, halt |
| Kernel logging | `lib/klog.c` | Structured log with severity levels |
| Early boot | `main.c`, `boot/` | Sequenced subsystem initialization |

## Kernel Object Model

Kernel objects (processes, threads, files, sockets, etc.) are reference-counted. The reference count is managed with atomic operations. Objects are allocated from the SLAB allocator. Destruction is deferred to a safe point (no outstanding references, not in interrupt context).

## Locking Discipline

| Lock type | Use case |
|---|---|
| Spinlock | Short critical sections in interrupt context or between CPUs |
| Mutex | Long-held locks that allow sleeping (process context only) |
| RW lock | Reader-heavy data (dentry cache, routing table) |
| RCU | Read-mostly with infrequent updates (module list, process list) |
| Per-CPU | Data accessed only by one CPU (run queue, SLAB magazine) — no lock needed |

Spinlocks disable preemption for their duration. Mutexes do not.

## Preemption Model

The kernel is preemptible with preemption disabled only in:

- Spinlock critical sections
- Interrupt handlers
- NMI handlers

Preemption is implemented via a per-thread `preempt_count`. When `preempt_count` drops to zero and a reschedule flag is set, the scheduler runs at the next safe point.

## Related Documents

- [interrupt-handling.md](interrupt-handling.md)
- [context-switching.md](context-switching.md)
- [privilege-levels.md](privilege-levels.md)
- [syscall-abi.md](syscall-abi.md)
- [module-system.md](module-system.md)
- [kernel-libraries.md](kernel-libraries.md)
- [panic-system.md](panic-system.md)
- [logging.md](logging.md)
- [early-boot.md](early-boot.md)
