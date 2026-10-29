# Kernel Overview

> This page is about **Fr Core**, the kernel. Fr Core is one component of
> **Fr OS**; it is not the system. A panic in Fr Core does not mean Fr Boot or
> the firmware are down, and the kernel's own diagnostics say "Fr Core" for
> exactly that reason. `KERNEL_VERSION_STRING` is `FR_CORE_NAME " " KERNEL_VERSION`.

## Architecture

The kernel is a **monolithic kernel** with the following characteristics:

- Core subsystems (memory, scheduler) run in ring 0.
- Device driver functionality is split: a minimal core driver runs in ring 0, the bulk of driver logic runs in ring 3 as a userspace process.
- The kernel is **preemptible** outside of critical sections (spinlocks, interrupt handlers). Preemption is timer-driven via `need_resched`; there is no voluntary preemption from mutexes or RCU.
- Kernel threads and user threads share the same scheduling infrastructure.

The following subsystems are designed into the kernel: VFS, networking stack, IPC
(pipes/shared memory), mutexes, RCU, loadable kernel modules, and security
(capabilities/seccomp/namespaces).

## Source Layout

```
src/kernel/
├── core/       entry (kmain.S), main.c, panic, kprintf, kstring, klog
├── arch/       cpu_features, gdt, idt, percpu, context.S,
│               interrupt_entry.S, syscall_entry.S
├── mm/         pmm, vmm, address spaces (mm), kmalloc, ELF loader
├── sched/      scheduler, tasks, timekeeping
├── console/    console (VGA, framebuffer, ANSI) and tty
├── proc/       process lifecycle, file/fd layer, syscall dispatch
├── drivers/    kernel-side drivers (serial, PS/2 keyboard, font)
├── include/    kernel headers
└── link.ld     linker script
```

The build discovers kernel sources recursively, so a new file in any of these
folders is picked up without a Makefile edit; object paths mirror the source
paths under `build/obj/`.

## Core Kernel Components

| Component | File(s) | Description |
|---|---|---|
| Interrupt handling | `arch/idt.c` | IDT, PIC remap/EOI, per-CPU interrupt stacks |
| Context switching | `arch/context.S` | Save/restore GPRs, scheduling metadata |
| Privilege levels | `arch/gdt.c` | GDT segments, TSS, ring transitions |
| Syscall entry | `arch/syscall_entry.S` | SYSCALL entry and register frame |
| Syscall dispatch | `proc/syscall.c` | Dispatch table and handlers |
| Process lifecycle | `proc/process.c` | fork/exec/wait/init, address-space setup |
| Files and descriptors | `proc/file.c` | File objects, console file, fd table |
| Panic system | `core/panic.c` | Panic handler, diagnostic dump, halt |
| Kernel logging | `core/klog.c` | Structured log with severity levels |
| Early boot | `core/main.c`, `core/kmain.S` | Sequenced subsystem initialization |

## Kernel Object Model

Kernel objects (processes, threads, files, sockets, etc.) are reference-counted. The reference count is managed with atomic operations. Objects are allocated from the SLAB allocator. Destruction is deferred to a safe point (no outstanding references, not in interrupt context).

## Locking Discipline

| Lock type | Use case |
|---|---|
| Spinlock | Short critical sections in interrupt context or between CPUs |
| **Mutex** | Long-held locks that allow sleeping (process context only) |
| **RW lock** | Reader-heavy data (dentry cache, routing table) |
| **RCU** | Read-mostly with infrequent updates (module list, process list) |
| Per-CPU | Data accessed only by one CPU (run queue, SLAB magazine) — no lock needed |

Spinlocks disable preemption for their duration.

## Preemption Model

The kernel is preemptible with preemption disabled only in:

- Spinlock critical sections
- Interrupt handlers
- NMI handlers

Preemption is implemented via a per-thread `preempt_count`. When `preempt_count` drops to zero and a reschedule flag is set (by the timer tick), the scheduler runs at the next safe point.

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
