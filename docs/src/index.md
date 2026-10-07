
# Fr OS — Project Documentation

**Fr OS** is a POSIX-oriented, Linux-shaped hobby operating system for QEMU,
written from scratch: a bootloader, a kernel, a C runtime and a userspace.

The project is assembled from six named components. The names are macros in
`src/include/version.h`, so no banner, panic message or init line can drift from
another:

| Component | What it is |
|---|---|
| **Fr OS** | The project as a whole |
| **Fr Core** | The kernel |
| **Fr Boot** | The bootloader, which hands control to Fr Core |
| **Fr Init** | The first userspace process, and system bring-up |
| **Fr Libc** | The C runtime the kernel and Fr Userland share |
| **Fr Userland** | The programs that run on top of Fr Init |

**Fr OS is the project; Fr Core is the kernel.** They are not interchangeable. A
panic that says "Fr OS" would claim the whole system is down when only the kernel
is, so the kernel's banner and panic path print Fr Core. `KERNEL_VERSION_STRING`
is `FR_CORE_NAME " " KERNEL_VERSION`.

## References

- [Intel 64 and IA-32 Architectures Software Developer's Manual][intel-sdm]
- [AMD64 Architecture Programmer's Manual][amd-apm]
- [POSIX.1-2017 (IEEE Std 1003.1-2017)][posix-2017]
- [System V Application Binary Interface AMD64 Architecture Processor Supplement][sysv-abi]
- [Multiboot Specification][multiboot-spec]
- [QEMU Documentation][qemu-docs]

[intel-sdm]: https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html "Intel 64 and IA-32 Architectures Software Developer's Manual"
[amd-apm]: https://www.amd.com/en/developer/architecture-programmer-manuals.html "AMD64 Architecture Programmer's Manual"
[posix-2017]: https://pubs.opengroup.org/onlinepubs/9699919799/ "The Open Group Base Specifications Issue 7, 2018 edition (POSIX.1-2017)"
[sysv-abi]: https://gitlab.com/x86-psABIs/x86-64-ABI/-/blob/master/abi.md "System V AMD64 ABI"
[multiboot-spec]: https://www.gnu.org/software/grub/manual/multiboot/multiboot.html "GNU Multiboot Specification"
[qemu-docs]: https://www.qemu.org/docs/master/ "QEMU Documentation"

## Read this before you trust a page

This documentation tree describes three different things, mixed together:

1. **What the code does.** Trustworthy.
2. **What the code is meant to do.** Worth reading; the gap matters.
3. **What a fully-featured operating system would do.** Several subsystems —
   filesystems, networking, a GUI, ASLR, containers, compaction — are documented
   in the present tense with no code behind them at all.

Pages that describe category 3 say so. Where they do not, check
[`../../MEGA_AUDIT.md`](../../MEGA_AUDIT.md) before writing code against a claim.
It is a 177-finding source audit, and every finding carries a status line saying
whether it is still open.

The kernel boots; the system does not yet reach userspace. The audit's summary
lists what blocks it, in order.

---

## Quick Start (QEMU)

```bash
make clean && make -j8
./run.sh
```

`make clean` is not optional hygiene here. The dependency tracking in this tree
has failed to rebuild an object after a header edit, and a stale object once
looked like a memory-management bug for hours.

See [build/qemu-setup.md](build/qemu-setup.md) for full QEMU configuration.

---

## System Overview

Fr Core is a monolithic kernel. Target: x86-64, single core today, booted under
QEMU with 4 GB of RAM.

Implemented and working: the boot chain, a 4 GiB direct map with 2 MiB pages, a
higher-half kernel window, a buddy physical allocator, a SLAB allocator with
per-CPU magazines, `vmalloc`, VMA management, the ELF loader, GDT/TSS/IDT, a
panic path, a console with VGA and serial backends, a libc, and a userspace
binary.

Designed but not working: the scheduler's context switch and syscall return path
(both have offset bugs the audit records), timers, `mmap`/`brk`, signals, and
init.

Not present at all: filesystems, networking, device drivers beyond serial, ASLR,
NX on the kernel's own mappings, modules, security primitives.

---

## Documentation Index

| Directory | Description |
|---|---|
| [architecture/](architecture/overview.md) | Top-level system design, subsystem relationships, ABI contracts, POSIX compliance scope, and the performance mandate |
| [bootloader/](bootloader/overview.md) | Fr Boot: stage1 (MBR/BIOS), stage2 (protected mode, kernel load, handoff) |
| [kernel/](kernel/overview.md) | Fr Core: interrupt handling, context switching, privilege levels, module system, panic, logging |
| [scheduling/](scheduling/overview.md) | CPU scheduler: MLFQ (priority queues, aging, CPU accounting), real-time class, multicore load balancing, work-stealing |
| [memory/](memory/overview.md) | Allocator hierarchy: buddy allocator → SLAB → virtual memory → userspace malloc; NUMA, huge pages, reclamation |
| [filesystem/](filesystem/overview.md) | VFS abstraction layer, ext4-compatible implementation (journaling, extents), page/inode/dentry caches |
| [ipc/](ipc/overview.md) | IPC primitives: pipes (zero-copy ring-buffer), shared memory (with sync), namespace isolation |
| [drivers/](drivers/overview.md) | Hybrid driver model: kernel-space core + userspace drivers; display, input, storage; hotplug and isolation |
| [security/](security/overview.md) | User/group model, capabilities, syscall filtering, ASLR, NX, stack canaries, namespaces |
| [networking/](networking/overview.md) | IPv4/TCP stack, BSD socket API, raw sockets, low-copy buffering, userspace networking offload |
| [syscalls/](syscalls/overview.md) | Stable versioned ABI, low-latency dispatch path, per-domain syscall tables (process, memory, file, IPC, socket, scheduling) |
| [userspace/](userspace/overview.md) | Fr Init, Fr Libc (IO, memory, threading), GUI program |
| [debugging/](debugging/overview.md) | Kernel tracing, event logging, performance counters, serial diagnostics, observability API |
| [build/](build/overview.md) | Toolchain setup, Makefile structure, QEMU configuration, cross-compilation, testing |

---

## Key Design Decisions

- **Language**: Fr Core and Fr Boot in C (C11) + x86-64 assembly. Fr Userland in
  C with Fr Libc.
- **ABI**: the syscall ABI is *intended* to be versioned and stable. It is not
  yet: there is no version negotiation at entry, no compat layer, and no userspace
  binary built against the shipped header can run on the shipped kernel — the
  libc needs a thread pointer the kernel never sets up.
- **Memory**: physical and virtual are kept strictly separate except for DMA
  allocations. The direct map covers 4 GiB.
- **Serial-first diagnostics**: the serial port is the primary observation
  channel. Every failure path before the framebuffer is reported over it.

---

## Component Dependency Map

```
Hardware
  └─ Fr Boot (stage1 → stage2)
       └─ Fr Core (ELF loaded by Fr Boot, entered at its link address)
            ├─ Memory Management (buddy → SLAB → vmalloc → VMA)
            ├─ Scheduler (MLFQ + RT)
            ├─ Interrupt Handling → Context Switching
            ├─ Syscall Dispatch → POSIX ABI
            ├─ Fr Libc (linked into the kernel and into userspace)
            └─ Fr Init (PID 1) → Fr Userland
```

---

## Performance Mandate

Every subsystem is supposed to document a **fast path**, a **degraded path** and a
**concurrency strategy**. See [architecture/performance-mandate.md](architecture/performance-mandate.md).
The honesty of those documents varies; see the audit.
