/*
 * resume_stub.S — the resume point of the host harness.
 *
 * This is deliberately naked assembly. The point of the sentinels in the
 * harness is to check that a resumed task gets *its own* rbx/rbp/r12-r15 back,
 * but a compiled C function is entitled to overwrite those registers with its
 * own spill slots before running a single instruction, so a C trampoline can
 * only ever observe the frame contract and never the register contract.
 *
 * Capturing them at the exact resume point is the whole test. So: record every
 * register the moment control arrives, then hand over to C with no state of
 * our own that could have been clobbered.
 *
 * The saved frame is exactly what src/kernel/context.S restores:
 *
 *     +0  r15   +8  r14   +16 r13   +24 r12   +32 rbx   +40 rbp   +48 ret
 *
 * so on entry rsp is frame + 56.
 */

	.text
	.globl	resume_stub_asm
	.type	resume_stub_asm, @function

	.bss
	.globl	resume_sp
	.globl	resume_rbx
	.globl	resume_rbp
	.globl	resume_r12
	.globl	resume_r13
	.globl	resume_r14
	.globl	resume_r15
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
	.text

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
	 * keep alive. */
	call	task_resume

	/* task_resume() switches away rather than returning. If it ever does
	 * return, that is a harness bug; trap loudly instead of returning
	 * through a frame that no longer describes anything. */
	ud2
	.size	resume_stub_asm, .-resume_stub_asm

	.section .note.GNU-stack,"",@progbits