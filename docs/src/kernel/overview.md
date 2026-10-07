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

The intended source layout groups subsystems into directories:

```
src/kernel/
├── *.c                 Core kernel files (main.c, idt.c, gdt.c, vmm.c, pmm.c, 
│                       kmalloc.c, sched.c, task.c, process.c, mm.c, syscall.c, 
│                       elf.c, tty.c, console.c, klog.c, panic.c, percpu.c, 
│                       cpu_features.c, module.c, ...)
├── *.h                 Public headers (interrupt.h, sched.h, vmm.h, pmm.h, 
│                       task.h, process.h, types.h, boot.h, syscall.h, ...)
├── include/            Internal kernel headers
├── drivers/            Driver sources (serial.c, keyboard.c, pci.c, ...)
└── arch/x86_64/        Architecture-specific assembly and C
    ├── *.c             (idt.c, gdt.c, apic.c, cpu.c)
    └── *.S             (context.asm, syscall_entry.S, interrupt_entry.S)
```

Most subsystems live as individual `.c` files directly in `src/kernel/` rather than
in subdirectories. The `mm/`, `sched/`, `fs/`, `ipc/`, `net/`, `security/`,
`module/`, and `lib/` directories are the target layout.

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
