/*
 * main.c — kernel bring-up.
 *
 * Everything here runs exactly once, on CPU 0, before the scheduler exists. The
 * order below is not a preference: each step depends on the ones above it, and
 * the comment on each step says which dependency, because "why is the allocator
 * before the interrupt tables" gets re-asked every time someone adds a
 * subsystem to the top of this file.
 *
 * The broad shape is the standard sequence — early text output, fixed-address
 * platform discovery, physical memory, virtual memory — and then it diverges by
 * not enabling interrupts until the address space is final. The reason is that
 * the page tables are built by an allocator, so a timer that fires before the
 * allocator is up lands in an interrupt path that cannot allocate the stack it
 * needs in order to report itself.
 */

#include <boot.h>
#include <gdt.h>
#include <version.h>

#include <console.h>
#include <drivers/serial.h>
#include <cpu_features.h>
#include <gdt.h>
#include <interrupt.h>
#include <klog.h>
#include <panic.h>
#include <percpu.h>
#include <pmm.h>
#include <process.h>
#include <sched.h>
#include <task.h>
#include <types.h>
#include <vmm.h>

/* Provided by the syscall subsystem. */
void syscall_init(void);

/* The stack reserved in .bss by the link script. */
extern char __kernel_stack_top[];

/*
 * The bootinfo the kernel actually uses, copied out of the bootloader's memory.
 *
 * It is copied rather than pointed at because stage2's structures are temporary:
 * once vmm_switch_to_kernel_pgd() replaces the bootstrap CR3, the identity view
 * of 0x90000 is gone. A pointer kept across that point would be a dangling
 * reference to memory the kernel has already reclaimed, and the E820 array
 * inside bootinfo would be the first thing read from it.
 *
 * The static buffer is also what makes the copy safe: it is in .bss, which
 * stage2 zeroed, and it is in the higher half, so it is mapped by the permanent
 * page tables the same as any other kernel object.
 */
static struct bootinfo boot;
static struct e820_entry e820_copy[128];

/* Set once the bootloader structures are no longer reachable. */
static bool direct_map_ready;

/*
 * Translate a bootloader physical address into a pointer the kernel can
 * dereference.
 *
 * Before vmm_switch_to_kernel_pgd() the only mapping is stage2's identity view
 * of the low 4 GiB, so a physical address is already a valid virtual address
 * and this is the identity function. Afterwards the direct map at
 * PHYS_DIRECT_MAP is live, and the low identity window is gone. Getting this
 * backwards is not a subtle failure — it reads a page that the allocator may
 * have already handed out.
 */
static inline void *boot_ptr(phys_addr_t phys)
{
	if (direct_map_ready)
		return (void *)(uintptr_t)(PHYS_DIRECT_MAP + phys);
	return (void *)(uintptr_t)phys;
}

static void kmain_banner(void);
static void kmain_report_cpu(void);
static void kmain_report_memory(void);
static void kmain_report_framebuffer(void);

/*
 * kmain — the kernel's C entry point.
 *
 * Called from kmain.S with RDI holding the physical address of the bootinfo
 * structure stage2 filled in. Nothing else in the register file is meaningful.
 */
void kmain(uint64_t bootinfo_phys)
{
	/*
	 * The serial port comes up first, before anything else, because it is the
	 * only backend that needs no information from the bootloader. Every
	 * failure below this line is reported over serial even if the framebuffer
	 * turns out to be unusable or the bootinfo is malformed.
	 */
	serial_init();
	console_init(NULL);

	/*
	 * Copy the bootloader report out of bootloader memory before anything can
	 * reclaim it. Validating the magic here rather than in the bootloader is
	 * deliberate: the check has to be in the kernel too, because the bootinfo
	 * is a plain struct in memory and nothing prevents it from being wrong.
	 *
	 * This happens after the serial-only console_init and before the one that
	 * would use a framebuffer, because choosing a framebuffer needs the
	 * framebuffer description.
	 */
	const struct bootinfo *src = boot_ptr((phys_addr_t)bootinfo_phys);

	if (src->magic != BOOTINFO_MAGIC)
		panic("bootinfo magic is 0x%016lx, expected 0x%016lx",
		      src->magic, (u64)BOOTINFO_MAGIC);
	if (src->version != BOOTINFO_VERSION)
		panic("bootinfo version %u, kernel expects %u",
		      src->version, (unsigned)BOOTINFO_VERSION);
	if (src->e820_count > (uint32_t)(sizeof(e820_copy) / sizeof(e820_copy[0])))
		panic("E820 map has %u entries, the kernel reserves room for %zu",
		      src->e820_count, sizeof(e820_copy) / sizeof(e820_copy[0]));

	boot = *src;

	const struct e820_entry *src_e820 = boot_ptr((phys_addr_t)boot.e820_addr);
	for (uint32_t i = 0; i < boot.e820_count; i++)
		e820_copy[i] = src_e820[i];

	/* Repoint at the kernel's own copies before anything reads them again. */
	boot.e820_addr = (uint64_t)(uintptr_t)e820_copy;

	klog_init();

	/*
	 * Identity. The per-CPU area has to exist before anything that uses
	 * this_cpu() or per_cpu(), and the CPU description has to exist before any
	 * code acts on the discovered feature set. cpu_features_init() only
	 * records what CPUID reports; nothing consumes it until the paging setup,
	 * which chooses between 4 KiB and 1 GiB pages based on it.
	 */
	percpu_setup(0);
	cpu_features_init();

	/*
	 * Virtual memory comes before physical memory, and that order is forced
	 * rather than chosen. pmm_init() places its bitmaps and its page array
	 * through phys_to_virt(), and phys_to_virt() is an addition on
	 * PHYS_DIRECT_MAP — which does not resolve until the direct map exists.
	 * Running these the other way round faults on the first memset of the
	 * page array, before anything has printed a character.
	 *
	 * vmm_init() gets away without an allocator because the first page tables
	 * come from a static pool inside the kernel image, which the bootloader's
	 * identity mapping already covers. cpu_features_init() has to run before it
	 * because the direct map picks 1 GiB or 2 MiB pages from what CPUID
	 * reported.
	 *
	 * This is the last point at which the bootloader is load-bearing. After
	 * the CR3 write, its structures are ordinary unused memory and the low
	 * identity window is gone — which is why direct_map_ready flips here and
	 * boot_ptr() changes meaning below this line.
	 */
	vmm_init();
	vmm_switch_to_kernel_pgd();
	direct_map_ready = true;

	/*
	 * Physical memory. The E820 map is consumed here, and this is the first
	 * point at which the kernel can allocate.
	 *
	 * The address handed to pmm_init() is the *physical* address of the
	 * kernel's own copy of the E820 array, not its virtual address. The array
	 * is static storage in the kernel image, and the kernel window is an
	 * alias of the landing zone at KERNEL_LANDING_ADDR rather than an
	 * identity map, so the two differ by that offset and not by
	 * KERNEL_VIRT_BASE.
	 *
	 * It must be kernel_virt_to_phys(), not virt_to_phys_direct(). The
	 * direct map and the kernel window are 8 exabytes apart; subtracting the
	 * wrong base produces a plausible address that classifies every range as
	 * absent, so pmm_init() would come up with no memory and no error to
	 * explain it.
	 */
	pmm_init(kernel_virt_to_phys((virt_addr_t)(uintptr_t)e820_copy),
		 boot.e820_count);

	/*
	 * The low 1 MiB is reserved explicitly. The E820 map marks it usable,
	 * which is correct as a statement about the firmware and wrong as a
	 * statement about this kernel, which is still executing from inside it —
	 * the MBR, stage2, the bounce window, the program-header scratch and the
	 * bootinfo all live here. Nothing else would catch the mistake: the
	 * first allocation landing there would succeed, and would then be
	 * silently overwritten by the next instruction fetched from the image.
	 */
	pmm_reserve_range(0, 1024 * 1024);

	/*
	 * The SLAB caches allocate buddy pages on first use, so they come up
	 * after the PMM and not before it. Nothing between pmm_init() and here
	 * allocates, so the first kmalloc() cannot arrive too early.
	 */
	kmalloc_init();

	/*
	 * The framebuffer backend is attached here rather than with the bootinfo
	 * copy above, and the reason is that attaching it allocates.
	 *
	 * console_fb_init() kmalloc()s the cell buffer behind the text grid, and
	 * against an allocator that has not been initialised that call is not a
	 * failed allocation: every cache is still zeroed BSS, so the partial list
	 * tests empty against a freelist threaded through NULL, slab_new() runs
	 * with c->per_slab == 0, and its free-list-threading loop — which walks
	 * from c->per_slab - 1 down to 0 and indexes the slab at i - 1 — becomes
	 * four billion iterations writing the same address. It is latent on
	 * SeaBIOS only because SeaBIOS reports no linear framebuffer, so the
	 * call is never made; on any firmware that reports one, this is the first
	 * thing the kernel executes after the banner.
	 *
	 * kmalloc() is the only allocator-backed call on this path, so moving the
	 * attachment below kmalloc_init() is the whole of the fix. The cost is
	 * that the framebuffer comes up after the paging switch rather than
	 * before it, which is why the VGA buffer and the framebuffer base are
	 * both resolved through vmm_boot_ptr() rather than stored as pointers:
	 * the low identity view they used to be reached through is gone by now.
	 *
	 * The serial-only console_init(NULL) above is untouched and still first,
	 * so every failure between here and the framebuffer is still reported.
	 */
	console_init(&boot);

	kmain_banner();
	kmain_report_cpu();
	kmain_report_framebuffer();
	kmain_report_memory();

	/*
	 * Privilege and interrupts.
	 *
	 * The GDT is reloaded with a proper TSS before the IDT exists, because the
	 * TSS supplies RSP0 for every interrupt that arrives from ring 3. A CPU
	 * taking such an interrupt without one faults on a null stack, inside the
	 * fault handler, before it can report anything.
	 */
	gdt_reload(0);
	tss_set_kernel_stack((void *)__kernel_stack_top);
	exceptions_init();
	idt_init();

	/*
	 * Syscall entry. Installed after the IDT so that a user process cannot
	 * reach a SYSCALL instruction that has no dispatcher behind it, which
	 * would #UD in user space with no kernel handler to explain it.
	 */
	syscall_init();

	/*
	 * The first user process, created before sched_start() because
	 * sched_start() never returns: it hands the CPU to the first runnable
	 * task and the kernel stops being a boot sequence. Anything belonging to
	 * user space that is not reachable later belongs here.
	 */
	struct task *init = process_create_init();
	if (!init)
		panic("could not create the init process; there is nothing to run");

	kprintf("init: pid %u loaded, entering the scheduler\n", init->pid);

	sched_start();

	/* Unreachable: reaching it means the idle task was torn down, leaving no
	 * valid stack to return into. */
	panic("sched_start() returned");
}

/* ------------------------------------------------------------------------- */

static void kmain_banner(void)
{
	kprintf("OS kernel %s (%s, rev %s)\n", KERNEL_VERSION_STRING,
		KERNEL_BUILD_STAMP, KERNEL_GIT_REV);
	kprintf("boot: entry 0x%016lx, image 0x%016lx, drive 0x%lx, cmdline '%s'\n",
		boot.kernel_entry, boot.kernel_phys_base, boot.boot_drive,
		boot.cmdline[0] ? boot.cmdline : "(none)");
}

static void kmain_report_cpu(void)
{
	kprintf("cpu0: %s, %u logical CPUs, features 0x%016llx\n",
		cpu_vendor(), cpu_features.logical_cpus,
		(unsigned long long)cpu_features.feature_mask);
	kprintf("  apic=%u tsc_deadline=%u tsc_khz=%llu invariant_tsc=%u "
		"hypervisor=%u 1g_pages=%u\n",
		cpu_features.apic, cpu_features.tsc_deadline_timer,
		(unsigned long long)cpu_features.tsc_khz,
		cpu_features.invariant_tsc, cpu_features.hypervisor,
		cpu_features.has_1gb_pages);
}

/*
 * Print the memory map.
 *
 * The E820 entries are printed raw rather than summarised, because the
 * interesting entry is always the surprising one, and a summary is exactly what
 * hides it.
 */
static void kmain_report_memory(void)
{
	kprintf("memory: %lu MiB total, %lu MiB free, %u E820 entries\n",
		(unsigned long)(pmm_total() >> 20), (unsigned long)(pmm_free() >> 20),
		boot.e820_count);

	for (uint32_t i = 0; i < boot.e820_count; i++) {
		uint64_t start = e820_copy[i].base;
		uint64_t end = start + e820_copy[i].length - 1;

		kprintf("  e820[%2u] %016llx-%016llx %-8s %llu MiB%s\n", i,
			(unsigned long long)start, (unsigned long long)end,
			e820_copy[i].type == E820_USABLE ? "usable" : "reserved",
			(unsigned long long)(e820_copy[i].length >> 20),
			(i + 1 == boot.e820_count) ? "  <- highest usable"
						    : "");
	}
}

static void kmain_report_framebuffer(void)
{
	if (!(boot.flags & BOOT_FLAG_HAS_FRAMEBUFFER)) {
		kprintf("display: none reported, falling back to the VGA text buffer\n");
		return;
	}

	kprintf("display: %ux%u at 0x%016llx, %u bpp, pitch %u\n",
		boot.fb.width, boot.fb.height,
		(unsigned long long)boot.fb.address, boot.fb.bpp, boot.fb.pitch);
}
