/*
 * syscall.S — raw syscall entry points.
 *
 * The kernel ABI is: rax = number, rdi/rsi/rdx/r10/r8/r9 = args, rax = return
 * (>=0 ok, negative is -errno). Only r10 carries the fourth argument because
 * rcx is clobbered by the `syscall` instruction itself (it holds the return
 * address). r11 is clobbered too, so the fifth and sixth arguments go in r8
 * and r9 untouched.
 *
 * The wrappers are grouped by argument count. __syscall0 is never reached —
 * it exists only so a 0-arg call site has a symbol to reference — and is
 * intentionally left reachable so the linker keeps it. The others return
 * normally and are deliberately NOT marked noreturn: a successful syscall
 * returns to its caller, and marking them noreturn would make the compiler
 * assume control flow never comes back, which is wrong.
 */
	.text
	.globl	__syscall0
	.globl	__syscall1
	.globl	__syscall2
	.globl	__syscall3
	.globl	__syscall4
	.globl	__syscall5
	.globl	__syscall6

	.type	__syscall0, @function
	.type	__syscall1, @function
	.type	__syscall2, @function
	.type	__syscall3, @function
	.type	__syscall4, @function
	.type	__syscall5, @function
	.type	__syscall6, @function

/* long __syscall0(long number) */
__syscall0:
	movq	%rdi, %rax
	syscall
	ret

/* long __syscall1(long number, long a1) */
__syscall1:
	movq	%rdi, %rax
	movq	%rsi, %rdi
	syscall
	ret

/* long __syscall2(long number, long a1, long a2) */
__syscall2:
	movq	%rdi, %rax
	movq	%rsi, %rdi
	movq	%rdx, %rsi
	syscall
	ret

/* long __syscall3(long number, long a1, long a2, long a3) */
__syscall3:
	movq	%rdi, %rax
	movq	%rsi, %rdi
	movq	%rdx, %rsi
	movq	%rcx, %rdx
	syscall
	ret

/* long __syscall4(long number, long a1, long a2, long a3, long a4) */
__syscall4:
	movq	%rdi, %rax
	movq	%rsi, %rdi
	movq	%rdx, %rsi
	movq	%rcx, %rdx
	movq	%r8,  %r10
	syscall
	ret

/* long __syscall5(long number, long a1, long a2, long a3, long a4, long a5) */
__syscall5:
	movq	%rdi, %rax
	movq	%rsi, %rdi
	movq	%rdx, %rsi
	movq	%rcx, %rdx
	movq	%r8,  %r10
	movq	%r9,  %r8
	syscall
	ret

/* long __syscall6(long number, long a1, long a2, long a3, long a4, long a5,
 *		long a6) */
__syscall6:
	movq	%rdi, %rax
	movq	%rsi, %rdi
	movq	%rdx, %rsi
	movq	%rcx, %rdx
	movq	%r8,  %r10
	movq	%r9,  %r8
	movq	8(%rsp), %r9
	syscall
	ret