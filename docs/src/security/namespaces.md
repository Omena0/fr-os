# Security Namespaces

## Overview

Namespaces provide isolation boundaries for system resources. Each namespace is an independent instance of a global resource, visible only to processes within that namespace.

## Namespace Types

| Namespace | Isolates | Clone flag |
|---|---|---|
| PID | Process IDs | `CLONE_NEWPID` |
| Mount | Filesystem mount tree | `CLONE_NEWNS` |
| IPC | POSIX IPC objects (shm, semaphores) | `CLONE_NEWIPC` |
| Network | Network interfaces, routing, sockets | `CLONE_NEWNET` |
| UTS | Hostname and domain name | `CLONE_NEWUTS` |

## PID Namespace Isolation

Within a PID namespace, processes can only see processes in the same namespace (or nested child namespaces). Signals cannot be sent across namespace boundaries:

```
Global namespace (init):  PIDs 1–100
  └── Container A PID namespace:
        PID 1 (process A's init)   [global PID 200]
        PID 2 (process A's service) [global PID 201]
  └── Container B PID namespace:
        PID 1 (process B's init)   [global PID 300]
        PID 2 (process B's service) [global PID 301]
```

Process A's PID 2 cannot kill Process B's PID 2 via `sys_kill(2, SIGKILL)` — they are in different namespaces and `2` in A's namespace refers to a different global PID.

## Mount Namespace Isolation

Each container gets its own mount namespace. The container's root filesystem is mounted inside the namespace:

```c
// Container setup:
clone(container_init, stack, CLONE_NEWPID | CLONE_NEWNS | SIGCHLD, arg);
// Inside container_init:
chroot("/containers/A/rootfs");
mount(NULL, "/proc", "procfs", 0, NULL);
```

Mounts inside the container are not visible outside. The host's mount tree is not visible to the container.

## IPC Namespace Isolation

IPC namespaces prevent POSIX shared memory and semaphores from leaking between containers:

- A process in namespace A calling `shm_open("/shared")` creates an object visible only to namespace A.
- A process in namespace B calling `shm_open("/shared")` creates a completely separate object.

## Network Namespace (Planned)

A network namespace provides each container with:

- Independent network interfaces (veth pairs connect namespace to host).
- Independent routing table.
- Independent socket space (ports used in namespace A do not conflict with namespace B).

## Security Implications

Namespaces prevent cross-container interference:

- A process in one container cannot send signals to another container.
- A process in one container cannot access another container's shared memory.
- A process in one container cannot read another container's `/proc` entries.

However, namespaces are not a complete security boundary — they must be combined with:

- Seccomp syscall filtering (prevent namespace escape via privileged syscalls).
- Capability restriction (prevent `CAP_SYS_ADMIN` in containers unless explicitly granted).
- Resource limits (cgroups-equivalent, planned — prevent resource exhaustion affecting the host).

## Creating and Joining Namespaces

```c
// Create new namespaces for this process:
unshare(CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWIPC);

// Join an existing namespace (by fd from /proc/<pid>/ns/<type>):
int nsfd = open("/proc/100/ns/mnt", O_RDONLY);
setns(nsfd, CLONE_NEWNS);
```

Both `unshare` and `setns` require `CAP_SYS_ADMIN`.

## Related Documents

- [ipc/namespace-isolation.md](../ipc/namespace-isolation.md)
- [filesystem/mount-system.md](../filesystem/mount-system.md)
- [security/capabilities.md](capabilities.md)
- [security/overview.md](overview.md)
