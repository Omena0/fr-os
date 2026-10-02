/*
 * percpu.c — per-CPU area setup.
 *
 * The GS base register is loaded with the address of this CPU's slice of the
 * per-CPU array. Because MSR_GS_BASE is a 64-bit MSR, this works in long mode
 * with no GDT entry and no reload: the value survives arbitrary nesting of
 * interrupts and syscalls.
 *
 * In long mode GS is not segment-relative, so the base *is* the value loaded
 * into GS. this_cpu() reads it out of MSR_GS_BASE with a single instruction.
 */

#include <percpu.h>
#include <io.h>
#include <klog.h>
#include <kstring.h>
#include <panic.h>

KLOG_SUBSYSTEM("percpu");

struct percpu_data percpu_data[MAX_CPUS] __aligned(64);
static uint32_t num_cpus;

/*
 * What the verification below found. Kept as a struct rather than a bare bool so
 * that a failure can say *which* check failed: the three are different
 * mechanisms, and which one broke is the whole diagnosis.
 */
struct gs_check {
	uint64_t want;		/* the address that was asked for */
	uint64_t msr;		/* what IA32_GS_BASE read back as */
	uint64_t fast;		/* what this_cpu() returned */
	uint32_t via_gs;	/* what a GS-relative load saw */
	int step;		/* 0 = ok, else the check that failed */
};

/*
 * Install `want` as GS.base, then prove it through three independent routes.
 *
 * Only checking one of them is what made this failure invisible for a day. The
 * route that was broken was never the one under test: WRMSR landed correctly
 * every boot, IA32_GS_BASE read back the exact value written, and this_cpu()
 * returned the GS *selector* (0x10) because `movq %%gs, %0` assembles to the
 * legacy 32-bit MOV r/m32, Sreg form. The MSR check passed; the per-CPU area was
 * still unreachable.
 *
 *   1. IA32_GS_BASE. The architectural register, and what RDGSBASE and GS-relative
 *      addressing both act on. Ground truth, and the only read here that is not
 *      itself an inline asm string that could be assembled into something else.
 *   2. this_cpu(). The exact function every caller uses. Today it *is* an rdmsr,
 *      so this is redundant by construction -- and that redundancy is the point:
 *      it is what makes the fast path's correctness a checked invariant rather
 *      than a coincidence. The moment this_cpu() stops being an rdmsr this check
 *      becomes the one that catches it, which is precisely when it is needed.
 *   3. GS-relative addressing. A `mov %%gs:0` through the segment override, which
 *      goes through the segment's effective base rather than through any MSR
 *      read instruction. This is the mechanism the interrupt and syscall paths
 *      actually use, and it is the only check that would notice if the hidden
 *      base and the effective base ever disagreed.
 *
 * WRMSR rather than WRGSBASE here. WRGSBASE needs CR4.FSGSBASE, and setting that
 * bit hands CPL 3 the ability to relocate the per-CPU area for itself, which the
 * syscall path then dereferences. kmain.S has already installed the same value
 * before this runs; this is the CPU-id-aware install, and the only one that knows
 * which slice of the array to point at. One serialising instruction once per boot
 * is not worth that surface. (WRMSR to 0xC0000101 is accepted under KVM -cpu host
 * on this machine -- measured, not assumed. 0xC0000102 is the one that is not.)
 */
static struct gs_check gs_base_install(uint64_t want, uint32_t want_id)
{
	struct gs_check c = { .want = want, .step = 0 };
	uint32_t id;

	wrmsr(MSR_GS_BASE, want);

	c.msr = rdmsr(MSR_GS_BASE);
	if (c.msr != want)
		c.step = 1;

	c.fast = (uint64_t)(uintptr_t)this_cpu();
	if (c.fast != want)
		c.step = c.step ? c.step : 2;

	/* Offset 0 of percpu_data is cpu_id, so this is a real round trip. */
	__asm__ volatile("movl %%gs:0, %0" : "=r"(id));
	c.via_gs = id;
	if (id != want_id)
		c.step = c.step ? c.step : 3;

	return c;
}

void percpu_setup(uint32_t cpu_id)
{
	if (cpu_id >= MAX_CPUS)
		panic("percpu: cpu %u is out of range, MAX_CPUS is %d",
		      cpu_id, MAX_CPUS);

	struct percpu_data *p = &percpu_data[cpu_id];

	memset(p, 0, sizeof(*p));
	p->cpu_id = cpu_id;

	percpu_install_gs_base(cpu_id);

	p->online = true;
	if (cpu_id + 1 > num_cpus)
		num_cpus = cpu_id + 1;
}

void percpu_install_gs_base(uint32_t cpu_id)
{
	if (cpu_id >= MAX_CPUS)
		panic("percpu: gs base for cpu %u, MAX_CPUS is %d",
		      cpu_id, MAX_CPUS);

	/*
	 * The kernel's own data must never land in the GS-relative window that
	 * userspace code could reach through a wild pointer, so the base points
	 * at a dedicated per-CPU array rather than at anything user-mapped.
	 */
	struct gs_check c = gs_base_install((uint64_t)(uintptr_t)&percpu_data[cpu_id],
					    cpu_id);

	if (c.step) {
		/*
		 * Fatal, and deliberately so. This used to log and return, which
		 * left `online` false, num_cpus at 0, and a GS base that nobody had
		 * installed -- so every this_cpu() caller went on reading and writing
		 * whatever happened to be mapped at the address it returned, which on
		 * this machine is the bootloader's identity window. The address is
		 * mapped and writable, so nothing faults: it surfaces much later as
		 * an allocator returning NULL for every request and a triple fault
		 * inside kmalloc(). There is no correct way to continue from here,
		 * and no version of "log it and carry on" that is better than stopping.
		 *
		 * `step` says which route failed. 1 = the write did not land (wrong
		 * MSR, hypervisor filtering, CR4 state). 2 = it landed and this_cpu()
		 * reads something other than IA32_GS_BASE -- the bug this whole
		 * function exists to catch. 3 = it landed and GS-relative addressing
		 * disagrees about where this CPU's data is.
		 */
		panic_on_cpu(cpu_id,
			     "percpu: GS base install failed at check %d: "
			     "wrote %#lx, IA32_GS_BASE %#lx, this_cpu() %#lx, "
			     "gs-relative cpu_id %u",
			     c.step, c.want, c.msr, c.fast, c.via_gs);
	}
}

uint32_t percpu_num_cpus(void)
{
	return num_cpus;
}