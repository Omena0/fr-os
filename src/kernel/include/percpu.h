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
#include <io.h>

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
 * IA32_GS_BASE.
 *
 * This is read with RDMSR, and that is not a stylistic choice -- it is the only
 * read of GS.base that is both correct and available here. Three candidates
 * were measured on the running kernel with the GS base set to
 * &percpu_data[0] = 0xffffffff8016d300:
 *
 *     rdmsr 0xC0000101          -> 0xffffffff8016d300   correct
 *     rdgsbase                  -> 0xffffffff8016d300   correct (not on Intel)
 *     48 8c /r  (mov %gs,%rax)  -> 0x10                 WRONG
 *
 * Three wrong ways to do this have been live in this file at various points,
 * and each of them fails silently:
 *
 *  1. `movq %%gs:0, %0` loads the eight bytes the GS base points *at* --
 *     percpu_data[0].cpu_id, which is 0 -- and returns it as the pointer.
 *  2. `movq %%gs, %0` looks like the fix and is worse. GAS ignores the `q` and
 *     emits the legacy 32-bit MOV r/m32, Sreg form (8c /r, no REX.W), so what
 *     the compiler emitted was `mov %gs,%eax`, which loads the GS *selector*:
 *     0x10 on this machine. Verified against the assembles this file is built
 *     with: `movq %gs,%rax` assembles to `8c e8`, and objdump renders that as
 *     `mov %gs,%eax`.
 *  3. Hand-writing `48 8c e8` to force REX.W is the third attempt, and QEMU TCG
 *     ignores REX.W for opcode 8C as well: it returned the same selector, 0x10.
 *     This is why kmain.S's read-back, which uses exactly those bytes, has to
 *     change too -- it aborts every boot with "GS base did not take" on a
 *     correctly installed base.
 *
 * None of the three faults. The value returned is not a pointer into a hole:
 * 0x10 is inside the bootloader's identity window, mapped and writable, so
 * there is no #PF to point at the mistake. It surfaces much later as an
 * allocator returning NULL for every request and a kernel triple-faulting on
 * the first dereference of the result, which is why this took a day to find.
 *
 * RDMSR is serialising, which is a real cost on the interrupt path. It buys
 * correctness on every CPU, cannot raise an unimplemented-instruction fault,
 * and adds no contention -- which is the property this design actually exists
 * for. RDGSBASE is the cheap alternative and does work under QEMU (measured
 * above), but it is an AMD extension with no CPUID bit of its own on Intel
 * parts, so using it means carrying a runtime capability flag and a second code
 * path for a saving that has not been measured to matter.
 */
static inline struct percpu_data *this_cpu(void)
{
	return (struct percpu_data *)(uintptr_t)rdmsr(MSR_GS_BASE);
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
 * before any subsystem is initialised. Panics if the base cannot be installed
 * or does not read back: there is no correct way to continue, because every
 * per-CPU access would resolve to whatever happened to be mapped at the
 * address this_cpu() returned.
 */
void percpu_setup(uint32_t cpu_id);

/* Total number of CPUs the kernel is managing. */
uint32_t percpu_num_cpus(void);

#endif /* PERCPU_H */
