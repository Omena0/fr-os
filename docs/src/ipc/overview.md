# IPC Overview

## Scope

The IPC (Inter-Process Communication) subsystem provides mechanisms for processes and threads to communicate and synchronize. All IPC mechanisms are exposed through the POSIX-standard syscall interface.

## Mechanisms

| Mechanism | Description | Document |
|---|---|---|
| Anonymous pipes | Unidirectional byte stream between related processes | [pipes.md](pipes.md) |
| Named pipes (FIFOs) | Unidirectional byte stream accessible by name on the filesystem | [pipes.md](pipes.md) |
| POSIX shared memory | Read/write memory region shared between processes | [shared-memory.md](shared-memory.md) |
| POSIX semaphores | Counting semaphores for synchronization | [synchronization-primitives.md](synchronization-primitives.md) |
| Futex | Fast userspace mutex with kernel assist | [synchronization-primitives.md](synchronization-primitives.md) |
| Signals | Asynchronous event notification | [signals.md](signals.md) |
| Unix domain sockets | Bidirectional byte or datagram stream via filesystem path | [unix-sockets.md](unix-sockets.md) |

## File Descriptor Abstraction

All IPC mechanisms that involve data streams (pipes, FIFOs, Unix domain sockets) are exposed as file descriptors. They support `read`, `write`, `poll`/`epoll`, `select`, `close`, and `dup`. This uniformity allows event loops to multiplex IPC channels alongside network sockets and files with a single `epoll` instance.

See [fd-abstraction.md](fd-abstraction.md).

## Namespace Isolation

IPC namespaces restrict visibility of IPC objects. A process in one IPC namespace cannot see or interact with IPC objects created in another namespace. Namespaces are created via `unshare(CLONE_NEWIPC)` or `clone(CLONE_NEWIPC)`.

See [namespace-isolation.md](namespace-isolation.md).

## Design Principles

- **Pipes**: Zero-copy ring buffer in the common case (same address space copy is avoided with direct page mapping when both ends are in the same process). Lock-free for the single-producer single-consumer case.
- **Shared memory**: No kernel involvement after the initial mapping — reads and writes go directly between processes. Synchronization (semaphores, futex) is the responsibility of the applications.
- **Futex**: Kernel involvement only on contention (slow path). Fast path is a CAS in userspace — no syscall, no kernel involvement.
- **Signals**: Asynchronous delivery at defined safe points (between instructions, on syscall return). Signal handlers run in user mode.

## Related Documents

- [pipes.md](pipes.md)
- [shared-memory.md](shared-memory.md)
- [synchronization-primitives.md](synchronization-primitives.md)
- [signals.md](signals.md)
- [unix-sockets.md](unix-sockets.md)
- [fd-abstraction.md](fd-abstraction.md)
- [namespace-isolation.md](namespace-isolation.md)
