# ABI Stability

## Scope

The ABI stability guarantee applies exclusively to the **userspace-facing syscall interface**. Internal kernel APIs (between subsystems) are not stable and may change without notice.

## Versioning Model

Syscall ABI is versioned with a two-component version: `MAJOR.MINOR`.

- **MINOR** bumps: Additive changes only. New syscalls may be added. Existing syscall numbers, argument layout, and return value semantics are frozen.
- **MAJOR** bumps: Breaking changes permitted. A major version bump is a scheduled event requiring a compatibility shim layer for the previous major version to be maintained for at least two release cycles.

The current ABI version is encoded in a well-known memory location mapped read-only into every process address space (`VDSO_ABI_VERSION`).

## Stability Guarantees

| Guaranteed stable | Not stable |
|---|---|
| Syscall numbers | Internal kernel data structures |
| Syscall argument types and order | Kernel module ABI |
| Syscall return value semantics | `/proc`/`/sys` file format (informational only) |
| `errno` values for all documented syscalls | Driver internal protocol |
| Signal numbers and `sigaction` layout | Userspace driver shared memory layout (driver-version negotiated) |
| `struct stat`, `struct timespec`, `struct timeval` layout | Debug/tracing event format |

## Calling Convention

All syscalls use the x86-64 System V ABI calling convention with the following register assignment:

| Register | Role |
|---|---|
| `rax` | Syscall number |
| `rdi` | Argument 1 |
| `rsi` | Argument 2 |
| `rdx` | Argument 3 |
| `r10` | Argument 4 (not `rcx` — `rcx` is clobbered by `syscall`) |
| `r8` | Argument 5 |
| `r9` | Argument 6 |
| `rax` (return) | Return value or negated `errno` on error |

## VDSO

A Virtual Dynamic Shared Object (VDSO) is mapped into every process address space. It provides:

- Fast-path implementations for high-frequency syscalls that do not require a full privilege transition (e.g., `clock_gettime`, `gettimeofday`).
- The `VDSO_ABI_VERSION` constant.
- A `syscall_dispatch` trampoline used by libc.

The VDSO image is generated at kernel build time and embedded in the kernel binary.

## Compatibility Layer

When a MAJOR version increment occurs, the kernel activates a compatibility translation layer for the previous major version. Processes signal their expected ABI version via an `execve` auxiliary vector entry (`AT_ABI_VERSION`). The kernel routes their syscalls through the appropriate dispatch table.

## Related Documents

- [syscalls/overview.md](../syscalls/overview.md)
- [syscalls/abi-versioning.md](../syscalls/abi-versioning.md)
- [syscalls/dispatch-path.md](../syscalls/dispatch-path.md)
