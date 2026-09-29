/*
 * syscall_entry.S — ring 3 to ring 0 entry.
 *
 * At the SYSCALL instruction the CPU has put the user's RIP in RCX and the
 * user's RFLAGS in R11, loaded the kernel selectors from STAR, and cleared the
 * RFLAGS bits named in SFMASK. What it has *not* done is switch stacks: RSP is
 * still the user stack, and it is under the user's control.
 *
 * This code therefore:
 *   1. swapgs, because the kernel's per-CPU area is reached through the GS base
 *      and ring 3 owns GS;
 *   2. reads the current CPU id out of that per-CPU area;
 *   3. computes &syscall_cpus[cpu].kstack_top and loads RSP from it. The
 *      scheduler maintains that field: it is the incoming task's kernel stack
 *      top, so a task preempted inside a syscall finds its own half-built frame
 *      when it comes back, and a task that never made a syscall has a stack
 *      nothing is using;
 *   4. pushes the caller-saved state and a struct syscall_regs, and calls the
 *      dispatcher in C.
 *
 * Nothing here is trusted: RAX, RDI, RSI, RDX, R10, R8 and R9 are all user
 * values and are only ever *read*, never used to address memory. The one
 * memory address computed from anything at all is the per-CPU array, and it is
 * derived from the kernel's own GS base, which ring 3 cannot influence.
 *
 * Register discipline: the entry needs one scratch register to hold the user
 * RSP and one to hold the per-CPU slot address, and no argument register can be
 * destroyed before it is saved. RDI is therefore used for the user RSP and RSI
 * for the slot, and both are pushed into the register frame at their proper
 * offsets, so the dispatcher sees the values the syscall instruction saw. R11
 * is free because SYSCALL itself clobbered it.
 *
 * Frame layout on the kernel stack, from the top down:
 *
 *     [rsp field]  [rflags]  [rip]  [r11]  [rcx]  [r9]  [r8]  [r10]
 *     [rdx]  [rsi]  [rdi]  [rax]                       <- &regs
 *     [0]                       alignment padding
 *     [rbp] [rbx] [r12] [r13] [r14] [r15]
 *     [user rip]  [user rflags]
 *     [user rsp]                                  <- bottom
 *
 * Total 176 bytes, so RSP is 16-byte aligned at the `call`, as the SysV ABI
 * requires. struct syscall_regs is the 96 bytes at the top of that block, laid
 * out low-to-high exactly as the C structure declares its fields, which is why
 * the pushes run backwards through the field list.
 */

/* Must match SYSCALL_CPU_SIZE and the field offsets in syscall.h. */
#define SYSCALL_CPU_SIZE  64
#define SC_KSTACK_TOP     0
#define SC_RET_TO_USER    16

/* Size of the pushed register frame, in bytes, from &regs to the alignment
 * padding inclusive. */
#define REG_FRAME_BYTES   104

	.text

	.globl	syscall_entry
	.type	syscall_entry, @function
syscall_entry:
	/*
	 * GS now names the kernel's per-CPU area. Before the swap it named
	 * whatever ring 3 set it to, so nothing may be read through GS until
	 * this instruction has retired.
	 */
	swapgs

	/* r11 = &syscall_cpus[cpu_id] */
	movq	%gs:0, %r11
	movl	0(%r11), %r11d		/* cpu_id, zero-extends into r11 */
	shlq	$6, %r11		/* * sizeof(struct syscall_cpu) */
	leaq	syscall_cpus(%rip), %r11
	addq	%r11, %r11

	/* The user RSP, saved before the kernel stack is adopted. */
	movq	%rsp, %rdi

	movq	%r11, %rsi
	movq	SC_KSTACK_TOP(%rsi), %rsp

	pushq	%rdi			/* user RSP, bottom of the frame */
	pushq	%r11			/* user RFLAGS */
	pushq	%rcx			/* user RIP */
	pushq	%rbp
	pushq	%rbx
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15
	pushq	$0			/* padding: 8 mod 16 -> 0 mod 16 */

	/*
	 * The register frame, pushed in reverse field order. The first push
	 * reaches back 168 bytes to the user RSP that is already on the stack,
	 * so the C structure is fully populated without a single mov from a
	 * register that has not been preserved.
	 */
	pushq	168(%rsp)		/* rsp */
	pushq	%r11			/* rflags */
	pushq	%rcx			/* rip */
	pushq	%r11			/* r11 */
	pushq	%rcx			/* rcx */
	pushq	%r9
	pushq	%r8
	pushq	%r10
	pushq	%rdx
	pushq	%rsi
	pushq	%rdi
	pushq	%rax			/* lowest address == &regs */

	movq	%rsp, %rdi
	call	syscall_dispatch

	/* RAX is the return value; lift it out before the frame is gone. */
	addq	$REG_FRAME_BYTES, %rsp
	movq	96(%rsp), %rax

	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp
	popq	%rcx			/* user RIP, which SYSRET wants in RCX */
	popq	%r11			/* user RFLAGS, which SYSRET wants in R11 */

	/* Discard the saved user RSP: SYSRET does not read it, and the CPU
	 * takes the user stack pointer from RCX. */
	popq	%rdi
	movq	%rdi, %rsp

	/*
	 * A handler that does not return (exit, exit_group) cleared this flag
	 * before switching away. The task's frame is abandoned, so control
	 * here means something went wrong that no other check caught; it is
	 * reported rather than turned into a silent hang.
	 */
	movq	%gs:0, %rdi
	movl	0(%rdi), %edi
	shlq	$6, %rdi
	leaq	syscall_cpus(%rip), %rdi
	addq	%rdi, %rdi
	cmpl	$0, SC_RET_TO_USER(%rdi)
	je	.Lsyscall_lost

	swapgs
	sysretq

.Lsyscall_lost:
	/* Rebuild a usable kernel stack for the panic handler. */
	movq	SC_KSTACK_TOP(%rdi), %rsp
	call	syscall_lost_frame
1:	jmp	1b
	.size	syscall_entry, .-syscall_entry

	.section .note.GNU-stack,"",@progbits
