/*
 * idt.c — the vector table, the shared dispatch path, and exception policy.
 *
 * Three things live here because each is one decision, not three: building the
 * table, deciding what each of the 32 architectural exceptions means, and the
 * accounting the scheduler needs on the way out.
 *
 * The vector map, entry contract, and exception policy are specified in
 * docs/src/kernel/interrupt-handling.md. That document is why the table is laid
 * out this way; if it and this file disagree, the document is wrong.
 */
#include <interrupt.h>

#include <console.h>
#include <drivers/serial.h>
#include <gdt.h>
#include <io.h>
#include <klog.h>
#include <kprintf.h>
#include <panic.h>
#include <percpu.h>
#include <sched.h>
#include <types.h>
#include <vmm.h>

KLOG_SUBSYSTEM("idt");

/* ------------------------------------------------- IDT entry encoding ----- */

/*
 * A 64-bit IDT gate is a 16-byte descriptor whose offset is a full 64 bits, so
 * unlike a TSS it does not have to be split across two slots.
 *
 *   0-1   offset[15:0]
 *   2-3   segment selector (always the kernel code segment; long mode has no
 *         far call so the CPU ignores it, but it must be presentable)
 *   4     IST index in the low three bits, DPL in the next two
 *   5     type and P
 *   6-7   offset[31:16]
 *   8-15  offset[63:32]
 */
struct idt_entry {
	uint16_t offset_low;
	uint16_t selector;
	uint8_t ist_dpl;
	uint8_t type_attr;
	uint16_t offset_mid;
	uint32_t offset_high;
	uint32_t reserved;
} __attribute__((packed));

/* 0x8F is a present *64-bit* interrupt gate: it clears IF on entry, which is
 * what every handler here wants, and -- the part that matters here -- a 64-bit
 * gate is the only kind that loads the full 64-bit offset. The low three bits
 * of the type field are the gate's size, and 0b1110 is the *32-bit* interrupt
 * gate. With that byte the CPU takes offset[31:0] as the whole handler address
 * and clears the top 32 bits, so a handler linked at 0xffffffff800108c7 is
 * entered at 0x000108c7, which is unmapped: the exception becomes a page
 * fault, the page fault becomes a double fault, and the machine resets.
 *
 * Nothing in the tree shows that. The gates are all present and correctly
 * filled in, idt_init() reports a plausible table, and the CPU accepts the
 * IDTR -- it just never reaches the handler. The only symptom is that the
 * kernel stops speaking mid-boot and the machine reboots, which looks like a
 * hang or a crash somewhere else entirely.
 *
 * 0xFF is the 64-bit *trap* gate, which leaves IF set; it is right only for a
 * handler that must not mask interrupts. */
#define IDT_TYPE_INTERRUPT_GATE  0x8F

static struct idt_entry idt[IDT_ENTRIES] __attribute__((aligned(16)));
static struct {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed)) idtr;

/* ------------------------------------------------- handler registry ------- */

/*
 * Indexed by vector. A NULL entry means "no handler", which after idt_init()
 * never happens: every vector is installed, most of them with the do-nothing
 * handler below. An empty gate is not something the CPU diagnoses — it would
 * jump to offset zero, the first byte of the kernel — so the table is kept
 * complete and the "nobody owns this" case is a handler that counts and
 * returns.
 */
static irq_handler_t handlers[IDT_ENTRIES];

/* Whether a vector has been claimed by a subsystem. Separate from handlers[]
 * because the default handler is replaceable; without this a driver could not
 * take over a vector that idt_init() had already filled in. */
static bool claimed[IDT_ENTRIES];

/* Counters, so a device that interrupts without a driver is visible in the log
 * rather than being silently absorbed forever. */
static u64 unhandled_irqs;
static u64 unhandled_exceptions;

/*
 * The reschedule request, per the dispatch step in the docs. It lives here
 * rather than in struct percpu_data because nothing else needs it: the
 * interrupt path is the only reader, and the timer driver is the only writer.
 */
static volatile u32 need_resched[MAX_CPUS];

/* ------------------------------------------------- assembly stubs --------- */

/*
 * One symbol per vector, named exception_N or interrupt_N, so that the stub
 * number is the vector number. That identity is what lets the dispatcher index
 * handlers[] instead of calling a distinct symbol per vector, and it is why the
 * stub macros in interrupt_entry.S generate the whole 0-63 range rather than
 * just the vectors in use.
 */
void exception_0(void);
void exception_1(void);
void exception_2(void);
void exception_3(void);
void exception_4(void);
void exception_5(void);
void exception_6(void);
void exception_7(void);
void exception_8(void);
void exception_9(void);
void exception_10(void);
void exception_11(void);
void exception_12(void);
void exception_13(void);
void exception_14(void);
void exception_15(void);
void exception_16(void);
void exception_17(void);
void exception_18(void);
void exception_19(void);
void exception_20(void);
void exception_21(void);
void exception_22(void);
void exception_23(void);
void exception_24(void);
void exception_25(void);
void exception_26(void);
void exception_27(void);
void exception_28(void);
void exception_29(void);
void exception_30(void);
void exception_31(void);

void interrupt_32(void);
void interrupt_33(void);
void interrupt_34(void);
void interrupt_35(void);
void interrupt_36(void);
void interrupt_37(void);
void interrupt_38(void);
void interrupt_39(void);
void interrupt_40(void);
void interrupt_41(void);
void interrupt_42(void);
void interrupt_43(void);
void interrupt_44(void);
void interrupt_45(void);
void interrupt_46(void);
void interrupt_47(void);
void interrupt_48(void);
void interrupt_49(void);
void interrupt_50(void);
void interrupt_51(void);
void interrupt_52(void);
void interrupt_53(void);
void interrupt_54(void);
void interrupt_55(void);
void interrupt_56(void);
void interrupt_57(void);
void interrupt_58(void);
void interrupt_59(void);
void interrupt_60(void);
void interrupt_61(void);
void interrupt_62(void);
void interrupt_63(void);

/* ------------------------------------------------- IDT construction ------- */

static void idt_set_gate(uint8_t vector, void (*handler)(void), uint8_t ist,
			 uint8_t dpl)
{
	struct idt_entry *e = &idt[vector];
	uint64_t addr = (uint64_t)handler;

	e->offset_low  = (uint16_t)(addr & 0xFFFF);
	e->selector    = KERNEL_CODE_SELECTOR;
	e->ist_dpl     = (uint8_t)((ist & 0x7) | ((dpl & 0x3) << 5));
	e->type_attr   = IDT_TYPE_INTERRUPT_GATE;
	e->offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
	e->offset_high = (uint32_t)(addr >> 32);
	e->reserved    = 0;
}

/*
 * The default handler: acknowledges nothing, reports nothing, returns. A vector
 * with no owner is normal — most of the APIC range belongs to drivers that do
 * not exist yet — and panicking on one would mean a machine cannot boot until
 * every planned device is implemented. The counter is what makes it debuggable.
 */
static void unhandled_vector(struct interrupt_frame *f)
{
	if (f->vector < VECTOR_EXCEPTIONS_MAX)
		unhandled_exceptions++;
	else
		unhandled_irqs++;
}

void idt_set_handler(uint8_t vector, irq_handler_t handler, uint8_t ist,
		     uint8_t dpl)
{
	/*
	 * Refusing to re-claim is deliberate. Two subsystems each believing they
	 * own the timer produce a bug that shows up as a keyboard interrupt
	 * occasionally running the timer handler, which is far harder to find
	 * than a boot-time panic.
	 */
	ASSERT_MSG(!claimed[vector],
		   "vector %u already claimed; two subsystems are fighting over it",
		   vector);

	claimed[vector] = true;
	handlers[vector] = handler;
	idt_set_gate(vector, (void (*)(void))handler, ist, dpl);
}

/*
 * The generic exception handler, installed on all 32 vectors so none can reach
 * an empty gate. Defined with the policy it implements, further down.
 */
static void exception_handler(struct interrupt_frame *f);

/* The PIT bring-up, defined with the other 8259 code further down and called
 * from idt_init() once the IDTR is loaded. */
static void pit_init(void);

void exceptions_init(void)
{
	static void (*const stubs[VECTOR_EXCEPTIONS_MAX])(void) = {
		exception_0,  exception_1,  exception_2,  exception_3,
		exception_4,  exception_5,  exception_6,  exception_7,
		exception_8,  exception_9,  exception_10, exception_11,
		exception_12, exception_13, exception_14, exception_15,
		exception_16, exception_17, exception_18, exception_19,
		exception_20, exception_21, exception_22, exception_23,
		exception_24, exception_25, exception_26, exception_27,
		exception_28, exception_29, exception_30, exception_31,
	};

	for (unsigned v = 0; v < VECTOR_EXCEPTIONS_MAX; v++) {
		/*
		 * Double fault is the exception that fires when the previous
		 * handler's stack is already unusable, so it must not run on that
		 * stack or it would recurse until the machine resets. NMI is the
		 * halt path and must not depend on the stack of whatever
		 * happened to fault.
		 *
		 * Both are requested, and both are skipped when no stack is
		 * installed: a gate that claims an IST slot the TSS cannot honour
		 * takes a #PF on entry, which is a worse outcome than running on
		 * the interrupted stack. See gdt_set_ist_stack().
		 */
		uint8_t ist = IST_NONE;
		if ((v == VECTOR_DOUBLE_FAULT || v == VECTOR_NMI) &&
		    gdt_have_ist(v == VECTOR_DOUBLE_FAULT ? IST_DOUBLE_FAULT
							 : IST_NMI))
			ist = (v == VECTOR_DOUBLE_FAULT) ? IST_DOUBLE_FAULT
							 : IST_NMI;

		/*
		 * DPL 3 only for #BP, so user code can raise int3. Everything
		 * else is DPL 0: a user-callable gate on a device vector would
		 * let any process interrupt any other at will.
		 */
		uint8_t dpl = (v == VECTOR_BREAKPOINT) ? 3 : 0;

		claimed[v] = true;
		handlers[v] = exception_handler;
		idt_set_gate((uint8_t)v, stubs[v], ist, dpl);
	}

	klog(KLOG_INFO, "idt: %u exception handlers, double fault %s IST%u, "
	     "NMI %s IST%u\n", VECTOR_EXCEPTIONS_MAX,
	     gdt_have_ist(IST_DOUBLE_FAULT) ? "on" : "without",
	     IST_DOUBLE_FAULT, gdt_have_ist(IST_NMI) ? "on" : "without",
	     IST_NMI);
}

void idt_init(void)
{
	static void (*const irq_stubs[VECTOR_STUB_MAX - VECTOR_IRQ_BASE])(void) = {
		interrupt_32, interrupt_33, interrupt_34, interrupt_35,
		interrupt_36, interrupt_37, interrupt_38, interrupt_39,
		interrupt_40, interrupt_41, interrupt_42, interrupt_43,
		interrupt_44, interrupt_45, interrupt_46, interrupt_47,
		interrupt_48, interrupt_49, interrupt_50, interrupt_51,
		interrupt_52, interrupt_53, interrupt_54, interrupt_55,
		interrupt_56, interrupt_57, interrupt_58, interrupt_59,
		interrupt_60, interrupt_61, interrupt_62, interrupt_63,
	};

	/*
	 * Every vector from 32 to 63 gets a real stub, not just the 16 the 8259
	 * can produce. The stubs are free, and a driver that later routes an
	 * APIC device into the low range finds the vector already wired; more
	 * importantly, the stub number equals the vector number across the whole
	 * range, which is what the dispatcher's indexing relies on. Handlers
	 * still default to the do-nothing one, so an unowned vector is merely
	 * counted rather than dispatched.
	 */
	for (unsigned v = VECTOR_IRQ_BASE; v < VECTOR_STUB_MAX; v++) {
		claimed[v] = false;
		handlers[v] = unhandled_vector;
		idt_set_gate((uint8_t)v, irq_stubs[v - VECTOR_IRQ_BASE],
			     IST_NONE, 0);
	}

	for (unsigned v = VECTOR_STUB_MAX; v < IDT_ENTRIES; v++) {
		claimed[v] = false;
		handlers[v] = unhandled_vector;
		/* A gate is never left empty. An empty gate is an offset of
		 * zero, which the CPU would treat as a valid entry and jump
		 * to the first byte of the kernel. */
		idt_set_gate((uint8_t)v, (void (*)(void))unhandled_vector,
			     IST_NONE, 0);
	}

	idtr.limit = sizeof(idt) - 1;
	idtr.base  = (uint64_t)idt;
	asm volatile("lidt %0" :: "m"(idtr) : "memory");

	klog(KLOG_INFO, "idt: %u entries at %#llx, %u bytes\n", IDT_ENTRIES,
	     (unsigned long long)idtr.base, (unsigned)idtr.limit);

	/*
	 * The timer is started here rather than from main.c for two reasons,
	 * both about ordering rather than convenience.
	 *
	 * 1. This is the first instruction of kernel initialisation at which a
	 *    vector can be delivered to: the gates exist, the IDTR is loaded, and
	 *    exceptions_init() has run. Nothing earlier would work.
	 * 2. It has to be before sched_start(), which never returns. Anything
	 *    that wants to print the tick count has to do so after this point
	 *    and before the scheduler takes the CPU away.
	 */
	pit_init();
}

/* ------------------------------------------------- exception policy ------- */

static const char *const exception_names[VECTOR_EXCEPTIONS_MAX] = {
	[VECTOR_DIVIDE_ERROR]         = "#DE divide error",
	[VECTOR_DEBUG]                = "#DB debug",
	[VECTOR_NMI]                  = "NMI",
	[VECTOR_BREAKPOINT]           = "#BP breakpoint",
	[VECTOR_OVERFLOW]             = "#OF overflow",
	[VECTOR_BOUND_RANGE]          = "#BR bound range exceeded",
	[VECTOR_INVALID_OPCODE]       = "#UD invalid opcode",
	[VECTOR_DEVICE_NOT_AVAILABLE] = "#NM device not available",
	[VECTOR_DOUBLE_FAULT]         = "#DF double fault",
	[9]                           = "#Cop coprocessor overrun",
	[VECTOR_INVALID_TSS]          = "#TS invalid TSS",
	[VECTOR_SEGMENT_NOT_PRESENT]  = "#NP segment not present",
	[VECTOR_STACK_FAULT]          = "#SS stack fault",
	[VECTOR_GENERAL_PROTECTION]   = "#GP general protection",
	[VECTOR_PAGE_FAULT]           = "#PF page fault",
	[15]                          = "reserved",
	[VECTOR_FP_ERROR]             = "#MF x87 FP error",
	[VECTOR_ALIGNMENT_CHECK]      = "#AC alignment check",
	[VECTOR_MACHINE_CHECK]        = "#MC machine check",
	[VECTOR_SIMD_FP_ERROR]        = "#XM SIMD FP error",
	[20]                          = "#VE virtualization",
	[VECTOR_CONTROL_PROTECTION]   = "#CP control protection",
	[22]                          = "reserved",
	[23]                          = "reserved",
	[24]                          = "reserved",
	[25]                          = "reserved",
	[26]                          = "reserved",
	[27]                          = "reserved",
	[28]                          = "hypervisor injection",
	[29]                          = "VMM communication",
	[30]                          = "security exception",
	[31]                          = "reserved",
};

static const char *exception_name(uint64_t vector)
{
	if (vector >= VECTOR_EXCEPTIONS_MAX)
		return "unknown vector";
	return exception_names[vector] ? exception_names[vector]
				       : "unnamed vector";
}

/*
 * One reporting path for every exception. The vector, the faulting RIP, the
 * ring, and the error code identify nearly every exception, and a single fixed
 * format means the line is recognisable in a serial log with no debugger
 * attached — which is the only place it will usually be read.
 */
static void report_exception(struct interrupt_frame *f, const char *action)
{
	kprintf("#EXC %-28s vector=%lu cs=0x%04lx rpl=%lu rip=0x%016lx "
		"err=0x%lx rsp=0x%016lx -> %s\n",
		exception_name(f->vector), (unsigned long)f->vector,
		(unsigned long)f->cs, (unsigned long)(f->cs & 3),
		(unsigned long)f->rip, (unsigned long)f->error_code,
		(unsigned long)f->rsp, action);

	kprintf("     rdi=0x%016lx rsi=0x%016lx rdx=0x%016lx rcx=0x%016lx "
		"r8=0x%016lx r9=0x%016lx r10=0x%016lx r11=0x%016lx\n",
		(unsigned long)f->rdi, (unsigned long)f->rsi,
		(unsigned long)f->rdx, (unsigned long)f->rcx,
		(unsigned long)f->r8, (unsigned long)f->r9,
		(unsigned long)f->r10, (unsigned long)f->r11);
}

/*
 * The generic exception handler, installed on all 32 vectors so none can reach
 * an empty gate. The decision it makes is: is this the process's problem or the
 * kernel's?
 *
 * A ring-3 exception is the process's, and the kernel survives it. A ring-0
 * exception is a kernel bug, and there is no safe way to continue — the state
 * that would let the kernel keep running is the state that just proved wrong.
 * Guessing wrong in the ring-0 case means running on corrupted state; guessing
 * wrong in the ring-3 case means killing a process that did nothing.
 */
static void exception_handler(struct interrupt_frame *f){
	bool from_user = frame_from_user(f);

	/*
	 * #PF first, and before the ring check: it is the one exception that is
	 * usually not a fault at all. Demand paging and stack growth both land
	 * here, and the VMM owns the decision about whether the access was
	 * legitimate, because only it can tell a mapping from a bug.
	 */
	if (f->vector == VECTOR_PAGE_FAULT) {
		/* CR2, not RIP: CR2 holds the address that was being accessed,
		 * which is what the VMM has to look up. */
		long r = vmm_handle_page_fault(current_mm(),
					       (virt_addr_t)read_cr2(),
					       f->error_code);
		if (r == 0)
			return;
		report_exception(f, "unrecoverable page fault");
		goto fatal;
	}

	/* SIGFPE, SIGILL and SIGTRAP describe a process's own execution and have
	 * no meaning in the kernel, so they are only ever raised against one. */
	if (from_user && (f->vector == VECTOR_DIVIDE_ERROR ||
			  f->vector == VECTOR_INVALID_OPCODE ||
			  f->vector == VECTOR_BREAKPOINT)) {
		int signo = f->vector == VECTOR_DIVIDE_ERROR  ? 8 /* SIGFPE */
			    : f->vector == VECTOR_BREAKPOINT ? 5 /* SIGTRAP */
							     : 4; /* SIGILL */

		if (process_deliver_signal) {
			report_exception(f, "delivering signal");
			process_deliver_signal(signo, f);
			return;
		}
		/* Without signal delivery the only correct action is to stop the
		 * process: resuming it would re-execute the faulting
		 * instruction and take the same exception forever. */
		if (process_terminate_from_fault) {
			report_exception(f, "no signal subsystem; killing process");
			process_terminate_from_fault(f);
			return;
		}
		report_exception(f, "no process-control hook; panicking");
		goto fatal;
	}

	/*
	 * A user process can legitimately take the exceptions that describe its
	 * own execution, and the segment/stack faults that mean it referenced
	 * something it was not allowed to. Anything outside that list arriving
	 * from ring 3 — an invalid TSS, a machine check, a double fault — is not
	 * something a ring-3 instruction can cause, so it is a kernel fault no
	 * matter which ring we were in when it landed.
	 */
	if (from_user) {
		switch (f->vector) {
		case VECTOR_GENERAL_PROTECTION:
		case VECTOR_INVALID_TSS:
		case VECTOR_SEGMENT_NOT_PRESENT:
		case VECTOR_STACK_FAULT:
		case VECTOR_OVERFLOW:
		case VECTOR_BOUND_RANGE:
		case VECTOR_CONTROL_PROTECTION:
		case VECTOR_SIMD_FP_ERROR:
		case VECTOR_DEVICE_NOT_AVAILABLE:
			if (process_terminate_from_fault) {
				report_exception(f, "killing process");
				process_terminate_from_fault(f);
				return;
			}
			report_exception(f, "no process-control hook; panicking");
			goto fatal;
		default:
			break;
		}
	}

fatal:
	/*
	 * Not survivable. panic() writes the full register set through its own
	 * serial path without touching the scheduler or the allocator, because
	 * either of those may be the thing that broke.
	 *
	 * CS and the ring go into the message, and that is not decoration. CS
	 * cannot be a MOV destination in 64-bit mode, so there is no instruction
	 * here that could read it, and panic.S's comment claims a "caller-side
	 * stub" records it — this is that stub, and the only honest way to get
	 * the value is the one the CPU already put in the frame. It goes in the
	 * message rather than in panic_state.cs because panic_common() calls
	 * panic_regs() first and that overwrites the whole tail of the struct;
	 * the message is formatted after it and survives.
	 *
	 * The "cs %p" field that panic_common() prints from panic_state is NOT
	 * that CS, and it cannot be made to be from here: panic.S's field
	 * numbering has no cs slot at all, so it stores RFLAGS into the cs
	 * offset and everything after it is one slot short. See the report. Until
	 * panic.S is fixed, read CS and RPL off this line and off the #EXC line
	 * above it, both of which are read from the real frame.
	 */
	report_exception(f, from_user ? "kernel panic (user exception)"
				       : "kernel panic");
	panic("unhandled exception: %s (vector %lu) at rip 0x%lx, cs 0x%04lx "
	      "rpl %lu, entered from %s",
	      exception_name(f->vector), (unsigned long)f->vector,
	      (unsigned long)f->rip, (unsigned long)f->cs,
	      (unsigned long)(f->cs & 3),
	      from_user ? "ring 3" : "ring 0");
}

/* ------------------------------------------------- 8259 PIC --------------- */

#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1

#define PIC_EOI    0x20
#define PIC1_VECTOR_BASE 0x20
#define PIC2_VECTOR_BASE 0x28

/*
 * One remap per boot, enforced here rather than at the call sites.
 *
 * tty.c keeps its own `pic_remapped` bool with a comment saying the guard has
 * to be somewhere, and it is right that it is needed — but a guard in each
 * caller cannot see the other callers, and this is now one of several: the
 * timer below remaps because it needs the vector offset, the keyboard remaps
 * because it needs a vector in range. A second ICW1 sequence is not harmless:
 * it re-enters initialisation on a controller that is already in 8086 mode
 * with a cascade wired, and it finishes by masking every line again, which
 * silently disarms the timer that the first caller had just unmasked. Two
 * subsystems each believing they own the sequence is the same hazard
 * idt_set_handler() refuses for a vector, in one place further down.
 */
static bool pic_remapped;

void pic_remap(void)
{
	if (pic_remapped)
		return;
	pic_remapped = true;

	/*
	 * The BIOS leaves both PICs identity-mapped, so IRQ 8 lands on vector 8,
	 * which the CPU has already defined as #DF. The first timer interrupt
	 * would become a double fault on a stack that is in use, which is the
	 * worst possible combination — and it would happen after boot, when
	 * everything else appears to work.
	 *
	 * ICW1 selects 8086 mode and announces ICW4. ICW2 gives the vector
	 * offset. ICW3 wires the cascade: IRQ 2 on the master carries the
	 * slave, and the slave's identity is 2, written into master's IRQ 2
	 * slot. ICW4 is 8086 mode with no automatic EOI.
	 */
	outb(PIC1_CMD, 0x11);
	outb(PIC2_CMD, 0x11);
	outb(PIC1_DATA, PIC1_VECTOR_BASE);
	outb(PIC2_DATA, PIC2_VECTOR_BASE);
	outb(PIC1_DATA, 0x04);
	outb(PIC2_DATA, 0x02);
	outb(PIC1_DATA, 0x01);
	outb(PIC2_DATA, 0x01);

	/*
	 * Both masked. A driver unmasks its own line, because an enabled line
	 * whose handler is not installed would fire forever and the machine
	 * would spend its entire life in interrupt entry.
	 */
	outb(PIC1_DATA, 0xFF);
	outb(PIC2_DATA, 0xFF);

	klog(KLOG_INFO, "pic: remapped to %#x-%#x, both lines masked\n",
	     PIC1_VECTOR_BASE, PIC2_VECTOR_BASE + 8);
}

/*
 * Unmask one line, leaving the others as they were.
 *
 * Read-modify-write rather than a literal, because the mask is shared state:
 * writing a table of "everything except bit n" would re-mask every line some
 * other driver had already enabled. That is not hypothetical here — the timer
 * and the keyboard both live on the master.
 */
static void pic_unmask(uint8_t irq)
{
	uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;

	outb(port, (uint8_t)(inb(port) & ~(1u << (irq & 7))));
}

/* ------------------------------------------------- PIT (8253/8254) -------- */

/*
 * The timer that was missing. Until this existed the kernel had no interrupt
 * source whatsoever: stage2 masks both 8259s at the top of stage2_main()
 * (src/boot/stage2.c, `outb(0x21,0xFF); outb(0xA1,0xFF)`) and nothing in the
 * kernel ever unmasked a line, so no interrupt was ever delivered. That
 * masking is boot-critical and is deliberately left alone — an unmasked IRQ0
 * lands inside a real-mode BIOS round trip in stage2 and executes the
 * bootloader's 32-bit reporter as 16-bit. Unmasking here, in the kernel, after
 * the kernel has installed its own IDT, is the place it was always safe.
 *
 * Channel 0 only. It is the only channel wired to IRQ0 on a PC, and it is the
 * one the PC timer is defined to be.
 */
#define PIT_CH0_DATA   0x40
#define PIT_CH0_MODE   0x43

/*
 * 1193182 Hz is the PC-compatible input clock rounded down; the real value is
 * 1193181.666..., so the measured rate is 0.0002 % low. Nobody has ever
 * noticed and the divisor below cannot express the fraction anyway.
 */
#define PIT_INPUT_HZ   1193182u

/*
 * 100 Hz, not the usual 18.2. The PIT divides by a 16-bit integer only, so the
 * highest rate it can produce is 1193182/65536 ≈ 18.2 Hz — and a divisor that
 * small is exactly what produced the bootloader's problem. 100 Hz costs
 * nothing on the path that matters (the handler is a counter increment) and
 * gives the scheduler five times the resolution.
 */
#define PIT_HZ         100u

/* Divisor 0 and 1 mean 65536 and 65535, not 0 and 1. */
#define PIT_DIVISOR    (PIT_INPUT_HZ / PIT_HZ)

/*
 * Ticks delivered on this CPU. Incremented in interrupt context, so it is
 * volatile and it is never read-modify-written by anything else — there is one
 * writer, on one CPU, and a lost update is not possible.
 */
static volatile u64 pit_ticks;

/*
 * Announced on the first tick and once a second thereafter, through the
 * polled serial path rather than klog.
 *
 * The reason is the whole design constraint. klog_emit() formats into 1.1 KiB
 * of stack and then takes the console spinlock, so a tick arriving while the
 * interrupted code already holds that lock spins forever with IF clear and the
 * machine stops with no output — a hang, indistinguishable from the bug that
 * made this timer worth having. ksnprintf() below is the same formatter with
 * none of the locking, and serial_puts() is the same polled primitive
 * panic() is documented to use from contexts that may hold anything.
 */
static void pit_announce(u64 n)
{
	char line[64];

	(void)ksnprintf(line, sizeof(line), "\r\npit: tick %llu, IRQ0 is live\r\n",
			(unsigned long long)n);
	serial_puts(line);
}

static void pit_irq(struct interrupt_frame *f)
{
	(void)f;
	pit_ticks++;

	if (pit_ticks == 1 || (pit_ticks % PIT_HZ) == 0)
		pit_announce(pit_ticks);

	/*
	 * The documented contract: "The timer driver calls this; the dispatch
	 * path consumes it." Kept, so that the tick means something the moment
	 * a run queue exists. It is a flag and a per-CPU index — no scheduler
	 * state is touched here, which is why it is safe to call before
	 * sched_init().
	 */
	idt_request_reschedule();
}

/*
 * Bring up the PIT and IRQ0. Called from idt_init(), which is the earliest
 * point in the kernel at which a vector exists to be delivered to and the
 * IDTR is loaded; see the comment at the call site.
 */
static void pit_init(void)
{
	/*
	 * The timer cannot work before this: with the BIOS's identity mapping an
	 * IRQ0 tick is vector 8, which the CPU has already defined as #DF. This
	 * is idempotent (see pic_remap) and masks both PICs, so the handler has
	 * to be installed and the channel programmed before the line is
	 * unmasked — an enabled line whose handler is not installed fires
	 * forever and the machine spends its whole life in interrupt entry.
	 */
	pic_remap();

	/*
	 * Control byte 0x34: channel 0, lobyte/hibyte access, mode 2 (rate
	 * generator), binary. Mode 2 reloads the count on every terminal count,
	 * which is what makes the output a steady square wave rather than a
	 * single pulse followed by silence; mode 3 would also work but has a
	 * duty cycle to get right for no benefit here.
	 */
	outb(PIT_CH0_MODE, 0x34);
	outb(PIT_CH0_DATA, (uint8_t)(PIT_DIVISOR & 0xFF));
	outb(PIT_CH0_DATA, (uint8_t)((PIT_DIVISOR >> 8) & 0xFF));

	/* Handler before the line is unmasked, in that order and not the
	 * other way round. */
	idt_set_handler(VECTOR_IRQ_TIMER, pit_irq, IST_NONE, 0);
	pic_unmask(0);

	klog(KLOG_INFO, "pit: ch0 divisor %u = %u.%u Hz, IRQ0 on vector %u "
	     "(first tick in %u us)\n", PIT_DIVISOR,
	     PIT_INPUT_HZ / PIT_DIVISOR, (PIT_INPUT_HZ % PIT_DIVISOR) * 10 / PIT_DIVISOR,
	     VECTOR_IRQ_TIMER, 1000000u / PIT_HZ);
}

/*
 * Ticks delivered since pit_init(). Safe to read from any context: it is a
 * single aligned load of a value only the handler writes.
 */
u64 pit_tick_count(void)
{
	return pit_ticks;
}

void pic_eoi(uint8_t vector)
{
	/*
	 * A cascaded interrupt is delivered to the master, so the slave's
	 * in-service bit is never cleared by acknowledging the master. The
	 * vector range is the only reliable way to tell the two apart.
	 */
	if (vector >= PIC2_VECTOR_BASE && vector < PIC2_VECTOR_BASE + 8)
		outb(PIC2_CMD, PIC_EOI);
	outb(PIC1_CMD, PIC_EOI);
}

/* ------------------------------------------------- dispatch --------------- */

void idt_request_reschedule(void)
{
	need_resched[this_cpu_id() & (MAX_CPUS - 1)] = 1;
}

void interrupt_dispatch(struct interrupt_frame *f)
{
	uint8_t vector = (uint8_t)f->vector;
	irq_handler_t handler = handlers[vector];

	/*
	 * Preemption depth rises before anything else so that an interrupt taken
	 * inside a handler nests correctly, and falls on the way out. The
	 * decrement has to happen for the panicking paths too, or panic() would
	 * believe it is nested in an interrupt and skip its final diagnostics.
	 */
	per_cpu(preempt_count)++;

	if (handler)
		handler(f);

	/*
	 * Acknowledge on the way out, not on the way in: a handler that itself
	 * takes an interrupt must not be re-entered by the same device before it
	 * has finished. Only the 8259 is acknowledged here. The LAPIC EOI the
	 * dispatch sequence in the docs also calls for is a single MMIO write
	 * and is added with APIC bring-up, which does not exist yet.
	 */
	if (vector >= VECTOR_IRQ_BASE && vector < VECTOR_IRQ_MAX)
		pic_eoi(vector);

	per_cpu(preempt_count)--;

	/*
	 * Reschedule only at the outermost level: a timer that fires inside a
	 * handler has interrupted work that is not finished.
	 *
	 * The run-queue check is not a workaround, it is the contract sched.h
	 * states for itself: "One run queue per CPU, allocated by that CPU in
	 * sched_init(). Indexed by this_cpu_id(); NULL before sched_init() has
	 * run on that CPU." sched_tick() dereferences it on its second
	 * statement (`rq->clock = now`), so calling it before then is a NULL
	 * dereference — and a timer turns "the scheduler was not started yet"
	 * from a silent non-event into a #PF on the first tick, 10 ms into the
	 * boot, on whatever code happened to be running.
	 *
	 * As of this change sched_init() has no caller anywhere in the tree, so
	 * this branch never reaches it and a tick only counts. That is the
	 * correct shape for the moment sched_init() is wired up: no change to
	 * this file is needed then, and until then a tick can do no harm.
	 *
	 * The request is cleared whether or not the tick runs. Leaving it set
	 * would be a latch with no reader, and a latch whose only effect is to
	 * surprise whoever reads it next.
	 */
	unsigned cpu = this_cpu_id();

	if (per_cpu(preempt_count) != 0 || !need_resched[cpu & (MAX_CPUS - 1)])
		return;

	need_resched[cpu & (MAX_CPUS - 1)] = 0;

	if (sched_runqueues[cpu])
		sched_tick();
}
