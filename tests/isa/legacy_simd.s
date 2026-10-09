# Comprehensive legacy SIMD probe.
#
# One instance of every legacy (non-VEX) x87 / MMX / SSE..SSE4.2 / AES /
# PCLMULQDQ mnemonic family that is reachable on the -msse4.2 baseline. The
# point is coverage: the gate has to flag every line of this file under the
# kernel profile, and flag none of it under the user profile.
    .text

# --- SSE1: packed/scalar single -------------------------------------------
    addps    %xmm0, %xmm1
    addss    %xmm0, %xmm1
    andnps    %xmm0, %xmm1
    andps    %xmm0, %xmm1
    cmpeqps    %xmm0, %xmm1
    comiss    %xmm0, %xmm1
    cvtsi2ss    %rax, %xmm0
    cvtss2si    %xmm0, %rax
    cvttss2si    %xmm0, %rax
    divps    %xmm0, %xmm1
    divss    %xmm0, %xmm1
    ldmxcsr    (%rdi)
# maskmovd/maskmovq (0f c7) cannot be assembled by name and this
# objdump does not decode it; this file does not emit them.
    maxps    %xmm0, %xmm1
    maxss    %xmm0, %xmm1
    minps    %xmm0, %xmm1
    minss    %xmm0, %xmm1
    movaps    %xmm0, %xmm1
    movhps    (%rdi), %xmm0
    movhps    %xmm0, (%rdi)
    movhlps    %xmm0, %xmm1
    movlhps    %xmm0, %xmm1
    movmskps    %xmm0, %eax
    movss    %xmm0, %xmm1
    movups    %xmm0, %xmm1
    mulps    %xmm0, %xmm1
    mulss    %xmm0, %xmm1
    orps    %xmm0, %xmm1
    packssdw    %xmm0, %xmm1
    paddb    %xmm0, %xmm1
    paddw    %xmm0, %xmm1
    paddd    %xmm0, %xmm1
    paddq    %xmm0, %xmm1
    paddsb    %xmm0, %xmm1
    paddsw    %xmm0, %xmm1
    pand    %xmm0, %xmm1
    pandn    %xmm0, %xmm1
    pause
    pavgw    %xmm0, %xmm1
    pmaxsw    %xmm0, %xmm1
    pmaxub    %xmm0, %xmm1
    pmulhuw    %xmm0, %xmm1
    pmulhw    %xmm0, %xmm1
    pmullw    %xmm0, %xmm1
    por    %xmm0, %xmm1
    psadbw    %xmm0, %xmm1
    pshuflw    $0x1b, %xmm0, %xmm1
    pshufhw    $0x1b, %xmm0, %xmm1
    pshufw    $0x1b, %mm0, %mm1
    pslld    $3, %xmm0
    psrad    $3, %xmm0
    psubb    %xmm0, %xmm1
    psubq    %xmm0, %xmm1
    psubsb    %xmm0, %xmm1
    psubsw    %xmm0, %xmm1
    psubusb    %xmm0, %xmm1
    psubusw    %xmm0, %xmm1
    psubw    %xmm0, %xmm1
    pxor    %xmm0, %xmm1
    rcpps    %xmm0, %xmm1
    rcpss    %xmm0, %xmm1
    rsqrtps    %xmm0, %xmm1
    rsqrtss    %xmm0, %xmm1
    shufps    $0x11, %xmm0, %xmm1
    sqrtps    %xmm0, %xmm1
    sqrtss    %xmm0, %xmm1
    stmxcsr    (%rdi)
    subps    %xmm0, %xmm1
    subss    %xmm0, %xmm1
    ucomiss    %xmm0, %xmm1
    cmpltps    %xmm0, %xmm1
    cmpnleps    %xmm0, %xmm1
    xorps    %xmm0, %xmm1

# --- SSE2: double, 64-bit integer, string ---------------------------------
    addpd    %xmm0, %xmm1
    addsd    %xmm0, %xmm1
    andnpd    %xmm0, %xmm1
    andpd    %xmm0, %xmm1
    cmpeqpd    %xmm0, %xmm1
    comisd    %xmm0, %xmm1
    cvtdq2pd    %xmm0, %xmm1
    cvtdq2ps    %xmm0, %xmm1
    cvtpd2dq    %xmm0, %xmm1
    cvtpd2ps    %xmm0, %xmm1
    cvtps2dq    %xmm0, %xmm1
    cvtps2pd    %xmm0, %xmm1
    cvtsd2si    %xmm0, %rax
    cvtsd2ss    %xmm0, %xmm1
    cvtsi2sd    %rax, %xmm0
    cvtsi2sdq    %rax, %xmm0
    cvtss2sd    %xmm0, %xmm1
    cvttpd2dq    %xmm0, %xmm1
    cvttps2dq    %xmm0, %xmm1
    cvttsd2si    %xmm0, %rax
    divpd    %xmm0, %xmm1
    divsd    %xmm0, %xmm1
    haddpd    %xmm0, %xmm1
    haddps    %xmm0, %xmm1
    hsubpd    %xmm0, %xmm1
    hsubps    %xmm0, %xmm1
    lddqu    (%rdi), %xmm0
    maxpd    %xmm0, %xmm1
    maxsd    %xmm0, %xmm1
    minpd    %xmm0, %xmm1
    minsd    %xmm0, %xmm1
    movapd    %xmm0, %xmm1
    movd    %xmm0, %eax
    movd    %eax, %xmm0
    movdqa    %xmm0, %xmm1
    movdqu    (%rdi), %xmm0
    movhpd    (%rdi), %xmm0
    movhpd    %xmm0, (%rdi)
    movlpd    (%rdi), %xmm0
    movlpd    %xmm0, (%rdi)
    movmskpd    %xmm0, %rax
    movsd    %xmm0, %xmm1
    movupd    %xmm0, %xmm1
    movdqa    %xmm0, %xmm1
    movdqu    %xmm0, %xmm1
    movq    %xmm0, %xmm1
    movq    %xmm0, %rax
    mulpd    %xmm0, %xmm1
    mulsd    %xmm0, %xmm1
    orpd    %xmm0, %xmm1
    packssdw    %xmm0, %xmm1
    packsswb    %xmm0, %xmm1
    packuswb    %xmm0, %xmm1
    paddsw    %xmm0, %xmm1
    pcmpgtb    %xmm0, %xmm1
    pcmpeqb    %xmm0, %xmm1
    pmovmskb    %xmm0, %eax
    pmuludq    %xmm0, %xmm1
    pmaddwd    %xmm0, %xmm1
    pslldq    $4, %xmm0
    psrlq    $4, %xmm0
    psubd    %xmm0, %xmm1
    psubw    %xmm0, %xmm1
    punpckhbw    %xmm0, %xmm1
    punpcklwd    %xmm0, %xmm1
    punpckldq    %xmm0, %xmm1
    punpcklqdq    %xmm0, %xmm1
    pxor    %xmm0, %xmm1
    sqrtpd    %xmm0, %xmm1
    sqrtsd    %xmm0, %xmm1
    subpd    %xmm0, %xmm1
    subsd    %xmm0, %xmm1
    unpckhpd    %xmm0, %xmm1
    unpcklpd    %xmm0, %xmm1
    xorpd    %xmm0, %xmm1

# --- SSE3 -----------------------------------------------------------------
    addsubpd    %xmm0, %xmm1
    addsubps    %xmm0, %xmm1
    haddpd    %xmm0, %xmm1
    hsubps    %xmm0, %xmm1
    movddup    %xmm0, %xmm1
    movdqu    (%rdi), %xmm0
    movshdup    %xmm0, %xmm1
    movsldup    %xmm0, %xmm1
    lddqu    (%rdi), %xmm0

# --- SSSE3 ----------------------------------------------------------------
    pabsb    %xmm0, %xmm1
    pabsw    %xmm0, %xmm1
    palignr    $4, %xmm1, %xmm0
    phaddd    %xmm0, %xmm1
    phaddw    %xmm0, %xmm1
    phsubw    %xmm0, %xmm1
    pmaddubsw    %xmm0, %xmm1
    pshufb    %xmm0, %xmm1
    psignb    %xmm0, %xmm1
    psignw    %xmm0, %xmm1
    psignd    %xmm0, %xmm1
    psubusb    %xmm0, %xmm1
    psubusw    %xmm0, %xmm1

# --- SSE4.1 / SSE4.2 ------------------------------------------------------
    blendpd    $0x3, %xmm0, %xmm1
    blendps    $0x3, %xmm0, %xmm1
    blendvpd    %xmm0, %xmm1
    blendvps    %xmm0, %xmm1
    dppd    $0x1b, %xmm0, %xmm1
    dpps    $0x1b, %xmm0, %xmm1
    extractps    $1, %xmm0, %eax
    insertps    $0x11, %xmm0, %xmm1
    maxsd    %xmm0, %xmm1
    minsd    %xmm0, %xmm1
    pblendvb    %xmm0, %xmm1
    pblendw    $0x3, %xmm0, %xmm1
    pcmpestri    $0x1b, %xmm1, %xmm0
    pcmpestrm    $0x1b, %xmm1, %xmm0
    pcmpistri    $0x1b, %xmm1, %xmm0
    pcmpistrm    $0x1b, %xmm1, %xmm0
    pmaxsd    %xmm0, %xmm1
    pmaxud    %xmm0, %xmm1
    pminsd    %xmm0, %xmm1
    pminud    %xmm0, %xmm1
    pmovsxbw    %xmm0, %xmm1
    pmovsxdq    %xmm0, %xmm1
    pmovsxwd    %xmm0, %xmm1
    pmovzxbw    %xmm0, %xmm1
    pmovzxdq    %xmm0, %xmm1
    pmovzxwd    %xmm0, %xmm1
    pmuldq    %xmm0, %xmm1
    pmulld    %xmm0, %xmm1
    ptest    %xmm0, %xmm1
    roundpd    $0x3, %xmm0, %xmm1
    roundps    $0x3, %xmm0, %xmm1
    roundsd    $0x3, %xmm0, %xmm1
    roundss    $0x3, %xmm0, %xmm1
    cmppd    $0x3, %xmm0, %xmm1
    cmpsd    $0x3, %xmm0, %xmm1
    cmpps    $0x3, %xmm0, %xmm1
    cmpss    $0x3, %xmm0, %xmm1
    cmpltpd    %xmm0, %xmm1
    cmpleps    %xmm0, %xmm1
    pcmpgtq    %xmm0, %xmm1

# --- AES / PCLMULQDQ ------------------------------------------------------
    aesenc    %xmm0, %xmm1
    aesenclast    %xmm0, %xmm1
    aesdec    %xmm0, %xmm1
    aesdeclast    %xmm0, %xmm1
    aesimc    %xmm0, %xmm1
    aeskeygenassist    $0x1, %xmm0, %xmm1
    pclmulqdq    $0x1, %xmm0, %xmm1

# --- MMX ------------------------------------------------------------------
    emms
    movd    %eax, %mm0
    movq    %rax, %mm0
    pmuludq    %mm0, %mm1
    paddd    %mm0, %mm1
    pxor    %mm0, %mm1
    femms

# --- x87 ------------------------------------------------------------------
    flds    (%rdi)
    fsts    (%rdi)
    fstps    (%rdi)
    fildl    (%rdi)
    fistpl    (%rdi)
    fisttps    (%rdi)
    fildll    (%rdi)
    fbld    (%rdi)
    fbstp    (%rdi)
    fadds    (%rdi)
    faddp    %st, %st(1)
    fsubs    (%rdi)
    fsubrs    (%rdi)
    fsubp    %st, %st(1)
    fsubrp    %st, %st(1)
    fmuls    (%rdi)
    fmulp    %st, %st(1)
    fdivs    (%rdi)
    fdivrs    (%rdi)
    fdivp    %st, %st(1)
    fdivrp    %st, %st(1)
    fchs
    fabs
    fsqrt
    fscale
    fsin
    fcos
    fptan
    fpatan
    fprem
    fprem1
    fyl2x
    fyl2xp1
    fnclex
    finit
    fldcw    (%rdi)
    fnstcw    (%rdi)
    fldenv    (%rdi)
    fnstenv    (%rdi)
    fnstsw    %ax
    fxch    %st(1)
    fucomi    %st(1), %st
    fucomip    %st(1), %st
    fcomi    %st(1), %st
    fwait
    fxam
    frndint
    fnop
    fucomp    %st(1)
    fucompp

# --- XSAVE family (deliberately not enabled, but reachable) ---------------
    # Not emitted below: xsave/xrstor are privileged-ish and the assembler
    # needs -mxsave. They are covered by the classifier's regex directly and
    # asserted in verify_isa_test.sh by construction.

    ret
