# Security Model

## Threat Model

The OS is designed to defend against:

- **Malicious userspace code** attempting to gain kernel privileges or access other processes' memory.
- **Compromised services** (including userspace drivers) attempting to escalate privileges or access unauthorized resources.
- **Userspace programs** attempting to exhaust system resources (DoS against other processes).

The OS does not currently defend against:

- Physical access attacks.
- Side-channel attacks (Spectre/Meltdown mitigations are not in initial scope).
- Malicious kernel modules (module signing is future work).

## Privilege Separation

The system enforces two hardware privilege levels:

- **Ring 0 (kernel mode)**: Full hardware access. Runs kernel code only.
- **Ring 3 (user mode)**: Restricted. All hardware access mediated through syscalls.

Userspace drivers run in Ring 3. They access hardware only through kernel-mediated shared memory and I/O port abstractions.

## Capability Model

Every process holds a **capability set** — a bitmask of granted privileges. Capabilities are:

- **Inherited** from parent, subject to capability bounding set rules.
- **Dropped** explicitly via `cap_drop()` syscall.
- **Never gained** without an explicit grant from a process with `CAP_SETCAP` or a setuid execution.

Example capabilities:

| Capability | Grants |
|---|---|
| `CAP_NET_ADMIN` | Network interface configuration |
| `CAP_SYS_ADMIN` | Mount, namespace creation |
| `CAP_SYS_PTRACE` | Tracing other processes |
| `CAP_DAC_OVERRIDE` | Bypass file permission checks |
| `CAP_SETUID` / `CAP_SETGID` | Change process credentials |

## Syscall Filtering

Each process may install a syscall filter (seccomp-like policy). The filter is expressed as a policy program that runs on each syscall entry:

- **Allow**: Syscall proceeds normally.
- **Deny with error**: Syscall returns a specified errno (e.g., `EPERM`).
- **Kill process**: Process is killed with SIGSYS.

Filters are inherited by child processes. A filter may only be made more restrictive, never relaxed.

## Memory Protections

- **ASLR**: Applied to executable, stack, and mmap regions. See [architecture/memory-layout.md](memory-layout.md).
- **NX (No-Execute)**: All non-text pages are marked non-executable via page table NX bits.
- **Stack canaries**: Compiler-inserted canary values protect stack frame return addresses.
- **Guard pages**: Every thread stack has an unmapped guard page below its base; stack overflow triggers a fault rather than silent corruption.
- **Hardened allocator**: The kernel heap allocator places guard regions between allocations and zeroes freed memory.

## User and Group Model

- Every process has a **UID** (user ID), **GID** (primary group), and a set of **supplementary groups**.
- File access is controlled by standard Unix permission bits (owner/group/other, rwx) and the process credentials.
- `setuid`/`setgid` executables are supported but restricted to the capability model — a setuid binary gains capabilities, not raw root access.

## Namespace Isolation

Optional namespace isolation for:

- **PID namespace**: Process IDs are isolated; a process cannot see or signal processes outside its PID namespace.
- **Mount namespace**: Each namespace has an independent filesystem tree.
- **Resource namespace**: Resource limits are scoped per namespace.

Namespace creation requires `CAP_SYS_ADMIN`.

## Related Documents

- [security/overview.md](../security/overview.md)
- [security/capabilities.md](../security/capabilities.md)
- [security/syscall-filtering.md](../security/syscall-filtering.md)
- [security/namespaces.md](../security/namespaces.md)
- [security/aslr.md](../security/aslr.md)
