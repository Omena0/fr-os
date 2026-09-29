/*
 * percpu.h — per-CPU data.
 *
 * Each CPU owns a slice of a single array. Accessing it is a GS-relative load
 * with no atomic operation and no lock: the offset of the current CPU's slice
 * is the only shared state involved, and that is what makes this pattern
 * appropriate for hot paths like the allocator's magazine refill or the
 * scheduler's run queue head.
 *
 * Correctness rests on two rules, both enforced by convention here:
 *
 *   1. A CPU may only ever touch its own slice. Reading another CPU's slice
 *      needs no lock only if that CPU is the only writer and the reader does
 *      not require a consistent snapshot.
 *   2. The current CPU id is cached in the GS base register, not in a memory
 *      variable. A memory variable would be a cache line shared by every CPU,
 *      which is precisely the contention this design exists to avoid.
 *
 * The GS base is set once per CPU in cpu_local_storage_setup() before any
 * subsystem is up. MSR_GS_BASE is used rather than the legacy GDT descriptor
 * approach because it needs no GDT slot and no reload on every syscall.
 */
#ifndef PERCPU_H
#define PERCPU_H

#include <types.h>

/*
 * Maximum CPUs. The per-CPU area is a statically reserved array so the GS base
 * can point at it before the allocator exists. 256 CPUs is far beyond what
 * QEMU will configure on the reference machine, but the array costs almost
 * nothing and a build-time bound is far better than a runtime allocation on the
 * boot path.
 */
#define MAX_CPUS 256

/*
 * The per-CPU area. Everything that must be reachable without a lock or an
 * array index lives here.
 */
struct percpu_data {
	volatile u32 cpu_id;          /* identity of this CPU, 0-based */
	volatile bool online;         /* registered with the scheduler */
	volatile bool in_scheduler;

	/* Scheduler. */
	void *current;                /* struct task * */
	void *idle_task;

	/* Interrupt bookkeeping: the flags word saved by every
	 * spinlock_irqsave() on this CPU. */
	u64 saved_irq_flags;

	/* Preemption depth. Non-zero means preemption is disabled, either by an
	 * explicit disable or by holding a lock. */
	u32 preempt_count;

	/* Scratch for the console lock, which has to be released with the exact
	 * interrupt state it was acquired under. */
	u64 console_irq_flags;

	/* Per-CPU idle statistics, for the scheduler's load balancer. */
	u64 idle_ticks;
	u64 ctx_switches;
};

/* The array itself. */
extern struct percpu_data percpu_data[MAX_CPUS];

/*
 * The per-CPU base. Written once per CPU at bring-up; after that it lives in
 * MSR_GS_BASE and is read with a single instruction.
 */
static inline struct percpu_data *this_cpu(void)
{
	struct percpu_data *p;

	__asm__ volatile("movq %%gs:0, %0" : "=r"(p));
	return p;
}

#define this_cpu_id()      (this_cpu()->cpu_id)
#define per_cpu(var)       (this_cpu()->var)
#define per_cpu_read(var)  (this_cpu()->var)
#define per_cpu_write(var, value) (this_cpu()->var = (value))

/* Per-CPU storage for a variable declared with DEFINE_PER_CPU. */
#define DEFINE_PER_CPU(type, name) \
	__attribute__((section(".per_cpu"))) type per_cpu_##name

/* Access a DEFINE_PER_CPU variable on this CPU. */
#define get_var(var) (per_cpu_##var)

/*
 * Set the GS base so this_cpu() resolves to `cpu`. Called exactly once per CPU
 * before any subsystem is initialised.
 */
void percpu_setup(uint32_t cpu_id);

/* Total number of CPUs the kernel is managing. */
uint32_t percpu_num_cpus(void);

#endif /* PERCPU_H */
