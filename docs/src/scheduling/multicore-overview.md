# Multicore Overview

## Design

The kernel targets full SMP (Symmetric Multi-Processing) support with per-CPU run queues, IPI-based coordination, and cross-CPU load balancing.

## Per-CPU Data Structures

- **Per-CPU data**: `struct percpu_data percpu_data[MAX_CPUS]` with per-CPU GS base, kernel stack, TSS, and run queue pointer.
- **Per-CPU run queues**: `struct runqueue *sched_runqueues[MAX_CPUS]` — each CPU has its own run queue with MLFQ, RT, and deadline queues.
- **CPU affinity**: `cpumask_t` in `struct task` with `sched_set_affinity()` and `sched_select_cpu()`.
- **Migration**: `sched_migrate()` places tasks on a global migration list (`global_queue`); destination CPU adopts via `sched_drain_global()`.
- **IPI vectors**: `VECTOR_IPI_TLB_SHOOTDOWN` (240), `VECTOR_IPI_RESCHEDULE` (241), `VECTOR_IPI_HALT` (242) in `interrupt.h`.
- **Need-resched flags**: Per-CPU `need_resched[MAX_CPUS]` for cross-CPU preemption signaling.

## SMP Initialization

1. `apic_init()` configures the LAPIC on the BSP and discovers APs via ACPI/MADT.
2. `smp_init()` sends SIPI/SIPI to start secondary CPUs.
3. IPI send/receive is implemented for `VECTOR_IPI_RESCHEDULE` and `VECTOR_IPI_TLB_SHOOTDOWN`.
4. `sched_drain_global()` consumes the global migration list on each CPU's tick.
5. Per-CPU LAPIC timer setup provides local timer interrupts.

## Execution Model

On boot, the kernel starts on CPU 0 (BSP). All interrupts, scheduling, and userspace execution initially run on CPU 0. Per-CPU data structures are indexed by `this_cpu_id()`. As APs are brought online, they enter the scheduler and begin processing their own interrupts.

## Related Documents

- [per-core-runqueues.md](per-core-runqueues.md)
- [load-balancing.md](load-balancing.md)
- [work-stealing.md](work-stealing.md)
- [cpu-affinity.md](cpu-affinity.md)
- [kernel/interrupt-handling.md](../kernel/interrupt-handling.md)
