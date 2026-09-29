# Stack Canaries

## Overview

Stack canaries are a mitigation against stack buffer overflow attacks that overwrite the saved return address. A random value (the "canary") is placed between local variables and the saved return address. Before the function returns, the canary value is checked — if it has been modified (by an overflow), the program is terminated immediately.

## Compiler-Inserted Canaries

The compiler (`-fstack-protector-strong` flag, which is the default for all kernel and userspace builds) automatically instruments functions that have:

- Local arrays or variable-length arrays.
- Calls to `alloca`.
- Addresses of local variables taken.

Generated code (pseudo-assembly):

```asm
function_prologue:
    mov rax, [gs:CANARY_OFFSET]   ; load per-thread canary from TLS
    xor rax, rsp                  ; mix with stack pointer for additional entropy
    push rax                      ; save canary to stack
    ; ... function body ...
function_epilogue:
    pop rax                       ; restore canary from stack
    xor rax, rsp
    cmp rax, [gs:CANARY_OFFSET]   ; compare with expected value
    jne __stack_chk_fail          ; if mismatch: terminate
    ret
```

## Per-Thread Canary

Each thread has an independent canary value stored in the Thread-Local Storage (TLS), accessed via the GS segment (for userspace, via the `fsbase`/`gsbase` MSRs set at thread creation time).

The canary is generated at thread creation:

```c
void thread_init_canary(struct thread *t) {
    t->tls.stack_canary = csprng_next64();
    t->tls.stack_canary &= ~0xFF;  // low byte always 0 (stops strlen-style attacks)
}
```

The low byte is zeroed to prevent string-based overflows (which stop at `\0`) from forging the canary.

## Kernel Stack Canaries

The kernel itself uses stack canaries for kernel stack frames. The kernel canary is per-CPU (stored in the per-CPU data structure accessed via GS in kernel mode):

```c
void cpu_init_canary(struct percpu *cpu) {
    cpu->stack_canary = csprng_next64() & ~0xFF;
}
```

On canary mismatch in kernel context: `kpanic("kernel stack smashed")`.

## `__stack_chk_fail`

The userspace implementation of `__stack_chk_fail`:

```c
__attribute__((noreturn)) void __stack_chk_fail(void) {
    // 1. Write a message to stderr (async-signal-safe write syscall)
    static const char msg[] = "stack smashing detected\n";
    write(2, msg, sizeof(msg) - 1);
    // 2. Abort: send SIGABRT to self
    kill(getpid(), SIGABRT);
    _exit(127);  // fallback if signal is caught
}
```

## Guard Pages

Stack guard pages are non-accessible pages placed below the thread stack (at the lowest address of the stack region). If the stack overflows beyond the canary (very large overflow), a page fault occurs on the guard page, which triggers an `SIGSEGV` rather than silently overwriting adjacent data.

Stack guard page size: one page (4 KB). Mapped with `PROT_NONE`.

## Related Documents

- [aslr.md](aslr.md)
- [nx-enforcement.md](nx-enforcement.md)
- [hardened-allocator.md](hardened-allocator.md)
- [memory/virtual-memory.md](../memory/virtual-memory.md)
