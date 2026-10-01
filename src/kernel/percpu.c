/*
 * percpu.c — per-CPU area setup.
 *
 * The GS base register is loaded with the address of this CPU's slice of the
 * per-CPU array. Because MSR_GS_BASE is a 64-bit MSR, this works in long mode
 * with no GDT entry and no reload: the value survives arbitrary nesting of
 * interrupts and syscalls.
 *
 * In long mode GS is not segment-relative, so the base *is* the value loaded
 * into GS. `this_cpu()` therefore dereferences GS directly, which compiles to a
 * single memory-operand instruction.
 */

#include <percpu.h>
#include <io.h>
#include <klog.h>
#include <kstring.h>

KLOG_SUBSYSTEM("percpu");

struct percpu_data percpu_data[MAX_CPUS] __aligned(64);
static uint32_t num_cpus;

void percpu_setup(uint32_t cpu_id)
{
	if (cpu_id >= MAX_CPUS) {
		klog(KLOG_FATAL, "percpu: cpu %u exceeds MAX_CPUS\n", cpu_id);
		return;
	}

	struct percpu_data *p = &percpu_data[cpu_id];

	memset(p, 0, sizeof(*p));
	p->cpu_id = cpu_id;
	p->preempt_count = 0;
	p->online = false;

	/*
	 * The kernel's own data must never land in the GS-relative window that
	 * userspace code could reach through a wild pointer, so the base points
	 * at a dedicated per-CPU array rather than at anything user-mapped.
	 */
	wrmsr(MSR_GS_BASE, (uint64_t)(uintptr_t)p);

	/* Confirm the write took effect before anything trusts this_cpu(). */
	struct percpu_data *check = this_cpu();
	if (check->cpu_id != cpu_id) {
		klog(KLOG_FATAL, "percpu: gs base verification failed\n");
		return;
	}

	p->online = true;
	if (cpu_id + 1 > num_cpus)
		num_cpus = cpu_id + 1;
}

uint32_t percpu_num_cpus(void)
{
	return num_cpus;
}
