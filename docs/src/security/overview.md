# Security Overview

## Design Philosophy

Security is built into the OS at every layer. The core principle is **default-deny**: processes start with minimal privileges and must be explicitly granted capabilities to access resources.

## Threat Model

| Threat | Mitigation |
|---|---|
| Unprivileged process exploits kernel via syscall | Syscall argument validation, copy_from/to_user, seccomp filter |
| Process reads another process's memory | Virtual address space isolation (page tables, U/S bit) |
| Heap/stack buffer overflow → arbitrary code execution | ASLR + NX/XD + stack canaries + guard pages |
| Compromised service pivots to root | Capability bitmask, seccomp filter, namespace isolation |
| Kernel code execution via DMA | IOMMU (future); currently QEMU provides virtual device isolation |
| Timing side-channels (Spectre) | Kernel page-table isolation (KPTI, future); SMEP prevents kernel gadgets from userspace |
| Resource exhaustion (DoS) | Per-process resource limits (`RLIMIT_*`), memory overcommit policy |

## Security Subsystems

| Subsystem | Document |
|---|---|
| User and group identity | [user-group-model.md](user-group-model.md) |
| Capability-based privilege control | [capabilities.md](capabilities.md) |
| Syscall filtering (seccomp-like) | [syscall-filtering.md](syscall-filtering.md) |
| Address Space Layout Randomization | [aslr.md](aslr.md) |
| Stack canaries | [stack-canaries.md](stack-canaries.md) |
| NX / execute-disable enforcement | [nx-enforcement.md](nx-enforcement.md) |
| Hardened memory allocator | [hardened-allocator.md](hardened-allocator.md) |
| Namespace isolation | [namespaces.md](namespaces.md) |

## Privilege Hierarchy

```
root (UID 0) — all capabilities implicitly available
    ↓ capability bit cleared
privileged service (e.g., network stack) — specific capabilities granted
    ↓ further restrictions
userspace driver — minimal capability set + seccomp filter
    ↓
unprivileged user process — no capabilities by default
```

## Kernel Self-Protection

- **SMAP** (Supervisor Mode Access Prevention): Kernel cannot inadvertently access user memory without explicit `copy_from/to_user`.
- **SMEP** (Supervisor Mode Execution Prevention): Kernel cannot execute code from user pages. Prevents ret2usr attacks.
- **KASLR**: Kernel text base randomized at boot. Disclosed kernel addresses are not available via any unauthenticated interface.
- **Stack canaries**: Per-kthread random canaries in kernel stacks.

## Default Security Posture for New Processes

On `execve`:

1. All capabilities are dropped (unless the binary has the setuid bit or the parent grants them via the capability inheritance model).
2. No syscall filter (applied by the process itself via `sys_seccomp`).
3. No special memory permissions.
4. Signal dispositions reset to defaults.

## Related Documents

- [user-group-model.md](user-group-model.md)
- [capabilities.md](capabilities.md)
- [syscall-filtering.md](syscall-filtering.md)
- [aslr.md](aslr.md)
- [stack-canaries.md](stack-canaries.md)
- [nx-enforcement.md](nx-enforcement.md)
- [hardened-allocator.md](hardened-allocator.md)
- [namespaces.md](namespaces.md)
- [architecture/security-model.md](../architecture/security-model.md)
