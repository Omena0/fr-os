# Syscalls Overview

## Design

The system call interface is the exclusive boundary between user-mode code and the kernel. Every kernel service — memory allocation, file I/O, process management, networking — is accessed via a numbered syscall.

The OS targets ~200 syscalls (POSIX-compatible subset). The full table is split across domain-specific documents.

## Syscall Dispatch Architecture

```
User code: SYSCALL instruction
    → CPU saves RIP, RSP, RFLAGS; loads kernel RSP0 from TSS
    → CPU jumps to LSTAR MSR (syscall_entry)

syscall_entry (assembly):
    → save caller-saved registers
    → switch to kernel GS (swapgs)
    → validate syscall number (< SYSCALL_MAX)
    → check seccomp filter (if installed)
    → call syscall_table[rax](args...)
    → restore registers
    → SYSRET
```

See [dispatch-path.md](dispatch-path.md).

## Calling Convention

| Register | Role |
|---|---|
| `RAX` | Syscall number (on call); return value (on return) |
| `RDI` | Argument 1 |
| `RSI` | Argument 2 |
| `RDX` | Argument 3 |
| `R10` | Argument 4 (NOT `RCX` — RCX is used by SYSCALL) |
| `R8` | Argument 5 |
| `R9` | Argument 6 |

Return value: non-negative = success; negative = `-errno` (e.g., `-ENOENT = -2`).

## Syscall Domains

| Document | Domain | Syscall numbers |
|---|---|---|
| [process-syscalls.md](process-syscalls.md) | Process management | 0–17 |
| [memory-syscalls.md](memory-syscalls.md) | Memory management | 32–38 |
| [file-syscalls.md](file-syscalls.md) | Filesystem | 64–91 |
| [ipc-syscalls.md](ipc-syscalls.md) | IPC and signals | 128–139 |
| [socket-syscalls.md](socket-syscalls.md) | Networking | 160–170 |
| [scheduling-syscalls.md](scheduling-syscalls.md) | Scheduler control | 180–194 |
| [security-syscalls.md](security-syscalls.md) | Security and capabilities | 200–210 |

## Error Handling

Every syscall returns either:
- A non-negative value on success (meaning depends on the syscall).
- A negative errno value on failure.

In libc wrappers:
```c
int open(const char *path, int flags, ...) {
    long ret = sys_open(path, flags, mode);
    if (ret < 0) { errno = -ret; return -1; }
    return (int)ret;
}
```

## ABI Versioning

The syscall ABI is versioned. `AT_ABI_VERSION` is provided in the `execve` auxiliary vector. Userspace can check the ABI version and use fallback paths for older kernels. See [abi-versioning.md](abi-versioning.md).

## Related Documents

- [dispatch-path.md](dispatch-path.md)
- [abi-versioning.md](abi-versioning.md)
- [process-syscalls.md](process-syscalls.md)
- [memory-syscalls.md](memory-syscalls.md)
- [file-syscalls.md](file-syscalls.md)
- [ipc-syscalls.md](ipc-syscalls.md)
- [socket-syscalls.md](socket-syscalls.md)
- [scheduling-syscalls.md](scheduling-syscalls.md)
- [security-syscalls.md](security-syscalls.md)
