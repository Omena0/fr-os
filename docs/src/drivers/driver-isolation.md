# Driver Isolation

## Overview

Driver isolation is the mechanism by which a userspace driver process crash cannot corrupt kernel state, interfere with other drivers, or affect unrelated processes. This is the primary safety benefit of the hybrid driver architecture.

## Isolation Mechanisms

### Ring-3 Process Isolation

Userspace drivers run in ring-3 with a private virtual address space. The kernel's memory is not accessible from ring-3 (SMAP + SMEP + U/S bit in page tables). A driver that dereferences an invalid pointer receives a SIGSEGV and crashes — it cannot write to kernel memory.

### Capability Minimization

Each driver process is started with only the capabilities it requires:

```c
cap_clear(caps);
cap_set(caps, CAP_IPC_LOCK);          // all drivers: lock shm pages
// device-specific:
cap_set(caps, CAP_NET_ADMIN);         // network drivers only
// ...
execve("/usr/lib/drivers/<name>", argv, envp_with_caps);
```

A compromised display driver cannot call `bind` on port 80, for example.

### Seccomp Syscall Filter

Each driver process has a syscall filter applied at startup:

```
ALLOW: read, write, mmap, munmap, poll, futex, close, exit_group
ALLOW: ioctl (on device control fd only)
KILL:  all others
```

This ensures a compromised driver cannot fork processes, make network connections, or modify files.

### ASLR

Driver processes are PIE binaries and benefit from ASLR (30-bit randomization for code, 22-bit for stack, 40-bit for mmap). This makes exploitation significantly harder.

### Shared Memory Boundary

The shared memory region between the kernel and the driver is the only communication channel. The kernel validates all data read from the shared region:

- Lengths are bounds-checked before use.
- Physical addresses provided by the driver are validated against known DMA ranges.
- The kernel never dereferences a pointer provided by the driver.

This follows the principle that shared memory is an untrusted input channel from the kernel's perspective.

### Crash Detection and Recovery

The kernel monitors driver processes. On crash (process exit):

1. The kernel detects the exit via the monitoring mechanism (a dedicated kthread watching driver process state).
2. Pending requests are completed with `-EIO`.
3. The device is marked unavailable.
4. The kernel logs the crash.
5. The driver is restarted (if within the restart limit — 3 restarts before declaring the device permanently failed).

### No Kernel Stack Sharing

Each driver has its own userspace stack. Kernel stacks (used during syscall and interrupt handling) are never shared with driver processes.

## Failure Modes

| Failure | Kernel impact | Recovery |
|---|---|---|
| Driver dereferences bad pointer | None (SIGSEGV to driver only) | Automatic restart |
| Driver hangs (deadlock) | I/O requests time out | SIGKILL from kernel watchdog, then restart |
| Driver fills cmd_queue | Kernel blocks new I/O; device stalls | Driver timeout → restart |
| Driver corrupts rsp_queue | Kernel validates entries; bad entries discarded | Log + restart |
| Driver calls forbidden syscall | SIGKILL (seccomp) | Restart |

## Related Documents

- [hybrid-architecture.md](hybrid-architecture.md)
- [userspace-drivers.md](userspace-drivers.md)
- [security/capabilities.md](../security/capabilities.md)
- [security/syscall-filtering.md](../security/syscall-filtering.md)
- [security/aslr.md](../security/aslr.md)
