# Context Switching

## Scope

A context switch saves the full execution state of the current thread and restores the state of the next thread. Context switches occur:

- On a scheduler tick (preemptive switch).
- When a thread blocks (voluntary switch via mutex, IPC wait, I/O wait).
- On `sched_yield()`.
- After interrupt return when a higher-priority thread is runnable.

## Thread State

The complete thread state consists of:

### General-Purpose Registers

Saved on the kernel stack in a `struct context` layout:

```c
struct context {
    // Callee-saved (ABI: must preserve across calls)
    uint64_t rbx, rbp, r12, r13, r14, r15;
    // Caller-saved (saved on interrupt/syscall entry)
    uint64_t rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11;
    // Control
    uint64_t rip, cs, rflags, rsp, ss;
};
```

### SIMD / FPU State

**The lazy `TS`-based scheme described in earlier versions of this page is not
what the code does.** `context.S` saves and restores the FPU state eagerly on
every switch, with `fxsave`/`fxrstor`, into a per-task `fpu_state` buffer. `TS`
is never set to trigger a #NM, and there is no FPU exception handler.

The buffer is `FXSAVE`-shaped: 512 bytes covering x87 and XMM0-15. `fxsave` does
**not** save bits 128-255 of YMM0-15, and userspace is still compiled with
`-mavx2 -mfma -mf16c -mxsave` (`USER_CFLAGS` in `src/config.mk`). So two
preempted userspace processes have the upper halves of their YMM registers
silently clobbered across a switch: no fault, no diagnostic, just corrupted
vector spills and results. The kernel itself is built GPR-only
(`-mno-avx -mno-avx2 -mno-fma -mno-f16c`), which is why this does not bite the
kernel.

Either drop the AVX flags from `USER_CFLAGS`, or move to `xsave`/`xrstor` with an
`XSAVE` header and a per-task XCR0. See finding #63 in
[`../../../MEGA_AUDIT.md`](../../../MEGA_AUDIT.md).

### Scheduling Metadata

Saved in the thread's `struct task` (not on the stack):

- Virtual runtime (`vruntime`) for MLFQ accounting
- Current MLFQ queue level
- CPU affinity mask
- Preemption counter
- Signal pending mask

### Kernel Stack Pointer

Each thread has its own kernel stack. On switch, the kernel stack pointer (`rsp`) of the outgoing thread is saved in its `struct task`. The incoming thread's `rsp` is loaded.

## Context Switch Assembly

The core switch is a single assembly function:

```asm
; switch_context(struct context **old_ctx, struct context *new_ctx)
switch_context:
    ; Save callee-saved registers of current thread
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    ; Save current RSP into *old_ctx
    mov [rdi], rsp
    ; Load new RSP from new_ctx
    mov rsp, rsi
    ; Restore callee-saved registers of new thread
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret   ; RIP is on the new thread's stack
```

The `rip` of the new thread is the top of its kernel stack — either a return address into the scheduler (for a thread that was previously preempted) or `thread_entry` (for a newly created thread).

## User/Kernel Transition

On entry from userspace (syscall or interrupt):

- `SYSCALL` instruction saves user `rip`, `rflags`, `rsp` into kernel-controlled MSRs (`LSTAR`, `STAR`, `SFMASK`).
- Kernel stack is loaded from the TSS `RSP0` field.
- User context is pushed onto the kernel stack.

On return to userspace (`SYSRET` or `IRETQ`):

- User context is restored from the kernel stack.
- Control returns to user `rip` at privilege level 3.

## Multiprocessor Considerations

- Each CPU has its own kernel stack per thread (one kernel stack per thread, not per CPU).
- A thread's kernel stack is only ever used by the CPU currently running that thread.
- TSS `RSP0` is updated to the current thread's kernel stack top on every context switch (per-CPU TSS).
- CR3 (page table base) is switched only when the new thread belongs to a different address space (different process). Threads within the same process share a CR3 and do not cause a TLB flush.

## Fast Path

- Switch between threads in the same process: no CR3 change, no TLB flush.
- Lazy FPU: no FXSAVE/XRSTOR unless FPU was actually used.
- Callee-saved register save/restore: 6 pushes + 6 pops = 12 instructions on the critical path.

## Related Documents

- [interrupt-handling.md](interrupt-handling.md)
- [privilege-levels.md](privilege-levels.md)
- [scheduling/overview.md](../scheduling/overview.md)
- [scheduling/per-core-runqueues.md](../scheduling/per-core-runqueues.md)
