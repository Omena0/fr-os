/*
 * sigreturn.S - the return address every signal handler is entered with.
 *
 * The kernel builds the handler frame so that when the handler returns, it
 * lands here with the saved context at the stack pointer. This is the only
 * place rt_sigreturn is issued from userland.
 */
    .text
    .globl    __restore_rt
    .type    __restore_rt, @function
__restore_rt:
    movq    $24, %rax
    syscall
    ud2
    .size    __restore_rt, .-__restore_rt

    .section .note.GNU-stack,"",@progbits
