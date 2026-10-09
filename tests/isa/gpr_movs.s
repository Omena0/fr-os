/*
 * gpr_movs.s -- the instructions whose *spelling* is shared between a
 * general-purpose move and a vector one, plus the control-register reads the
 * kernel legitimately executes.
 *
 * The gate must report nothing here. It is the false-positive half of the
 * test: a check that flags a GPR image stops the build for no reason, and
 * that is the failure mode which makes people delete a gate.
 *
 * movsd and movsq and friends below are the *string* forms. objdump prints
 * the string "move string qword" and the SSE2 "move quadword" both as movq,
 * and the string "move string dword" and the SSE2 "move scalar double" both
 * as movsd. Nothing distinguishes them at the mnemonic level; the operands
 * do, because every SSE form names an %xmm and no string form does.
 *
 * xgetbv is here for the same reason. It starts with "x" and reads a control
 * register, and kmain.S and cpu_features.h both execute it.
 */

    .text
    .globl gpr_movs
gpr_movs:
    /* string moves, implicit operands only.
     *
     * MOVSB/MOVSW/MOVSL/MOVSQ are always %ds:(%rsi) -> %es:(%rdi); the
     * assembler rejects an explicit operand on them, and that is precisely why
     * the implicit form is what a real object contains. objdump prints them as
     * movsb/movsw/movsl/movsq, and movsd/movsq are also the spellings of SSE
     * scalar-double and quadword moves. Nothing at the mnemonic level tells
     * them apart; the operands do, because every SSE form names an %xmm and no
     * string form does. The classifier must therefore look at operands, which
     * is what this object is for. */
    movsb
    movsw
    movsl
    movsq
    /* movq: the same text as the string quadword move above, and the 64-bit
     * immediate form below */
    movq    $0, 0x8(%rsp)
    movq    $0x1234, 0x10(%rsp)
    movq    %rax, 0x18(%rsp)
    /* No `movd` here on purpose: in 64-bit mode MOVD is an SSE move whose
     * source is an %xmm, so there is no GPR form to write and the
     * mnemonic can only appear if real vector code was generated. That is
     * what isa_probe.c is for; this object only covers spellings a
     * GPR-using object can actually contain. */
    movl    %eax, 0x24(%rsp)
    movb    $0, 0x28(%rsp)
    /* BMI2, which is GPR-only and starts with p */
    pdep    %rax, %rbx, %rcx
    pext    %rax, %rbx, %rcx
    /* memory ordering and prefetch: hints, not writes */
    mfence
    sfence
    lfence
    prefetcht0    (%rax)
    prefetchw    (%rax)
    /* control registers the kernel already uses */
    xgetbv
    /* BMI1/BMI2/LZCNT that KERNEL_CFLAGS enables on purpose */
    bextr    %rax, %rbx, %rcx
    bzhi    %rax, %rbx, %rcx
    andn    %rax, %rbx, %rcx
    blsr    %rax, %rbx        /* no one-operand form */
    blsi    %rax, %rbx
    tzcnt    %rax, %rbx
    lzcnt    %rax, %rbx
    popcnt    %rax, %rbx
    adcx    %rax, %rbx
    adox    %rax, %rbx
    rdrand    %rax
    clwb    (%rax)
    clflushopt    (%rax)
    ret
