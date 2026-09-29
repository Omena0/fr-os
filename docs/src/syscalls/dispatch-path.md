# Syscall Dispatch Path

## Overview

The syscall dispatch path is the code sequence from the `SYSCALL` instruction in userspace to the invocation of the appropriate kernel handler and back.

## SYSCALL / SYSRET Mechanism

x86-64 provides `SYSCALL` / `SYSRET` as fast ring-3 to ring-0 transition instructions:
- `SYSCALL` saves `RIP` to `RCX`, `RFLAGS` to `R11`, loads kernel `CS`/`SS` from `STAR`, loads kernel `RIP` from `LSTAR`, clears `RFLAGS` bits set in `SFMASK`.
- `SYSRET` restores `RIP` from `RCX`, `RFLAGS` from `R11`, restores user `CS`/`SS`.

No stack switch is performed by the CPU — the kernel sets up `RSP` to the kernel stack from the TSS `RSP0` field.

## Kernel-Side Entry: `syscall_entry`

```asm
syscall_entry:
    ; At entry: RCX = user RIP, R11 = user RFLAGS, RSP = user RSP (untrusted!)
    swapgs                          ; switch to kernel GS (per-CPU data)
    mov [gs:USER_RSP_OFFSET], rsp   ; save user RSP
    mov rsp, [gs:KERNEL_RSP_OFFSET] ; load kernel RSP from per-CPU data
    
    ; Save caller-saved registers not preserved by SYSCALL:
    push r11  ; user RFLAGS
    push rcx  ; user RIP
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    
    ; RAX = syscall number, RDI/RSI/RDX/R10/R8/R9 = args
    
    ; Bounds check:
    cmp rax, SYSCALL_MAX
    jae syscall_invalid
    
    ; Seccomp filter check (if installed):
    call seccomp_check_syscall      ; returns 0=allow, -errno=deny/kill
    test rax, rax
    jnz syscall_filtered
    
    ; Dispatch:
    mov rax, [syscall_table + rax*8]
    call rax                        ; args already in RDI/RSI/RDX/R10/R8/R9
    
syscall_return:
    ; RAX = return value
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    pop rcx  ; user RIP
    pop r11  ; user RFLAGS
    mov rsp, [gs:USER_RSP_OFFSET]   ; restore user RSP
    swapgs
    sysretq                         ; return to ring-3
```

## Argument Validation

Every syscall handler validates its arguments before use:
- Pointer arguments: verified to be in user address space (not in kernel virtual range).
- String lengths: bounded.
- Sizes: checked against reasonable limits.
- `copy_from_user` / `copy_to_user`: used for all user memory access (respects SMAP).

Any argument that fails validation: return `-EFAULT` or `-EINVAL`.

## Performance

Measured on QEMU with KVM enabled:
- Fast path (simple syscall, no seccomp): ~80–120 ns.
- With seccomp filter (binary search over 10 rules): ~130–160 ns.
- With context switch to another thread: ~1–2 µs.

The target is < 200 ns per syscall (including argument validation and return).

## Slow Path: Syscall via `INT 0x80`

For compatibility, `INT 0x80` is also supported as a legacy syscall entry point (using 32-bit argument convention). This is slower (~3× overhead vs SYSCALL) and is not used by new code.

## Related Documents

- [overview.md](overview.md)
- [abi-versioning.md](abi-versioning.md)
- [kernel/syscall-abi.md](../kernel/syscall-abi.md)
- [kernel/privilege-levels.md](../kernel/privilege-levels.md)
- [security/syscall-filtering.md](../security/syscall-filtering.md)
