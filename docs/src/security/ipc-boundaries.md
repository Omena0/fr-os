# IPC Security Boundaries

## Overview

IPC objects (pipes, shared memory, semaphores, Unix sockets) carry data between processes. Without access controls, a process could read messages intended for another or write malicious data into another's input. This document describes the security checks applied to each IPC mechanism.

## Pipe Security

Anonymous pipes are only accessible via inherited file descriptors:

- Created by one process, shared only with processes that received the fd via `fork` or explicit `sendmsg(SCM_RIGHTS)`.
- The kernel does not expose pipe fds by name — there is no way to "find" an existing anonymous pipe.
- If passed via `SCM_RIGHTS` over a Unix socket: the receiving process must have been granted the fd explicitly by the sender.

Named pipes (FIFOs) use standard filesystem permissions:

- The FIFO's inode has owner UID/GID and mode bits.
- `open(path, O_RDONLY)` requires read permission on the inode.
- A process with only write permission cannot read from the FIFO.

## Shared Memory Security

Named POSIX shared memory objects are protected by mode bits on the SHM inode:

```c
// Mode 0600: only owner can read/write
int fd = shm_open("/priv_shm", O_CREAT | O_RDWR, 0600);
```

`shm_open` performs the same UID/GID/mode permission check as `open` on a regular file. A process that does not own the SHM object and does not have `CAP_DAC_OVERRIDE` cannot open it without read/write permission in the mode bits.

Once mapped, the shared memory is subject to the page table protections:

- A process that maps with `PROT_READ` only cannot write to the region (SIGSEGV on write attempt).
- A process that has not mapped the region cannot access it at all.

## Semaphore Security

Named POSIX semaphores follow the same access control model as named shared memory: filesystem-style permission check at `sem_open` time.

## Unix Domain Socket Security

The server socket inode has mode bits. `connect(AF_UNIX, path)` requires:

- Write permission on the socket inode (to connect).
- The path must be accessible (read+execute on the directory).

Additional security:

- `SO_PEERCRED` allows the server to obtain the client's UID/GID/PID after `accept`, enabling authentication:

  ```c
  struct ucred cred;
  socklen_t len = sizeof(cred);
  getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &cred, &len);
  if (cred.uid != expected_uid) close(client_fd);  // reject unauthorized clients
  ```

## Futex Security

Futexes operate on a virtual address and the physical page underneath it. The kernel validates that:

- The futex address is within the process's mapped virtual address space.
- For process-shared futexes: both processes have a valid mapping for the shared page.

A process cannot use a futex to wake threads in a different process that it does not share memory with (the physical page hash would not match).

## Capability-Gated IPC

Some IPC operations require capabilities:

| Operation | Required capability |
|---|---|
| `mlock` shared memory | `CAP_IPC_LOCK` |
| `unshare(CLONE_NEWIPC)` | `CAP_SYS_ADMIN` |
| `setns(nsfd, CLONE_NEWIPC)` | `CAP_SYS_ADMIN` |
| Send signal to arbitrary process | `CAP_KILL` (if UID mismatch) |

## Related Documents

- [ipc/pipes.md](../ipc/pipes.md)
- [ipc/shared-memory.md](../ipc/shared-memory.md)
- [ipc/unix-sockets.md](../ipc/unix-sockets.md)
- [security/capabilities.md](capabilities.md)
- [security/namespaces.md](namespaces.md)
