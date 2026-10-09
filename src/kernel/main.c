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
#include <tty.h>
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
     * The bootstrap page tables, which the CPU is walking right now.
     *
     * stage2 builds them at BOOT_PT_ADDR/BOOT_PT_BYTES and enables paging on
     * them before handing over. They are ordinary usable RAM as far as the
     * E820 map is concerned, so the allocator would hand them out -- and the
     * first write to an allocated frame would rewrite the PML4 or the page
     * directory the processor is using, turning one allocation into a machine
     * that no longer can fetch its next instruction.
     *
     * pmm reserves the kernel image itself from _ebss, but these tables are
     * not part of the image: they are loader scratch that outlives the loader.
     * Nothing else covers them. Everything else stage2 touches sits below the
     * 1 MiB reserved above -- stack, bounce buffer, E820 map, bootinfo, GDT.
     */
    pmm_reserve_range(0x002D0000, 0x00010000);

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
     * The IDT goes in before the GDT, not after.
     *
     * This used to be the other way round, with the reasoning that the TSS
     * must exist before any ring-3 interrupt can arrive, because a CPU taking
     * such an interrupt with no TSS would load RSP0 from nothing and fault
     * inside the fault handler. That is sound -- and it is a hazard from
     * *ring 3*, which cannot happen yet: there are no user processes and no
     * syscall entry installed at this point.
     *
     * The ordering cost far more than it bought. Everything that faults between
     * here and `idt_init()` is delivered through the *bootloader's* IDT, which
     * is still installed and whose entries are 16/32-bit stage2 stubs. Run
     * against a 64-bit frame those decode as something else entirely, so the
     * machine triple-faulted into a non-canonical RIP and RDX=0xE9 rather than
     * reporting anything:
     *
     *     IDT=000000000000b120 00000fff   <- stage2's, not ours
     *     RIP=66ee75b003f8ba66  RDX=0xe9
     *
     * That is what made this fault so hard to read: a bug in the kernel was
     * being reported by the bootloader, in the bootloader's own instruction
     * set, as an address that belonged to neither.
     *
     * The TSS still has to exist before anything arrives from ring 3, so
     * `tss_set_kernel_stack()` stays between the two. A fault inside
     * `gdt_reload` now lands in a real 64-bit handler and is reported.
     */
    idt_init();

    /*
     * Interrupts off across the GDT reload, and the timer does not start until
     * it is back on.
     *
     * idt_init() installs the PIT and unmasks IRQ0, so from here on a tick can
     * arrive at any instruction -- including between gdt_flush pushing a
     * three-word far-return frame and the `lretq` that consumes it. The CPU
     * pushes its own frame onto the same stack, runs the handler, and pops it;
     * if anything in that path is not exactly balanced the frame underneath has
     * moved, and the `lretq` then pops a RIP that was never pushed.
     *
     * That is a much better explanation for this fault than anything in the
     * frame itself: gdb single-stepping shows the frame laid out correctly --
     * RIP at rsp+16, CS at rsp+8, RFLAGS at rsp -- and the fault is still
     * #GP(0) with a non-canonical RIP. A correct frame that is read from the
     * wrong place is exactly what an interleaved interrupt produces, and it is
     * also why the fault address was never anything recognisable.
     *
     * The hazard the original ordering was worried about -- ring 3 arriving
     * before the TSS exists -- cannot happen yet: there is no user code and no
     * syscall entry at this point. This is the ordering that is actually
     * needed.
     */
    asm volatile("cli" ::: "memory");
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
     * The console tty, before anything can hand a process a descriptor that
     * points at it. tty_init() attaches the ring storage, installs the
     * keyboard handler and turns scanning on; the rings are `.buf = NULL`
     * until it runs, so the first write from any process -- and PID 1 writes
     * as soon as it starts -- dereferenced a null pointer and took the kernel
     * down with CR2 = 0.
     *
     * After idt_init(), because it calls idt_set_handler() and installs the
     * keyboard handler before unmasking the line on purpose. Before
     * process_create_init(), for the reason above.
     */
    tty_init();


    /*
     * The scheduler's per-CPU state, before anything can enter it.
     *
     * sched_init() is the only place that creates the idle task and publishes
     * per_cpu(current), per_cpu(idle_task) and per_cpu(in_scheduler). It is
     * also the only caller of syscall_set_kernel_stack() -- its own comment
     * says the syscall entry path "needs a kernel stack to land on, and the
     * idle task is what is running", so without it the first SYSCALL from
     * userspace lands on a null kernel stack.
     *
     * Nothing called it. process_create_init() below runs first and builds
     * the first user process; it needs a kernel stack for the task, and it
     * needs per_cpu(current) to be coherent when it runs. So this belongs
     * *before* process_create_init(), not immediately before sched_start().
     *
     * Note the ordering that falls out of it: gdt/tss/idt/syscall, then the
     * scheduler's per-CPU state, then PID 1. Anything that runs before
     * sched_init() is running with per_cpu(current) unset.
     */
    sched_init();

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
    /*
     * A rule above and below, so the kernel's output is visually separable
     * from the loader's. The loader prefixes its lines with "[boot2]" and the
     * kernel's with a timestamp, and in a log where the two are interleaved
     * with a run of NUL bytes in between, that difference is easy to miss --
     * and the mistake is easy to make, because "did the loader say that or did
     * the kernel?" changes what a line means.
     */
    kprintf("=========================================================\n");
    kprintf(" %s (%s, rev %s)\n", KERNEL_VERSION_STRING,
        KERNEL_BUILD_STAMP, KERNEL_GIT_REV);
    kprintf(" boot: entry 0x%016lx, image 0x%016lx, drive 0x%lx, "
        "cmdline '%s'\n",
        boot.kernel_entry, boot.kernel_phys_base, boot.boot_drive,
        boot.cmdline[0] ? boot.cmdline : "(none)");
    kprintf("=========================================================\n");
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
    /*
     * Bytes, not frames.
     *
     * pmm_total() and pmm_free() count frames, and this shifted them by 20 as
     * though they were bytes -- so a machine with 4 GiB of usable memory
     * reported "0 MiB total", because 1048576 frames is the threshold and
     * anything under 4 GiB of frames printed zero. The count was always right;
     * the unit was wrong. pmm_total_bytes()/pmm_free_bytes() exist so the
     * conversion happens in one place with a name that says which way it goes.
     */
    kprintf("memory: %lu MiB total, %lu MiB free, %u E820 entries\n",
        (unsigned long)(pmm_total_bytes() >> 20),
        (unsigned long)(pmm_free_bytes() >> 20),
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
