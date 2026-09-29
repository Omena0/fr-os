# IPC Namespace Isolation

## Overview

Namespaces partition global resources so that processes within a namespace see their own isolated view of those resources. Processes in different namespaces cannot interact via the corresponding resource.

The OS implements the following namespaces relevant to IPC and process isolation:

| Namespace | Clone flag | Isolates |
|---|---|---|
| PID namespace | `CLONE_NEWPID` | Process IDs; nested PID trees |
| Mount namespace | `CLONE_NEWNS` | Filesystem mount tree |
| IPC namespace | `CLONE_NEWIPC` | POSIX shared memory, semaphore namespaces |
| Resource namespace | `CLONE_NEWRESOURCE` | Resource limits (planned) |

## IPC Namespace

An IPC namespace contains its own:

- Named POSIX shared memory objects (`shm_open` namespace).
- Named POSIX semaphores (`sem_open` namespace).

Processes in different IPC namespaces cannot see or access each other's named IPC objects.

Creation:

```c
// Unshare IPC namespace (creates a fresh, empty namespace for this process)
unshare(CLONE_NEWIPC);

// Fork a child with a new IPC namespace
clone(child_fn, stack, CLONE_NEWIPC | SIGCHLD, arg);
```

## PID Namespace

In a PID namespace, process IDs are scoped to the namespace:

- The first process in a new PID namespace has PID 1 within that namespace.
- The kernel maps the namespace-local PID to a global PID internally.
- A process can see PIDs in its namespace and all parent namespaces (but not child namespaces).

```c
// child sees itself as PID 1; parent sees it as a global PID
clone(child_fn, stack, CLONE_NEWPID | SIGCHLD, arg);
```

## Mount Namespace

Mount namespaces allow different processes to have different views of the filesystem:

- A process in a new mount namespace starts with a copy of the parent's mount tree (unless `MS_PRIVATE` propagation mode is used).
- Mounts made inside the new namespace are not visible outside (and vice versa, by default).

This is the mechanism used by container runtimes to give each container its own root filesystem.

## Namespace Inheritance

By default, a process inherits all namespaces from its parent. New namespaces are created only when explicitly requested via:

- `clone(fn, stack, CLONE_NEW* | ..., arg)` — child starts in new namespace.
- `unshare(CLONE_NEW*)` — calling process moves into a new namespace.
- `setns(nsfd, nstype)` — join an existing namespace (identified by an fd from `/proc/<pid>/ns/<type>`).

## Namespace File Descriptors

Each namespace has a file in `/proc/<pid>/ns/`:

```
/proc/<pid>/ns/pid    → fd referencing the process's PID namespace
/proc/<pid>/ns/mnt    → fd referencing the mount namespace
/proc/<pid>/ns/ipc    → fd referencing the IPC namespace
```

Opening this file returns a namespace fd. `setns(fd, 0)` allows any process with sufficient capabilities to join that namespace.

## Capability Requirements

Creating a new namespace requires `CAP_SYS_ADMIN`. Joining an existing namespace also requires `CAP_SYS_ADMIN` (or the process is the owner of the namespace — a future user namespace feature).

## Related Documents

- [shared-memory.md](shared-memory.md)
- [ipc/overview.md](overview.md)
- [security/namespaces.md](../security/namespaces.md)
- [filesystem/mount-system.md](../filesystem/mount-system.md)
