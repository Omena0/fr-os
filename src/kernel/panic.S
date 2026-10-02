/*
 * panic.S — capture the current register state for the panic dump.
 *
 * The registers have to be read here rather than in C: by the time a C function
 * runs, the general-purpose registers already hold the values from the call
 * itself, so the compiler has spilled and reused what the faulting code held.
 * Reading them in the prologue preserves the actual fault context.
 *
 * The SysV argument is a pointer to `struct panic_state`, passed in RDI.
 */

	.text

	.globl panic_regs
	.type panic_regs, @function
panic_regs:
	/* rdi = &panic_state */
	/* Saved first, because the return address below wants RAX as its
	 * scratch register and the caller's RAX is the value that belongs at
	 * offset 0. */
	movq	%rax,  0(%rdi)

	/*
	 * The return address, taken before anything is pushed. At entry (rsp)
	 * is the return address itself, because this function has no prologue
	 * and no frame, so this is the address to return to in the *caller* --
	 * the instruction after its call to panic_regs, which is the closest
	 * thing to a faulting RIP available without a debugger.
	 *
	 * A label inside this function (the obvious `leaq .Lrip(%rip), %rax`)
	 * answers a different question: it yields the address of the panic
	 * handler, so every dump points into the reporter and looks like the
	 * fault happened there.
	 */
	movq	(%rsp), %rax
	movq	%rax, 128(%rdi)

	movq	%rbx,  8(%rdi)
	movq	%rcx, 16(%rdi)
	movq	%rdx, 24(%rdi)
	movq	%rsi, 32(%rdi)
	movq	%rbp, 48(%rdi)
	movq	%rsp, 56(%rdi)
	movq	%r8,   64(%rdi)
	movq	%r9,   72(%rdi)
	movq	%r10,  80(%rdi)
	movq	%r11,  88(%rdi)
	movq	%r12,  96(%rdi)
	movq	%r13, 104(%rdi)
	movq	%r14, 112(%rdi)
	movq	%r15, 120(%rdi)

	/*
	 * RFLAGS, then the control registers.
	 *
	 * struct panic_state puts rip at 128, cs at 136, rflags at 144, cr2 at
	 * 152, cr3 at 160, cr4 at 168, cpu_id at 172.
	 *
	 * These offsets were all one slot low. RFLAGS went to 136 -- the `cs`
	 * slot -- and then CR2, CR3 and CR4 went to 144, 152 and 160, which are
	 * `rflags`, `cr2` and `cr3`. So `cs` printed RFLAGS, `rflags` printed
	 * CR2, `cr2` printed CR3, `cr3` printed CR4, and `cr4` was never written
	 * at all. Every field from 136 upward was one register out, and because
	 * all of them are plausible-looking 64-bit values, nothing about the
	 * output looks wrong -- a panic report that names the wrong registers is
	 * worse than one that omits them, because it is believed.
	 *
	 * `cr4` is written to 168 and `cpu_id` to 172; `movl $0, 172(%rdi)` below
	 * clears it for the caller.
	 *
	 * CS is deliberately left alone. It has no MOV-source form in 64-bit mode
	 * and `push cs` is not encodable either, so this function cannot read it;
	 * the caller-side stub that received the exception knows it and must
	 * store it at 136 before calling. Nothing does that today, so `cs` reads
	 * as whatever was in the buffer -- do not believe it.
	 */
	pushfq
	popq	%rax
	movq	%rax, 144(%rdi)

	/* RDI itself is captured last, after the argument register is no
	 * longer needed to address the output buffer. */
	movq	%rdi, 40(%rdi)

	/* MOV to a control register operand accepts a general-purpose register
	 * only, never a memory operand, so each is loaded through a scratch
	 * register first. RAX is free here: its previous uses have all been
	 * stored, and this function returns nothing. */
	movq	%cr2, %rax
	movq	%rax, 152(%rdi)
	movq	%cr3, %rax
	movq	%rax, 160(%rdi)
	movq	%cr4, %rax
	movq	%rax, 168(%rdi)

	movl	$0, 172(%rdi)		/* cpu_id, filled by the caller */
	ret
	.size panic_regs, . - panic_regs

	.section .note.GNU-stack, "", @progbits
