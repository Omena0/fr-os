/*
 * interrupt.h — the vector table, the entry frame, and the exception policy.
 *
 * The design this implements is written down in
 * docs/src/kernel/interrupt-handling.md; the frame layout below is the part
 * that assembly depends on, so it is duplicated here in the same order and
 * asserted against the stubs at compile time.
 */
#ifndef INTERRUPT_H
#define INTERRUPT_H

#include <types.h>

/* ------------------------------------------------------------- vectors ----- */

/* The first 32 are the architectural exceptions; the CPU defines these numbers
 * and nothing may renumber them. */
#define VECTOR_DIVIDE_ERROR            0
#define VECTOR_DEBUG                   1
#define VECTOR_NMI                     2
#define VECTOR_BREAKPOINT              3
#define VECTOR_OVERFLOW                4
#define VECTOR_BOUND_RANGE             5
#define VECTOR_INVALID_OPCODE          6
#define VECTOR_DEVICE_NOT_AVAILABLE    7
#define VECTOR_DOUBLE_FAULT            8
#define VECTOR_INVALID_TSS            10
#define VECTOR_SEGMENT_NOT_PRESENT    11
#define VECTOR_STACK_FAULT            12
#define VECTOR_GENERAL_PROTECTION     13
#define VECTOR_PAGE_FAULT             14
#define VECTOR_FP_ERROR               16
#define VECTOR_ALIGNMENT_CHECK        17
#define VECTOR_MACHINE_CHECK          18
#define VECTOR_SIMD_FP_ERROR          19
#define VECTOR_CONTROL_PROTECTION     21

#define VECTOR_EXCEPTIONS_MAX         32

/* The remapped 8259 lines. The BIOS leaves the PICs identity-mapped, which
 * collides with the CPU's own exception vectors, so these are moved up to 32.
 *
 * `vector == VECTOR_IRQ_BASE + irq`, exactly. The block used to start the
 * numbering at IRQ *one* -- timer 33, keyboard 34, COM1 37 -- while
 * `PIC1_VECTOR_BASE` is 0x20, so every line was delivered one vector lower than
 * the vector its handler was installed on. Nothing about that is visible from
 * inside: the timer landed on vector 32 where `pit_irq` was not, the keyboard
 * on 33, and the read-modify-write in `pic_eoi()` derived the IRQ number from
 * the wrong vector as well.
 *
 * It is measured, not inferred. `-d int` shows the timer arriving as INT=0x20
 * and the keyboard as INT=0x21, which is the remap working correctly and these
 * constants disagreeing with it. The kernel's own boot log agrees: "pic:
 * remapped to 20-30" and "IRQ0 on vector 33" in the same run.
 */
#define VECTOR_IRQ_BASE               32
#define VECTOR_IRQ_TIMER              32	/* IRQ0 */
#define VECTOR_IRQ_KEYBOARD           33	/* IRQ1 */
#define VECTOR_IRQ_CASCADE            34	/* IRQ2 */
#define VECTOR_IRQ_COM2               35	/* IRQ3 */
#define VECTOR_IRQ_COM1               36	/* IRQ4 */
#define VECTOR_IRQ_PARALLEL           37	/* IRQ5, LPT2 or primary IDE */
#define VECTOR_IRQ_FLOPPY             38	/* IRQ6, floppy controller */
#define VECTOR_IRQ_RTC                40	/* IRQ8, CMOS/RTC -- the first
						 * line on the *slave*, so
						 * this is also the
						 * master/slave boundary
						 * IRQ2 == VECTOR_IRQ_MAX */
#define VECTOR_IRQ_MAX                48

/*
 * The master/slave boundary, named so that both sides of it can be asserted
 * against the same constant. IRQ 0-7 arrive on the master at
 * VECTOR_IRQ_BASE + 0..7; IRQ 8-15 arrive on the slave at VECTOR_IRQ_BASE + 8..15.
 * pic_eoi() branches on exactly this split, so it is the one boundary where a
 * wrong answer acknowledges the wrong controller.
 */
#define VECTOR_IRQ_SLAVE_FIRST  (VECTOR_IRQ_BASE + 8)
#define PIC_IRQ_COUNT           16

/*
 * Every constant above is `VECTOR_IRQ_BASE + irq`, and the whole block was once
 * one higher than that for every line -- the off-by-one this file's neighbours
 * have been rewritten around three times. The values are asserted rather than
 * derived at the use site because the use sites are exactly where a silent slip
 * is invisible: a handler installed one vector above the one the controller
 * raises produces no fault, no diagnostic, and an interrupt that silently runs
 * somebody else's handler.
 *
 * The 8259 remap in idt.c is the other half of the pair, and asserts the same
 * identity from its side (PIC1_VECTOR_BASE == VECTOR_IRQ_BASE). Between the two
 * sets a change to either end that is not a change to both fails the build.
 */
_Static_assert(VECTOR_IRQ_TIMER    == VECTOR_IRQ_BASE + 0,
	       "IRQ0 must be VECTOR_IRQ_BASE + 0: the 8259 master is remapped to "
	       "PIC1_VECTOR_BASE, so vector == base + irq with nothing in between");
_Static_assert(VECTOR_IRQ_KEYBOARD == VECTOR_IRQ_BASE + 1, "IRQ1 is off by one");
_Static_assert(VECTOR_IRQ_CASCADE  == VECTOR_IRQ_BASE + 2,
	       "IRQ2 is the cascade and is never acknowledged directly");
_Static_assert(VECTOR_IRQ_COM2     == VECTOR_IRQ_BASE + 3, "IRQ3 is off by one");
_Static_assert(VECTOR_IRQ_COM1     == VECTOR_IRQ_BASE + 4, "IRQ4 is off by one");
_Static_assert(VECTOR_IRQ_PARALLEL == VECTOR_IRQ_BASE + 5, "IRQ5 is off by one");
_Static_assert(VECTOR_IRQ_FLOPPY   == VECTOR_IRQ_BASE + 6, "IRQ6 is off by one");

/*
 * `VECTOR_IRQ_RTC` was 37 with the comment "IRQ6 on a PC; IRQ5 is LPT2". Both
 * halves of that were wrong, and the value contradicted the comment it was
 * carrying: 37 is VECTOR_IRQ_BASE + 5, i.e. IRQ5, which is exactly the LPT2 the
 * comment says it is not. The AT RTC/CMOS line is IRQ8, on the slave.
 *
 * Nothing used the constant, so nothing was delivered to the wrong vector by
 * this. It is corrected because the alternative is a name and a value that
 * disagree, which is the condition the off-by-one lived in for a day, and the
 * assert below is what noticed.
 */
_Static_assert(VECTOR_IRQ_RTC == VECTOR_IRQ_BASE + 8,
	       "the CMOS/RTC line is IRQ8 on AT hardware, which is the first line "
	       "on the slave; 37 is IRQ5 (LPT2) and the old comment claimed 38 "
		       "(floppy), so the constant and its comment disagreed");
_Static_assert(VECTOR_IRQ_RTC == VECTOR_IRQ_SLAVE_FIRST,
	       "IRQ8 is the first slave line, so pic_eoi()'s master/slave split "
		       "must fall exactly at VECTOR_IRQ_RTC");
_Static_assert(VECTOR_IRQ_MAX == VECTOR_IRQ_BASE + PIC_IRQ_COUNT,
	       "VECTOR_IRQ_MAX must cover all 16 lines the two 8259s can raise, "
	       "or interrupt_dispatch() will leave a real interrupt unacknowledged "
	       "and the controller stops raising it");

/* The highest vector for which interrupt_entry.S generates a stub. Everything
 * from here to 255 has a gate but no stub, and a vector that arrives without
 * one is absorbed by the default handler. */
#define VECTOR_STUB_MAX               64

/* Local APIC device vectors, and the inter-processor interrupts. See
 * "IPIs" in docs/src/kernel/interrupt-handling.md. */
#define VECTOR_APIC_BASE              48
#define VECTOR_IPI_BASE              240
#define VECTOR_IPI_TLB_SHOOTDOWN     240
#define VECTOR_IPI_RESCHEDULE        241
#define VECTOR_IPI_HALT              242
#define VECTOR_IPI_MAX               255
#define VECTOR_SPURIOUS              255

#define IDT_ENTRIES                   256

/* ------------------------------------------------------------- IST --------- */

/* IST indices, matching the CPU's IST1..IST7. Zero means "no dedicated stack",
 * which is the normal case: the CPU then uses TSS.RSP0 for a privilege change,
 * or leaves the interrupted stack alone if there is none. */
#define IST_NONE         0
#define IST_DOUBLE_FAULT 1
#define IST_NMI          2

/*
 * IST stacks are not installed yet — see the comment on gdt_set_ist_stack() in
 * gdt.c. The 32-bit IST field cannot address a page the high-half kernel can
 * reach, so it needs a fixed low mapping that the VMM does not expose yet.
 *
 * idt.c asks this before putting an IST index in a gate. A gate that claims an
 * IST slot with a null pointer behind it does not fall back to the normal
 * stack; it takes a #PF on entry, which is worse than having no IST.
 */
void gdt_set_ist_stack(uint8_t ist, void *stack_top);
bool gdt_have_ist(uint8_t ist);

/* ------------------------------------------------- interrupt frame -------- */

/*
 * The frame the stubs build, mirrored exactly by interrupt_entry.S.
 *
 * Only the caller-saved registers are saved. R12-R15, RBX and RBP are callee-saved,
 * so the interrupted code already holds them somewhere safe and an interrupt
 * handler that clobbers them would be a bug in the handler, not a lost value.
 * RSP and RBP are excluded for the same reason.
 *
 * RAX belongs to the first list, not the second, and leaving it out is the
 * subtlest bug in this file. The eight registers that used to be saved are the
 * System V caller-saved set *minus RAX* -- not a smaller job, the same list
 * with the one register every C function is allowed to destroy left out. The
 * CPU does not restore general-purpose registers on IRETQ, so a handler that
 * clobbers RAX silently changes the interrupted code's RAX.
 *
 * From ring 0 that is nearly invisible: the interrupted code is the kernel, and
 * a clobbered RAX is usually a return value somebody was going to check anyway.
 * From ring 3 it is fatal and it is silent -- nothing crashes at the exception,
 * and the faulting instruction is re-executed with a different RAX than it had.
 *
 * Measured: init's allocator stored through a pointer it had just been handed
 * by mmap(). The store faulted on a not-present page, the demand fill serviced
 * it correctly, and the CPU re-executed the store at the *same* address with
 * RAX now 0 rather than the mapping. The page was filled; the process had lost
 * the only pointer to it, and the next store went to address 0x10000.
 *
 * `rip` through `ss` are the CPU's own IRETQ frame. `rsp` and `ss` describe the
 * interrupted context; they are only meaningful when `cs` shows ring 3, and for
 * a kernel-origin interrupt the CPU pushes a zero SS.
 */
struct interrupt_frame {
	uint64_t rax;
	uint64_t rdi;
	uint64_t rsi;
	uint64_t rdx;
	uint64_t rcx;
	uint64_t r8;
	uint64_t r9;
	uint64_t r10;
	uint64_t r11;
	uint64_t vector;
	uint64_t error_code;
	uint64_t rip;
	uint64_t cs;
	uint64_t rflags;
	uint64_t rsp;
	uint64_t ss;
};

/*
 * The stubs live in interrupt_entry.S and cannot be given a C struct, so the
 * layout is written twice — once as push order there, once as fields here.
 * These asserts pin the C side against the documented numbers, and the
 * .set FRAME_* block in the assembly is the same list; the build fails if a
 * field is added or reordered in only one of the two.
 */
_Static_assert(offsetof(struct interrupt_frame, vector) == 72,
	       "interrupt_entry.S pushes the vector at offset 72");
_Static_assert(offsetof(struct interrupt_frame, error_code) == 80,
	       "interrupt_entry.S pushes the error code at offset 80");
_Static_assert(offsetof(struct interrupt_frame, rip) == 88,
	       "interrupt_entry.S starts the CPU frame at offset 88");
_Static_assert(offsetof(struct interrupt_frame, cs) == 96, "bad cs offset");
_Static_assert(offsetof(struct interrupt_frame, rflags) == 104,
	       "bad rflags offset");
_Static_assert(offsetof(struct interrupt_frame, rsp) == 112, "bad rsp offset");
_Static_assert(offsetof(struct interrupt_frame, ss) == 120, "bad ss offset");
_Static_assert(sizeof(struct interrupt_frame) == 128,
	       "the stub pops 16 bytes of vector and error code after 9 saved "
	       "registers; a size change here has to change the stub too");

/* Raised on ring 3 by a POP SS or interrupt, to prevent an attacker from
 * slipping a second stack frame in between the two CPU pushes. */
#define ERROR_CODE_INJECTED  (1ULL << 31)

/* Page-fault error-code bits, per the SDM's table "Error code" in §14.5. */
#define PF_PRESENT        (1ULL << 0)
#define PF_WRITE          (1ULL << 1)
#define PF_USER           (1ULL << 2)
#define PF_RESERVED       (1ULL << 3)
#define PF_FETCH          (1ULL << 4)
#define PF_PROTECTION_KEY (1ULL << 5)

static inline bool frame_from_user(const struct interrupt_frame *f)
{
	return (f->cs & 3) == 3;
}

/* ------------------------------------------------------------- API --------- */

typedef void (*irq_handler_t)(struct interrupt_frame *frame);

/*
 * Install a handler for a vector.
 *
 * `ist` is an IST index or IST_NONE. `dpl` is the descriptor's privilege level:
 * 0 for everything except VECTOR_BREAKPOINT, which is 3 so that user code can
 * raise int3.
 *
 * Installing over an existing handler is a bug rather than a replacement. Two
 * drivers both believing they own the timer is a race that shows up as a
 * keyboard interrupt occasionally running the timer handler.
 */
void idt_set_handler(uint8_t vector, irq_handler_t handler, uint8_t ist,
		     uint8_t dpl);

/* Build the IDT, install the IST stacks, and load IDTR. */
void idt_init(void);

/* Install handlers for vectors 0-31. Must run before idt_init(). */
void exceptions_init(void);

/*
 * Shared body of all 64 IRQ stubs. Called from assembly with interrupts already
 * masked and RSP switched to the kernel stack.
 */
void interrupt_dispatch(struct interrupt_frame *frame);

/*
 * Ask for a reschedule at the next outermost interrupt exit. The timer driver
 * calls this; the dispatch path consumes it. Kept as a request rather than a
 * flag the driver pokes directly, so that the per-CPU indexing and the
 * outermost-level check stay in one place.
 */
void idt_request_reschedule(void);

/* The 8259 remap and EOI. Owned by this file because the vector layout above is
 * what the remap exists to produce. */
void pic_remap(void);
void pic_eoi(uint8_t vector);

/*
 * The two 8259 mask registers as one value: master in the low byte, slave in
 * the high. Bit n of the combined value is line n's mask, so bit 0 clear means
 * IRQ0 is *unmasked*. Read from the controllers rather than from a cached copy,
 * so a driver that enables a line behind this file's back is visible.
 */
u16 pic_mask_state(void);

/*
 * Log every remapped line with its mask state and whether a subsystem has
 * claimed it. Answers "which lines can actually fire?" from the hardware rather
 * than from the source, which is the only channel that has been right about the
 * 8259s in this tree.
 */
void pic_report_lines(void);

/* Ticks delivered since the PIT was programmed. Safe to read from any
 * context: a single aligned load of a value only the handler writes. */
u64 pit_tick_count(void);

/* ---------------------------------------------------------- signal hook ---- */

/*
 * Deliver a signal to the task described by `frame`. See "Exception Handling"
 * in docs/src/kernel/interrupt-handling.md.
 *
 * Weak: the signal subsystem is specified but not yet implemented, and the
 * kernel has to link without it. Until it exists the exception layer falls back
 * to terminating the task. When it is implemented this call site picks it up
 * with no change, which is the whole point of routing through one function.
 */
void process_deliver_signal(int signo, struct interrupt_frame *frame)
	__attribute__((weak));

/*
 * Terminate the task described by `frame`. Used for exceptions that are fatal to
 * the process when no signal machinery exists to report them more gracefully.
 *
 * Also weak, for the same reason. If neither hook is present the exception layer
 * panics rather than continuing; see "Exception Handling" in
 * docs/src/kernel/interrupt-handling.md.
 */
void process_terminate_from_fault(struct interrupt_frame *frame)
	__attribute__((weak));

#endif /* INTERRUPT_H */
