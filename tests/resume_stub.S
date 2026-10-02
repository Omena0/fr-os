/*
 * resume_stub.S — the resume points of the host harness.
 *
 * Two different things have to be observed at the exact instant control
 * arrives after a context switch, and neither can be observed from C:
 *
 *   resume_stub_asm is the address every fake task's frame is built with, the
 *   analogue of the instruction after `call context_switch` in schedule(). It
 *   is naked assembly because a compiled C function is entitled to overwrite
 *   rbx/rbp/r12-r15 with its own spill slots before executing a single
 *   instruction, so a C trampoline could only ever check the frame offset and
 *   never the register contract. Capturing the registers here, before any
 *   compiler-generated code runs, is the only way to observe them.
 *
 *   switch_check_asm performs the switch and checks the frame offset on *every*
 *   resume, not just the first: `ret` has consumed the return address by the
 *   time it lands at resume_stub_asm, so rsp must be exactly 56 above the
 *   frame that was handed to context_switch() — 48 for the six saved registers
 *   plus the 8-byte return address. The comparison happens in the two
 *   instructions immediately after the call, with nothing in between to move
 *   rsp.
 *
 * The frame being restored is exactly what src/kernel/context.S restores:
 *
 *     +0 r15  +8 r14  +16 r13  +24 r12  +32 rbx  +40 rbp  +48 ret
 */

	.text

/* --------------------------------------------------------------- resume ---- */

	.globl	resume_stub_asm
	.type	resume_stub_asm, @function
resume_stub_asm:
	movq	%rsp, resume_sp(%rip)
	movq	%rbx, resume_rbx(%rip)
	movq	%rbp, resume_rbp(%rip)
	movq	%r12, resume_r12(%rip)
	movq	%r13, resume_r13(%rip)
	movq	%r14, resume_r14(%rip)
	movq	%r15, resume_r15(%rip)

	/* Hand to C. The task is identified out of band (the harness tracks
	 * `cur`), so there are no arguments to set up and no frame of ours to
	 * keep alive.
	 *
	 * The 8-byte adjustment is not cosmetic. The resume point is not a
	 * function entry: rsp here is frame+56, which is 8 mod 16 because the
	 * frame base is 16-byte aligned. `call` pushes 8 more, so the callee
	 * would enter with rsp 16-byte aligned rather than the 8 mod 16 the SysV
	 * ABI promises, and every frame offset the compiler computed from that
	 * assumption would be 8 bytes out — a fault inside check_first_resume
	 * with nothing to do with the context switch. Real resume points in
	 * this kernel are jump targets, not call targets, so they never have
	 * this problem; only this hand-off does. */
	subq	$8, %rsp
	call	task_body

	/* task_body() switches away rather than returning. If it ever does
	 * return, that is a harness bug; trap loudly rather than returning
	 * through a frame that no longer describes anything. */
	ud2
	.size	resume_stub_asm, .-resume_stub_asm

/* --------------------------------------------------------------- switch ---- */

/*
 * void switch_check_asm(u64 *save_rsp, u64 incoming_frame, u64 *incoming_slot)
 *
 * rdi, rsi and rdx are exactly context_switch()'s arguments plus the slot
 * holding the frame that was handed in, so the post-switch check has something
 * to compare rsp against.
 *
 * rdx is caller-saved and therefore not part of the context frame, which is
 * what makes this work: at the moment of the switch, the outgoing task's rdx
 * holds &incoming->handed, and the task that resumes is that same incoming
 * task. So the value in rdx after the resume names the frame we resumed into.
 */
	.globl	switch_check_asm
	.type	switch_check_asm, @function
switch_check_asm:
	pushq	%rbx
	call	context_switch
	/* First instruction after the call on a resume. Nothing above has run
	 * except context_switch itself. */
	movq	%rsp, %rax
	subq	$56, %rax
	cmpq	(%rdx), %rax
	jne	1f
	addq	$1, resume_offs_ok(%rip)
	popq	%rbx
	ret
1:
	addq	$1, resume_offs_bad(%rip)
	popq	%rbx
	ret
	.size	switch_check_asm, .-switch_check_asm

/* ----------------------------------------------------------------- data ---- */

	.bss
	.globl	resume_sp
	.globl	resume_rbx
	.globl	resume_rbp
	.globl	resume_r12
	.globl	resume_r13
	.globl	resume_r14
	.globl	resume_r15
	.globl	resume_offs_ok
	.globl	resume_offs_bad
	.align	8
resume_sp:
	.quad	0
resume_rbx:
	.quad	0
resume_rbp:
	.quad	0
resume_r12:
	.quad	0
resume_r13:
	.quad	0
resume_r14:
	.quad	0
resume_r15:
	.quad	0
resume_offs_ok:
	.quad	0
resume_offs_bad:
	.quad	0

	.section .note.GNU-stack,"",@progbits