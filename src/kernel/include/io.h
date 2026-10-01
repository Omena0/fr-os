/*
 * io.h — port I/O, interrupt control, and CPU feature detection.
 *
 * These are the primitives everything else in the kernel is built on, so they
 * are defined inline in one header rather than scattered as inline assembly
 * across the tree. The x86 port I/O instructions are not serialising by
 * themselves; every outb/inb here carries a compiler barrier so the compiler
 * cannot reorder memory operations across a device register access, which
 * matters for the very first writes to devices the kernel is still bringing up.
 */
#ifndef IO_H
#define IO_H

#include <types.h>

/* ------------------------------------------------------- port I/O ----------- */

static inline void outb(uint16_t port, uint8_t val)
{
	__asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port) : "memory");
}

static inline void outw(uint16_t port, uint16_t val)
{
	__asm__ volatile("outw %0, %1" :: "a"(val), "Nd"(port) : "memory");
}

static inline void outl(uint16_t port, uint32_t val)
{
	__asm__ volatile("outl %0, %1" :: "a"(val), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port)
{
	uint8_t val;
	__asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port) : "memory");
	return val;
}

static inline uint16_t inw(uint16_t port)
{
	uint16_t val;
	__asm__ volatile("inw %1, %0" : "=a"(val) : "Nd"(port) : "memory");
	return val;
}

static inline uint32_t inl(uint16_t port)
{
	uint32_t val;
	__asm__ volatile("inl %1, %0" : "=a"(val) : "Nd"(port) : "memory");
	return val;
}

/*
 * The ISA "delay" idiom. Writing to port 0x80 costs a full I/O cycle on real
 * legacy hardware, which is what old UART and keyboard controllers need between
 * register writes. QEMU models it too, so this stays useful under emulation.
 */
static inline void io_wait(void)
{
	outb(0x80, 0);
}

/* ---------------------------------------------------- interrupts ----------- */

/*
 * Read-modify-write on RFLAGS is not atomic on x86: a concurrent interrupt or a
 * migration between the read and the write can lose the update. Clearing IF
 * around the whole sequence makes cli/sti the correct construction, and the
 * inline asm keeps it to one instruction pair rather than a function call that
 * the compiler might otherwise reorder.
 */
static inline void cli(void)
{
	__asm__ volatile("cli" ::: "memory");
}

static inline void sti(void)
{
	__asm__ volatile("sti" ::: "memory");
}

static inline bool irqs_enabled(void)
{
	uint64_t flags;
	__asm__ volatile("pushfq\n\tpopq %0" : "=r"(flags) :: "memory");
	return (flags & (1 << 9)) != 0;
}

/* Save the current interrupt state and disable interrupts, returning the old
 * state. This is the correct way to take a short critical section. */
static inline u64 irq_save(void)
{
	u64 flags;

	__asm__ volatile(
		"pushfq\n\t"
		"popq %0\n\t"
		"cli"
		: "=r"(flags)
		:
		: "memory");
	return flags;
}

static inline void irq_restore(u64 flags)
{
	if (flags & (1 << 9))
		__asm__ volatile("sti" ::: "memory");
}

/* Halt until the next interrupt. Used by idle threads and panic paths. */
static inline void hlt(void)
{
	__asm__ volatile("hlt");
}

/* Full memory barrier. x86 loads and stores are already ordered, so this only
 * has to keep the compiler from reordering. */
static inline void cpu_barrier(void)
{
	__asm__ volatile("" ::: "memory");
}

/* Serialising fence: also waits for the instruction stream to drain, which is
 * required before writing timestamp counters whose reads must be ordered. */
static inline void cpu_serialize(void)
{
	__asm__ volatile("mfence" ::: "memory");
}

static inline void pause_cpu(void)
{
	/* PAUSE hints to the CPU that the thread is in a spin-wait loop, letting
	 * it back off memory-order violation penalties and yield SMT resources. */
	__asm__ volatile("pause");
}

/* ------------------------------------------------------ control registers --- */

static inline u64 read_cr0(void)
{
	u64 v;
	__asm__ volatile("movq %%cr0, %0" : "=r"(v));
	return v;
}

static inline u64 read_cr2(void)
{
	u64 v;
	__asm__ volatile("movq %%cr2, %0" : "=r"(v));
	return v;
}

static inline u64 read_cr3(void)
{
	u64 v;
	__asm__ volatile("movq %%cr3, %0" : "=r"(v));
	return v;
}

static inline u64 read_cr4(void)
{
	u64 v;
	__asm__ volatile("movq %%cr4, %0" : "=r"(v));
	return v;
}

static inline void write_cr0(u64 v)
{
	__asm__ volatile("movq %0, %%cr0" :: "r"(v) : "memory");
}

static inline void write_cr3(u64 v)
{
	__asm__ volatile("movq %0, %%cr3" :: "r"(v) : "memory");
}

static inline void write_cr4(u64 v)
{
	__asm__ volatile("movq %0, %%cr4" :: "r"(v) : "memory");
}

/* Invalidates the TLB entry for one virtual address. */
static inline void invlpg(virt_addr_t addr)
{
	__asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
}

/*
 * Reload CR3, flushing the entire non-global TLB.
 *
 * On a machine with PCID this can replace CR3 with the PCID preserved, so the
 * replacement only invalidates entries belonging to the old PCID. The caller
 * must have already checked CR4.PCIDE; using the PCID form without it produces
 * a #GP.
 */
static inline void flush_tlb(void)
{
	write_cr3(read_cr3());
}

/* ---------------------------------------------------------------- MSRs ----- */

/*
 * Read the time stamp counter.
 *
 * RDTSC is not serialising: the instructions around it can be reordered across
 * it, so a single reading is not a reliable upper bound on elapsed cycles. That
 * matters for short measurements, and it is why everything that measures
 * duration here reads twice and lets the surrounding barriers do the work
 * rather than trusting one sample.
 *
 * LFENCE is used rather than CPUID for the ordering because CPUID costs
 * hundreds of cycles and serialises the whole pipeline, which is ruinous in
 * the scheduler tick and the deadline timer paths. RDTSCP would be stronger,
 * but it is not guaranteed present; RDTSC plus LFENCE is available on every
 * x86-64 part, which is the only portability claim this kernel makes.
 */
static inline u64 rdtsc(void)
{
	u32 lo, hi;

	__asm__ volatile("lfence\n\trdtsc"
			 : "=a"(lo), "=d"(hi)
			 :
			 : "memory");
	return ((u64)hi << 32) | lo;
}

/* Read the TSC as it appears on another CPU, for skew measurement. */
static inline u64 rdtscp(u32 *aux)
{
	u32 lo, hi, a;

	__asm__ volatile("rdtscp"
			 : "=a"(lo), "=d"(hi), "=c"(a)
			 :
			 : "memory");
	if (aux)
		*aux = a;
	return ((u64)hi << 32) | lo;
}

/*
 * Model specific registers. RDMSR/WRMSR fault in ring 3 and are serialising,
 * so they are only ever reached from kernel code.
 */
#define MSR_EFER        0xC0000080
#define MSR_STAR        0xC0000081
#define MSR_LSTAR       0xC0000082
#define MSR_SFMASK      0xC0000084
#define MSR_FS_BASE     0xC0000100
/*
 * IA32_GS_BASE is the GS base in long mode at CPL 0, and the only one that
 * percpu code reads through.
 *
 * IA32_KERNEL_GS_BASE (0xC0000102) is a different register: it is where the
 * kernel's GS base is parked across a `swapgs`, and the CPU consults it only
 * while executing at CPL 3. Writing a per-CPU base there from ring 0 has no
 * effect on the GS base that GS-relative addressing actually uses, so the
 * write succeeds, reads back whatever was there, and the per-CPU pointer is
 * silently never updated.
 */
#define MSR_GS_BASE      0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102
#define MSR_TSC_AUX     0xC0000103
#define MSR_APIC_BASE   0x1B
#define MSR_MISC_ENABLE 0x1A0
#define MSR_IA32_TSC_ADJUST 0x3B
#define MSR_IA32_TSX_CTRL 0x3AB

/* EFER bits. */
#define EFER_SCE  (1 << 0)   /* System call extensions */
#define EFER_LME  (1 << 8)   /* Long mode enable */
#define EFER_LMA  (1 << 10)  /* Long mode active */
#define EFER_NXE  (1 << 11)  /* No-execute page support */

static inline u64 rdmsr(uint32_t msr)
{
	u32 lo, hi;

	__asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
	return ((u64)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, u64 val)
{
	__asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)val),
			 "d"((uint32_t)(val >> 32)));
}

/* --------------------------------------------------------------- CPUID ----- */

/*
 * CPUID is the only way to discover what the processor can do. The leaf and
 * subleaf selectors go in EAX and ECX; results come back in EAX, EBX, ECX, EDX.
 *
 * Leaf 0 returns the maximum supported leaf in EAX and a 12-character vendor
 * string spread across EBX, EDX, ECX — which is why this cannot be a plain
 * struct return.
 */
static inline void cpuid(uint32_t leaf, uint32_t subleaf,
			 uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
	__asm__ volatile("cpuid"
			 : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
			 : "a"(leaf), "c"(subleaf));
}

#define CPUID_MAX_LEAF        0x00000000u
#define CPUID_VENDOR           0x00000000u
#define CPUID_FEATURES_1       0x00000001u
#define CPUID_MAX_EXT_LEAF     0x80000000u
#define CPUID_EXT_FEATURES     0x80000001u
#define CPUID_TSC              0x00000015u
#define CPUID_TSC_EXT          0x80000007u
#define CPUID_AMD_EXT_1        0x80000001u
#define CPUID_INTEL_EXT_1      0x00000001u

#endif /* IO_H */
