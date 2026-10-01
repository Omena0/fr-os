/*
 * syscall_entry.S — ring 3 to ring 0 entry.
 *
 * At the SYSCALL instruction the CPU has put the user's RIP in RCX and the
 * user's RFLAGS in R11, loaded CS/SS from STAR, and cleared the RFLAGS bits
 * named in SFMASK. What it has *not* done is switch stacks: RSP is still the
 * user stack, and it is under the user's control.
 *
 * ---------------------------------------------------------------- register
 * pressure is the whole difficulty here. At a syscall boundary all sixteen
 * general-purpose registers hold live state:
 *
 *      rax rdi rsi rdx r10 r8 r9   user arguments (rax = syscall number)
 *      rcx r11                     the two SYSCALL clobbered (RIP, RFLAGS)
 *      rsp                         the user stack pointer
 *      rbx rbp r12 r13 r14 r15     callee-saved by the ABI, and preserved by
 *                                   libc's __syscallN callers, which are
 *                                   ordinary C functions as far as the
 *                                   compiler is concerned
 *
 * There is no scratch register. Anything this code needs before the frame
 * exists has to come out of memory.
 *
 * Two facts make that possible without ever touching a register:
 *
 *   1. TSS.RSP0 holds the incoming task's kernel stack top, and the scheduler
 *      writes it with the same value it writes to
 *      syscall_cpus[cpu].kstack_top (sched.c, both unconditional). Reading it
 *      is `movq kernel_tss+4(%rip), %rsp` — RIP-relative, so it needs no
 *      register. It has to be reached this way rather than through
 *      syscall_cpus[cpu]: computing that address needs RDMSR, and RDMSR
 *      destroys EAX, EDX and ECX, which are RAX (the syscall number), RDX
 *      (the third argument) and RCX (the user RIP).
 *   2. RCX and R11 are dead one instruction after they are first pushed — both
 *      values are already on the stack by then — so R11 becomes the one
 *      scratch register this code ever needs.
 *
 * ---------------------------------------------------------------- GS
 * There is deliberately no SWAPGS here.
 *
 * this_cpu() reads GS.base with RDMSR on IA32_GS_BASE, which is the *hidden*
 * base. That is correct for as long as nothing exchanges the hidden base with
 * IA32_KERNEL_GS_BASE, and SWAPGS is exactly the instruction that does. The
 * invariant a SWAPGS-based entry relies on — that both MSRs hold the same
 * value, so the swap exchanges a value with itself — is not established
 * anywhere in this tree: kmain.S's vendor check writes 0xC0000102 only after
 * a comparison that can never succeed, so IA32_KERNEL_GS_BASE is still zero.
 * A SWAPGS here would therefore leave ring-0 code with a visible GS base of
 * zero for the duration of the syscall.
 *
 * Reading the hidden base directly is also what makes the entry immune to
 * ring 3: %gs-relative addressing uses the *visible* base, which CPL 3 can
 * set (CR4.FSGSBASE is set by the loader), so anything reached through a GS
 * memory operand in ring 0 is attacker-influenceable. IA32_GS_BASE has no such
 * problem, because only the kernel writes it.
 *
 * The matching return path is ret_to_user (interrupt_entry.S), which also does
 * not SWAPGS. The two must agree: if the iretq path ever starts swapping, this
 * file has to start swapping in the same place, and only once IA32_GS_BASE and
 * IA32_KERNEL_GS_BASE are known to hold the same value.
 *
 * Frame layout on the kernel stack, from the bottom up (&regs is the
 * struct syscall_regs the C dispatcher is handed):
 *
 *     [rax][rdi][rsi][rdx][r10][r8][r9][rcx][r11][rip][rflags][rsp]
 *     [padding]
 *     [rbp][rbx][r12][r13][r14][r15]
 *     [user rip][user rflags]
 *     [padding]
 *
 * 176 bytes, a multiple of 16, so RSP is 16-byte aligned at the `call` as the
 * SysV ABI requires — provided the kernel stack top is 16-byte aligned, which
 * it is: it is a page-aligned vmalloc result plus a page-multiple size.
 *
 * The pushes therefore run backwards through the field list. RCX and R11 each
 * appear twice: once in the register frame for the C code to read, and once at
 * the top for SYSRET, which takes RIP from RCX and RFLAGS from R11. SYSRET
 * does not load RSP at all in 64-bit mode, so the user RSP is restored
 * explicitly at the end.
 */

/* Must match SYSCALL_CPU_SIZE and the field offsets in syscall.h. */
#define SYSCALL_CPU_SIZE  64
#define SC_KSTACK_TOP     0
#define SC_RET_TO_USER    16

/* IA32_GS_BASE. The hidden GS base; there is no RDGSBASE on Intel. */
#define MSR_GS_BASE       0xC0000101

/* sizeof(struct syscall_regs) plus the alignment padding above it, i.e. the
 * distance from &regs to the first register the return path pops. */
#define REG_FRAME_BYTES   104

/* Total frame size, &regs to the topmost padding slot. */
#define FRAME_BYTES       176

/* offsetof(struct syscall_regs, rsp) — where the user RSP is parked. */
#define REG_RSP_OFF       88

/* After REG_FRAME_BYTES and the eight pops, RSP sits one padding slot below
 * the top of the frame, so the user RSP is this far below it. */
#define USER_RSP_DISP  (-(FRAME_BYTES - 8 - REG_RSP_OFF))     /* -80 */

/* TSS.RSP0. Architected offset 4; the CPU reads it as a full 64 bits in long
 * mode, and gdt.c asserts the offset so the two cannot drift. */
#define TSS_RSP0_OFF      4

	.text

	.globl	syscall_entry
	.type	syscall_entry, @function
syscall_entry:
	/*
	 * Nothing below may modify a general-purpose register until the callee-
	 * saved six are on the stack. Both instructions are RIP-relative and
	 * therefore clobber nothing.
	 *
	 * The parked user RSP is only needed eleven instructions later, once
	 * R11 is dead. Nothing can observe the scratch in between: a syscall
	 * can only be entered from CPL 3, and SFMASK cleared IF on entry, so
	 * neither an interrupt nor a nested syscall can run here.
	 */
	movq	%rsp, syscall_user_rsp_scratch(%rip)
	movq	kernel_tss+TSS_RSP0_OFF(%rip), %rsp

	/* The return path's half of the frame, pushed from the top down. */
	pushq	$0			/* [padding] */
	pushq	%r11			/* user RFLAGS */
	pushq	%rcx			/* user RIP */
	pushq	%rbp
	pushq	%rbx
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15
	pushq	$0			/* padding */

	/*
	 * R11's value has been in the frame since the third instruction above,
	 * so it is the one register available for scratch from here on. RCX is
	 * in the same position and is reused the same way.
	 */
	movq	syscall_user_rsp_scratch(%rip), %r11

	/*
	 * struct syscall_regs, pushed in reverse field order so that the
	 * pushes below are the highest addresses and &regs is the lowest. Every
	 * register pushed here still holds the value the syscall instruction
	 * saw: the entry code has not written to RAX, RDI, RSI, RDX, R10, R8,
	 * R9, RCX or R11 since the CPU loaded them.
	 *
	 * regs->r11 and regs->rflags are both the user's RFLAGS, and regs->rip
	 * and regs->rcx are both the user's RIP: SYSCALL destroyed the user's
	 * own RCX and R11, so those fields record what the instruction put
	 * there rather than what the user had.
	 */
	pushq	%r11			/* rsp    */
	pushq	%r11			/* rflags */
	pushq	%rcx			/* rip    */
	pushq	%r11			/* r11    */
	pushq	%rcx			/* rcx    */
	pushq	%r9
	pushq	%r8
	pushq	%r10
	pushq	%rdx
	pushq	%rsi
	pushq	%rdi
	pushq	%rax			/* lowest address == &regs */

	/* RSP is 16-byte aligned here, as the ABI requires at a call site. */
	movq	%rsp, %rdi
	call	syscall_dispatch

	/*
	 * Return path. Everything that still needs the kernel — reading the
	 * return value out of the frame, and asking whether this frame may be
	 * returned to at all — happens while RSP is still the kernel stack.
	 * Adopting the user RSP is the last thing done, immediately before
	 * SYSRETQ, so no ring-0 instruction ever executes on memory the
	 * process owns.
	 *
	 * &syscall_regs is at (%rsp); the return value is its first field.
	 */
	movq	(%rsp), %rax		/* RAX = syscall_dispatch's return */

	/*
	 * This CPU's slot in syscall_cpus, resolved the same way this_cpu()
	 * resolves the per-CPU area: RDMSR of the hidden GS base, whose offset
	 * zero is percpu_data[cpu].cpu_id.
	 *
	 * RDMSR destroys EAX, EDX and ECX. Nothing is live at that point — the
	 * frame holds every user value and the callee-saved registers are
	 * restored by the pops below — and RAX in particular is reloaded from
	 * the frame afterwards, not before.
	 */
	rdmsr
	movq	%rax, %rbp		/* low 32 bits of GS.base */
	shlq	$32, %rdx		/* high 32 bits. WRMSR takes EDX:EAX, so
					   the halves are the other way round from
					   what a plain movq %rax, %rdx would
					   give — this is the mistake that made
					   two GS bases disagree all day. */
	orq	%rdx, %rbp		/* &percpu_data[cpu] */
	movl	(%rbp), %ebp		/* cpu_id; a 32-bit store zero-extends */
	shlq	$6, %rbp		/* * sizeof(struct syscall_cpu) */
	leaq	syscall_cpus(%rip), %rdx
	addq	%rdx, %rbp		/* &syscall_cpus[cpu_id] */

	/*
	 * A handler that does not return (exit, exit_group) cleared this flag
	 * before switching away. The task's frame is abandoned, so control here
	 * means something went wrong that no other check caught; it is
	 * reported rather than turned into a silent hang.
	 */
	cmpl	$0, SC_RET_TO_USER(%rbp)
	je	.Lsyscall_lost

	addq	$REG_FRAME_BYTES, %rsp

	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp
	popq	%rcx			/* user RIP, which SYSRETQ takes from RCX */
	popq	%r11			/* user RFLAGS, which SYSRETQ takes from R11 */

	/*
	 * SYSRETQ in 64-bit mode does not load RSP: the ring-3 stack pointer
	 * is whatever RSP holds when the instruction retires. The user RSP
	 * was parked in the register frame at the top of this path, so put it
	 * back now — after every kernel access, and immediately before the
	 * instruction that leaves.
	 */
	movq	USER_RSP_DISP(%rsp), %rsp

	sysretq

.Lsyscall_lost:
	/* Rebuild a usable kernel stack for the panic handler. %rbp still holds
	 * this CPU's syscall_cpus slot. */
	movq	SC_KSTACK_TOP(%rbp), %rsp
	call	syscall_lost_frame
1:	jmp	1b
	.size	syscall_entry, .-syscall_entry

	.section .note.GNU-stack,"",@progbits

/*
 * Scratch for the user RSP, which has to survive from the first instruction to
 * the eleventh. It is deliberately a private word in this file rather than
 * syscall_cpus[cpu].user_rsp: that field lives in the per-CPU array, whose
 * index is the very thing that cannot be computed until after the stack
 * switch.
 */
	.section .bss
	.align 8
	.local	syscall_user_rsp_scratch
	.type	syscall_user_rsp_scratch, @object
	.size	syscall_user_rsp_scratch, 8
syscall_user_rsp_scratch:
	.zero	8
	.section .text