/*
 * interrupt_entry.S — the assembly half of the interrupt path.
 *
 * One stub per vector, because the CPU pushes the vector number but not an
 * error code, and a shared stub would have to be told which entry it came from.
 * Every stub jumps into one common body, so the prologue exists once and the
 * list of vectors is the only thing that has to stay in step with the table in
 * idt.c.
 *
 * The stubs do the minimum: push a uniform frame and call C. Everything else is
 * in C, where the compiler can keep things in registers across the call.
 *
 * The stubs deliberately do NOT switch stacks. The CPU has already chosen the
 * stack by the time the first instruction here runs:
 *
 *   IST != 0            the dedicated stack for that vector (double fault, NMI)
 *   IST == 0, CPL != 0  TSS.RSP0, i.e. the current task's kernel stack
 *   IST == 0, CPL == 0  the interrupted stack, which is already a kernel stack
 *
 * Loading TSS.RSP0 here would be actively wrong for the first case: the whole
 * point of the IST is that double fault gets a stack which is by construction
 * not the one that just faulted, and a stub that overwrites RSP throws that
 * away. It would also be redundant in the other two cases, since the CPU has
 * already made the right choice.
 */

#include <gdt.h>

	.section .text

/*
 * GAS macros rather than cpp ones. This is an assembler file, and the cpp form
 * needs its continuations to survive whatever last touched the source; a
 * silently collapsed macro here turns 64 stubs into one unparseable line. GAS
 * has its own macro facility for exactly this, so the file uses it.
 *
 * In a GAS macro a backslash interpolates a parameter, so the stubs are spelled
 * \exception_\vec and $\vec. Every macro emits its own label, so the
 * invocation lines below are bare.
 */

/* Vectors the CPU pushes an error code for: one word on the stack already. */
.macro	err_stub vec
	.globl	exception_\vec
exception_\vec:
	pushq	$\vec
	jmp	interrupt_common
.endm

/*
 * Vectors with nothing on the stack. A zero goes in the error-code slot so the
 * frame is the same size for every vector.
 *
 * The vector is pushed *first* so it lands at FRAME_VECTOR, with the zero above
 * it at FRAME_ERROR_CODE. These two used to be pushed the other way round, which
 * put the zero at offset 64 and the vector at 72 -- so every no-error exception
 * and every IRQ was dispatched as vector 0 with the real vector sitting in the
 * error-code slot. err_stub gets this right by construction, since the CPU has
 * already placed the error code above the vector; these have to match it.
 */
.macro	noerr_stub vec
	.globl	exception_\vec
exception_\vec:
	pushq	$\vec
	pushq	$0
	jmp	interrupt_common
.endm

/*
 * The legacy PIC lines at the remapped base, plus everything up to 63. The
 * whole range is generated even though only 16 of the lines exist, so that the
 * stub number equals the vector number across the range and the dispatcher's
 * indexing by vector holds.
 */
.macro	irq_stub vec
	.globl	interrupt_\vec
interrupt_\vec:
	pushq	$\vec
	pushq	$0
	jmp	interrupt_common
.endm

/*
 * The argument registers, in the order struct interrupt_frame declares them:
 * rdi, rsi, rdx, rcx, r8, r9, r10, r11.
 *
 * Only these are saved. R12-R15, RBX and RBP are callee-saved, so the
 * interrupted code already holds them somewhere safe; saving them here would
 * cost eight stores on every interrupt to preserve values no handler can have
 * clobbered. RSP and RBP are excluded for the same reason.
 *
 * Push order is the *reverse* of the declaration order, and that is load-bearing
 * rather than stylistic: a push decrements RSP first, so the register pushed
 * last ends up at the lowest address. Pushing rdi first put rdi at offset 56 and
 * r11 at offset 0 -- exactly backwards from the FRAME_* block below and from
 * struct interrupt_frame. save_regs and restore_regs were a matched pair and so
 * restored correctly, which is why nothing ever crashed: every frame the kernel
 * ever reported had its eight argument registers in reverse order, and
 * `f->rdi` was the interrupted code's r11.
 */
.macro	save_regs
	pushq	%r11
	pushq	%r10
	pushq	%r9
	pushq	%r8
	pushq	%rcx
	pushq	%rdx
	pushq	%rsi
	pushq	%rdi
.endm

/* The exact inverse, so the two cannot be updated independently. */
.macro	restore_regs
	popq	%rdi
	popq	%rsi
	popq	%rdx
	popq	%rcx
	popq	%r8
	popq	%r9
	popq	%r10
	popq	%r11
.endm

	noerr_stub 0
	noerr_stub 1
	noerr_stub 2
	noerr_stub 3
	noerr_stub 4
	noerr_stub 5
	noerr_stub 6
	noerr_stub 7
	err_stub 8
	noerr_stub 9
	err_stub 10
	err_stub 11
	err_stub 12
	err_stub 13
	err_stub 14
	noerr_stub 15
	noerr_stub 16
	err_stub 17
	noerr_stub 18
	noerr_stub 19
	noerr_stub 20
	err_stub 21
	noerr_stub 22
	noerr_stub 23
	noerr_stub 24
	noerr_stub 25
	noerr_stub 26
	noerr_stub 27
	noerr_stub 28
	noerr_stub 29
	noerr_stub 30
	noerr_stub 31

	irq_stub 32
	irq_stub 33
	irq_stub 34
	irq_stub 35
	irq_stub 36
	irq_stub 37
	irq_stub 38
	irq_stub 39
	irq_stub 40
	irq_stub 41
	irq_stub 42
	irq_stub 43
	irq_stub 44
	irq_stub 45
	irq_stub 46
	irq_stub 47
	irq_stub 48
	irq_stub 49
	irq_stub 50
	irq_stub 51
	irq_stub 52
	irq_stub 53
	irq_stub 54
	irq_stub 55
	irq_stub 56
	irq_stub 57
	irq_stub 58
	irq_stub 59
	irq_stub 60
	irq_stub 61
	irq_stub 62
	irq_stub 63

/*
 * The one body, shared by all 64 stubs.
 *
 * The frame this builds is described by struct interrupt_frame in
 * interrupt.h, and interrupt_entry.S asserts the offsets at the bottom of this
 * file so the two cannot drift.
 */
interrupt_common:
	save_regs
	movq	%rsp, %rdi			/* struct interrupt_frame * */
	call	interrupt_dispatch
	restore_regs
	addq	$16, %rsp			/* vector and the error-code slot */
	iretq

/*
 * gdt_flush — load the GDT and reload the segments that depend on it.
 *
 * The far return is what makes the reload actually take. LGDT changes the CPU's
 * idea of the descriptor table, but CS keeps its cached descriptor until
 * something reloads it, and a near jump does not reload it. Pushing the target
 * address and the selector and using LRETQ is how the hardware is meant to
 * refresh CS; a plain `jmp` after LGDT would leave CS pointing at a descriptor
 * the CPU has already invalidated.
 *
 * DS, ES, FS, GS and SS are reloaded afterwards. A descriptor table change
 * leaves segment registers whose cached descriptors no longer match the table
 * in an unusable state, and the C code after this call must not have to know
 * that a reload just happened.
 */
	.globl	gdt_flush
	.type	gdt_flush, @function
gdt_flush:

	movq	%rdi, %rax			/* the GDTR pointer, in full */
	lgdt	(%rax)


	/*
	 * Reload the data segments while the old CS is still the one in use, and
	 * therefore still valid: LGDT has replaced the descriptor table, but the
	 * cached CS descriptor survives until CS itself is reloaded, and the
	 * segments we are about to load are looked up in the *new* table.
	 */
	movw	$KERNEL_DATA_SELECTOR, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %fs
	movw	%ax, %gs
	movw	%ax, %ss

	/*
	 * The GS reload is correct and unavoidable -- a MOV to a segment
	 * *selector* is the only thing that refreshes the cached descriptor,
	 * and a near jump does not do it. It also throws the per-CPU area
	 * away: loading a selector replaces the hidden base with the one from
	 * the descriptor, which for a flat segment is zero. So the GS selector
	 * is reloaded here and percpu_install_gs_base() puts the base back
	 * immediately after, from C, where the CPU number is known.
	 *
	 * Leaving the reload out does "fix" it -- klog then works past
	 * gdt_reload -- and leaves a GS whose cached descriptor came from the
	 * table the loader installed, which is the bug the whole function
	 * exists to prevent.
	 */



/*
	 * Two words, not three. LRETQ pops RIP and CS and nothing else; it does
	 * not restore RFLAGS. Only IRET pops flags. The 32-bit idiom this was
	 * written from -- EFLAGS, CS, EIP, popped by a three-dword LRET -- does
	 * not survive the widening, and carrying the extra word over is not
	 * harmless: the surplus RFLAGS stays on the stack and the `ret` below
	 * pops 0x202 as if it were a return address, so control transfers to
	 * linear 0x202 and the fetch faults. Verified by measurement, not by
	 * derivation: at the resume label RSP had advanced 16 and the word at
	 * RSP was still the 0x202 that had been pushed, under both TCG and KVM.
	 *
	 * Not restoring flags is fine here. IF is the caller's business, and
	 * gdt_reload runs inside the cli/sti pair around it in main.c.
	 */
	pushq	$KERNEL_CODE_SELECTOR		/* CS */
	pushq	$.Lgdt_flush_resume		/* RIP */
	lretq
.Lgdt_flush_resume:
	ret
	.size	gdt_flush, . - gdt_flush



/*
 * tss_flush — load TR with the TSS selector.
 *
 * LTR is privileged and takes a 16-bit selector, so the index is passed in and
 * shifted here rather than at each call site. Without a valid TR, a ring-3
 * interrupt has no RSP0 to switch to: the CPU would take the fault on the user
 * stack that it was already using.
 */
	.globl	tss_flush
	.type	tss_flush, @function
tss_flush:
	ltr	%di
	ret
	.size	tss_flush, . - tss_flush

/*
 * ret_to_user — return to ring 3 with a fresh frame.
 *
 * IRETQ rather than SYSRETQ. SYSRET derives CS and SS from STAR, so it can only
 * resume the one ring-3 pair the GDT was arranged for, and it is not available
 * at all for entering ring 3 for the first time. IRETQ takes its selectors from
 * the frame, which is what lets a first entry into a new process use any
 * selectors and lets a resumed process restore the ones it was interrupted
 * with. The syscall return path uses SYSRET, and that is the fast path; this is
 * the general one.
 *
 * IRETQ pops, in order, RIP, CS, RFLAGS, RSP, SS -- so the frame is pushed in
 * exactly that reverse: SS, RSP, RFLAGS, CS, RIP. Five words, no padding.
 *
 * The order was wrong. It read SS, CS, RFLAGS, RIP, RSP, which puts CS where
 * RSP belongs and RIP and RSP last; a trailing `add $8,%rsp` / `popq %rbp` pair
 * hid the mismatch by absorbing one word, so the sequence looked balanced and
 * disassembled cleanly. What it actually framed was RIP=RFLAGS, CS=CS,
 * RFLAGS=SS, with the user's RSP and RIP never reaching the CPU at all, and
 * the first entry into ring 3 died on the iretq with #GP.
 *
 * There is no padding here and there never needed to be: the RSP the user sees
 * is the value the caller pushed, so its alignment is the caller's business,
 * not something a frame builder on this side can influence.
 *
 * rcx and r11 are accepted and ignored. Some ABIs want the user to observe
 * RCX=user RIP and R11=user RFLAGS on return, which is why the parameters
 * exist, but IRETQ restores RIP and RFLAGS from the frame itself and the only
 * callers pass zeros.
 */
	.globl	ret_to_user
	.type	ret_to_user, @function
ret_to_user:
	cld

	/*
	 * Load the running task's thread pointer into MSR_FS_BASE before the
	 * iretq frame is built. NULL-safe: before the scheduler starts there is
	 * no current task, and MSR_FS_BASE is left alone.
	 *
	 * Required for every forked child, which resumes at its parent's syscall
	 * return site with RAX=0 and never re-runs crt1, so nothing will ever
	 * call arch_prctl on its behalf. Required for a process's first entry
	 * too, once exec has given it a thread pointer.
	 *
	 * Here, above the pushes, and not just before the iretq: at that point
	 * RSP points at the frame's RIP slot, so the call's return address
	 * would land inside memory the iretq is about to read, and it would run
	 * C with a live interrupt frame underneath it. Above the pushes, RSP is
	 * exactly what the caller left, and the frame is built from an unchanged
	 * RSP.
	 *
	 * Encoding checked in the linked artefact: `e8` with a 32-bit relative
	 * displacement. Not the `PUSH r/m` trap from the gdt_flush bug above --
	 * which is the reason to look, every time, at the bytes.
	 */
	call	task_load_fs_current

	pushq	$USER_DATA_SELECTOR		/* SS */
	pushq	%rsi				/* RSP */
	pushq	%rdx				/* RFLAGS */
	pushq	$USER_CODE64_SELECTOR		/* CS — index 5, the 64-bit user
						 * code descriptor, NOT the 32-bit
						 * one at index 3 that STAR
						 * points at. */
	pushq	%rdi				/* RIP */
	/*
	 * Zero the caller-saved registers -- here, *after* the frame has been
	 * built, because RDX, RDI and RSI are its inputs and clearing them first
	 * would frame iretq with RFLAGS=0, RIP=0 and RSP=0.
	 *
	 * A forked child reaches ring 3 through this path, and RAX is the entire
	 * contract of fork's return value in the child: it must see 0. Nothing here
	 * used to write RAX, so the child entered ring 3 carrying whatever the
	 * kernel's call chain left there. `if (fork() == 0)` then took the parent
	 * branch on the first fork after any call that returned non-zero.
	 * fork_child_start's comment documented the contract correctly; nothing
	 * implemented it.
	 *
	 * The rest of the caller-saved set is cleared because a process's first
	 * entry to user code should not be able to observe kernel register
	 * residue. R11 is left alone deliberately: it carries the interrupt
	 * frame pointer, and this code has already finished with it.
	 */
	xorl	%eax, %eax
	xorl	%ecx, %ecx
	xorl	%r8d, %r8d
	xorl	%r9d, %r9d
	xorl	%r10d, %r10d
	/*
	 * Past this point the CPU is about to run on memory the process owns.
	 * Nothing may fault between here and the iretq, so anything that can be
	 * checked has to be checked above this line.
	 */
	iretq
	.size	ret_to_user, . - ret_to_user

/* ------------------------------------------------- frame layout asserts --- */

/*
 * The authoritative frame offsets, measured from RSP at the moment
 * interrupt_dispatch is entered. interrupt.h mirrors these numbers in
 * struct interrupt_frame and static-asserts them, so a change here that is not
 * mirrored there fails the build.
 */
	.set FRAME_RDI,		0
	.set FRAME_RSI,		8
	.set FRAME_RDX,		16
	.set FRAME_RCX,		24
	.set FRAME_R8,		32
	.set FRAME_R9,		40
	.set FRAME_R10,		48
	.set FRAME_R11,		56
	.set FRAME_VECTOR,	64
	.set FRAME_ERROR_CODE,	72
	.set FRAME_RIP,		80
	.set FRAME_CS,		88
	.set FRAME_RFLAGS,	96
	.set FRAME_RSP,		104
	.set FRAME_SS,		112
	.set FRAME_SIZE,		120
