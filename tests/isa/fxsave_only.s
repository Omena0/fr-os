/*
 * fxsave_only.s -- the kernel's two exemptions, and the neighbours of them
 * that are not exempt.
 *
 * context.S defines fpu_save/fpu_restore on fxsave and fxrstor and the
 * scheduler calls them on every task switch, so the kernel profile allows
 * exactly those two. Everything below the divider is x87 or a conversion or
 * a state-management instruction that is *not* on the list, and the gate has
 * to report it: allowing the whole x87 family to hide a compiler-emitted fld
 * would defeat the point of having a gate at all.
 */

	.text
	.globl exempt
exempt:
	/* allowed: state management, moves 512 bytes, interprets none of them */
	fxsave	(%rdi)
	fxrstor	(%rdi)

	/* --- everything below must be reported by the kernel profile --- */

	flds	(%rdi)
	fstps	(%rdi)
	fildl	(%rdi)
	fistpl	(%rdi)
	fldenv	(%rdi)
	fnstenv	(%rdi)
	fadds	(%rdi)
	fmul	%st(1)
	fchs
	fnstsw	%ax
	fxsave64	(%rdi)
	fxrstor64	(%rdi)
	ldmxcsr	(%rdi)
	stmxcsr	(%rdi)
	emms
	cvtsi2sd	%rax, %xmm0
	cvtsi2ss	%rax, %xmm0
	movaps	%xmm0, %xmm1
	ret
