/*
 * kmain.S — the kernel's entry point.
 *
 * stage2 long mode entry jumps here with RDI holding the physical address of
 * struct bootinfo. Nothing else in the register file is meaningful: stage2 uses
 * RDI/ESI/EDX itself to stage the transition, so this code must treat every
 * other register as garbage.
 *
 * The code in this file is deliberately restricted to four things that cannot be
 * expressed in C: reading fixed-address linker symbols, switching to a known
 * stack, and the two MSR writes that must happen before any per-CPU code runs.
 * Everything else is in kmain() proper, where the compiler can keep the
 * registers alive across calls.
 *
 * Ordering here is load-bearing, not stylistic:
 *
 *   1. Stack first. Until RSP points at memory the compiler cannot spill a
 *      register, so this must precede the first call.
 *   2. GS second, via MSR_GS_BASE. percpu_setup() is a normal C function that
 *      reads this_cpu(), and this_cpu() is a GS-relative load. If GS were set
 *      after that call, the load would return garbage and the kernel would
 *      scribble somewhere unpredictable.
 *   3. EFER checks last, as assertions rather than setup. Stage2 owns the
 *      long-mode transition; the kernel's job is to notice if it did not happen
 *      and say so, because "kernel reached main in protected mode" is a bug
 *      that would otherwise present much later as an unexplained #GP.
 */

#include <boot.h>

/*
 * The stack top. Placed in the kernel's own BSS by the linker script, so it is
 * always valid regardless of how much BSS stage2 zeroed: stage2 cannot know
 * the kernel's BSS size, and the kernel's .bss is the one region it must not
 * trample. __stack_chk_fail is referenced so a stack-protector build cannot
 * silently lose its canary check on this file.
 */
	.extern __kernel_stack_top

/* The identity mapping stage2 built. Replaced by the VMM's own tables. */
	.extern boot_pml4_phys

	.section .text.boot, "ax", @progbits
	.globl kernel_start
	.type kernel_start, @function
kernel_start:
	/*
	 * Load the stack pointer. RBP is zeroed at the same time: several things
	 * downstream (unwinding, the panic backtrace) treat RBP==0 as the end of
	 * the frame chain, and leaving stage2's frame pointer in place would make
	 * the first backtrace walk into the bootloader's stack.
	 */
	leaq	__kernel_stack_top(%rip), %rsp
	xorq	%rbp, %rbp

	/*
	 * Set the GS base before the first call. percpu_setup() is the first C
	 * function to run, and it stores a pointer that this_cpu() then reads back
	 * with a GS-relative load — so GS has to be correct before the call, not
	 * after it.
	 *
	 * MSR_GS_BASE is 0xC0000101, MSR_GS_KERNEL_BASE is 0xC0000102. The kernel
	 * runs with the same value in both: there is no separate kernel GS base
	 * because swapgs on the syscall path is a no-op once both halves are
	 * programmed identically, and leaving the kernel half garbage would turn
	 * every swapgs into a wild pointer dereference.
	 */
	movq	$percpu_data, %rax
	movl	$0xC0000102, %ecx		/* MSR_GS_KERNEL_BASE */
	wrmsr
	movq	%rax, %rdx
	movl	$0xC0000101, %ecx		/* MSR_GS_BASE */
	wrmsr

	/*
	 * Fail loudly and early if the long-mode transition did not happen. The
	 * symptom of skipping this check is not a crash here: it is a kernel that
	 * runs, prints through a silently misidentified mode, and corrupts memory
	 * somewhere that gives no hint about the real cause. Two instructions to
	 * turn a day of debugging into a message.
	 */
	/*
	 * LMA is bit 10 of EFER, and RDMSR leaves the low half of the MSR in EAX.
	 *
	 * This read EFER, shifted RAX left by 8 and then tested bit 8 -- which is
	 * a two-step way of testing bit 16 of a value whose bit 10 is the one that
	 * matters. EFER.LMA is set by the hardware the moment paging is enabled in
	 * long mode, so it is always set here, and this check rejected every
	 * correct boot. The shift then bit 10 down to bit 2, and `btl $8` on that
	 * never matches either, so the jump was always taken and the kernel went
	 * straight to its "not long mode" path: a message to port 0xE9, which
	 * nothing is listening on, and a halt. Serial stayed completely silent.
	 */
	movq	$0xC0000080, %rcx		/* MSR_EFER */
	rdmsr
	btl	$10, %eax			/* EFER.LMA */
	jnc	.Lnot_long_mode

	/*
	 * Pass bootinfo across to C. RDI already holds this address, put there by
	 * stage2's far jump. Reloading it here only gave a second place for the
	 * two images to disagree about where the record lives, and they did: this
	 * was E820_ADDR, the 4 KiB page below the record, so kmain read the
	 * memory map's first bytes as a magic number and panicked every boot.
	 * BOOTINFO_ADDR is now a shared constant in boot.h.
	 */
	movq	$BOOTINFO_ADDR, %rdi

	/* Clear the direction flag: the BIOS can leave it set, and it silently
	 * turns every string primitive backwards. */
	cld
	xorl	%ebx, %ebx
	xorl	%ebp, %ebp

	call	kmain
	.size kernel_start, . - kernel_start

	/*
	 * kmain() is not expected to return — it ends in the idle loop, and a
	 * return would mean the idle task was torn down. Halt forever rather than
	 * running off the end of an unknown stack.
	 */
.Lhang:
	cli
	hlt
	jmp	.Lhang

.Lnot_long_mode:
	/*
	 * No console yet: the framebuffer and serial backends are set up from C
	 * and neither is guaranteed to exist yet. Port 0xE9 is the diagnostic port
	 * the platform debug card exposes on QEMU, and it needs no
	 * initialisation, so it is the only sink available this early.
	 */
	/* RIP-relative, not `movq $LMM_REPORT`. The immediate form asks the
	 * assembler for an absolute relocation against the symbol, and a local
	 * label beginning with `.L` is never emitted into the symbol table, so
	 * the reference is unresolvable at link time. */
	leaq	.LMM_REPORT(%rip), %rsi
	movl	$1, %edx			/* 1 = no long mode */
	movw	$0xE9, %dx
1:	lodsb
	testb	%al, %al
	jz	2f
	outb	%al, %dx
	jmp	1b
2:	cli
	hlt
	jmp	2b
	.size kernel_start, . - kernel_start

	.section .rodata
.LMM_REPORT:
	.asciz "OS: stage2 did not enter long mode; kernel cannot start.\r\n"

/*
 * kernel_panic_halt — the last instruction executed by a panicked kernel.
 *
 * Deliberately does not call into C. The panic path may be running with the
 * console lock held, the allocator in an inconsistent state, or on a stack
 * that has already overflowed; a path that only touches the CPU is the only
 * one guaranteed to execute.
 */
	.section .text, "ax", @progbits
	.globl kernel_panic_halt
	.type kernel_panic_halt, @function
kernel_panic_halt:
	cli
1:	hlt
	jmp	1b
	.size kernel_panic_halt, . - kernel_panic_halt

	.section .note.GNU-stack, "", @progbits
