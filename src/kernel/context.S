/*
 * context.S — context switch and FPU state primitives.
 *
 * Three jobs live here, all of them impossible or unsafe to express in C under
 * the kernel's build flags:
 *
 *   1. context_switch(), the callee-saved half of a switch. The caller-saved
 *      half is saved by whoever owns the larger frame (an interrupt stub or the
 *      syscall entry), which is why this function only touches rbp/rbx and
 *      r12-r15.
 *   2. fxsave64/fxrstor64. The kernel is compiled with -mno-sse -mno-80387, so
 *      the compiler would refuse to emit the instructions and would not know
 *      how to spill the state area. Written in assembly it is both correct and
 *      free of the compiler's assumptions.
 *   3. tsc_read(), which the scheduler uses for sub-tick accounting.
 *
 * Frame contract (repeated here because it is the one thing that has to be
 * exactly right on both sides):
 *
 *   context_switch(save_rsp, new_rsp) stores the post-push RSP at *save_rsp.
 *   That address is where a task's suspended frame begins, and it points at the
 *   r15 slot. From low to high the words are
 *
 *       +0   r15
 *       +8   r14
 *       +16  r13
 *       +24  r12
 *       +32  rbx
 *       +40  rbp
 *       +48  return address (consumed by `ret`)
 *
 *   so the saved return address is at offset 48 from the frame base, the frame
 *   is 56 bytes, and the frame base must be 16-byte aligned for the callee to
 *   see the 8 mod 16 the SysV ABI requires at function entry.
 */
    .text

/* void context_switch(u64 *save_rsp, u64 new_rsp) */
    .globl    context_switch
    .type    context_switch, @function
context_switch:
    /* Push order is the reverse of the pop order below, which leaves r15 at
     * the lowest address of the frame. */
    pushq    %rbp
    pushq    %rbx
    pushq    %r12
    pushq    %r13
    pushq    %r14
    pushq    %r15

    /* Publish the outgoing frame. */
    movq    %rsp, (%rdi)

    /* Adopt the incoming frame. Everything below this point runs on the new
     * task's kernel stack; nothing may touch the old one. */
    movq    %rsi, %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp

    /* Control transfers to the address the frame was built with. The kernel
     * is compiled with -fomit-frame-pointer, so the frame we are abandoning
     * has no rbp chain: the return address is the only link back. */
    ret
    .size    context_switch, .-context_switch

/* void context_restore(u64 rsp) — the restore half on its own. */
    .globl    context_restore
    .type    context_restore, @function
context_restore:
    movq    %rdi, %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp
    ret
    .size    context_restore, .-context_restore

/*
 * void fpu_save(void *state)
 *
 * FXSAVE writes the 512-byte x87 + SSE image and, on CPUs with SSE3, a
 * 16-byte LWP/MXCSR trailer. It never generates #NM, so it is safe to execute
 * regardless of CR0.TS — which matters because the whole point of this
 * function is to be called while TS is clear, at the instant a task that was
 * executing SSE is being switched out.
 */
    .globl    fpu_save
    .type    fpu_save, @function
fpu_save:
    /* RDI holds the destination. fxsave64 needs a plain memory operand, so
     * the pointer is forced out of the operand encoding with AT&T syntax. */
    fxsave    (%rdi)
    ret
    .size    fpu_save, .-fpu_save

/* void fpu_restore(const void *state) */
    .globl    fpu_restore
    .type    fpu_restore, @function
fpu_restore:
    fxrstor    (%rdi)
    ret
    .size    fpu_restore, .-fpu_restore

/*
 * u64 tsc_read(void)
 *
 * RDTSC is not serialising: a load that precedes it can still be in flight, so
 * the value read can be older than the instruction that requested it. LFENCE
 * before RDTSC is the cheapest way to keep the read from being hoisted above
 * earlier loads, which is the direction that matters for interval accounting.
 * Callers that need a globally ordered timestamp use cpu_serialize() around
 * this instead.
 */
    .globl    tsc_read
    .type    tsc_read, @function
tsc_read:
    lfence
    rdtsc
    /* RDTSC writes EDX:EAX. Shifting the high half down by 32 turns the pair
     * into the single 64-bit value the SysV return register pair would have
     * produced, so the caller can treat it like any other integer. */
    shlq    $32, %rdx
    orq    %rdx, %rax
    ret
    .size    tsc_read, .-tsc_read

/* Mark the stack non-executable. */
    .section .note.GNU-stack,"",@progbits
