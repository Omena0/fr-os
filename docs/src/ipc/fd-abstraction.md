# File Descriptor Abstraction

## Overview

The file descriptor (fd) abstraction provides a uniform integer handle for all I/O resources: regular files, directories, pipes, sockets, device files, and FIFOs. This uniformity enables generic I/O multiplexing via `poll`/`epoll`/`select` regardless of the underlying resource type.

## Process File Descriptor Table

Each process has a file descriptor table:

```c
#define MAX_FDS 1024   // per-process fd limit (default; adjustable via setrlimit)

struct fd_table {
    struct file *fds[MAX_FDS];  // NULL = closed
    uint32_t    flags[MAX_FDS]; // FD_CLOEXEC per fd
    spinlock_t  lock;
    int         next_free;      // hint for next free slot
};
```

File descriptors 0, 1, 2 are conventionally stdin, stdout, stderr.

## File Descriptor Operations

| Syscall | Operation |
|---|---|
| `open(path, flags, mode)` | Allocate a new fd referencing a filesystem file |
| `pipe(fds[2])` | Allocate two fds (read end and write end) |
| `socket(domain, type, proto)` | Allocate a new fd referencing a socket |
| `dup(fd)` | Create a new fd pointing to the same `struct file` (increments refcount) |
| `dup2(old, new)` | Create fd `new` pointing to the same object as fd `old`; close `new` first if open |
| `close(fd)` | Decrement `struct file` refcount; if it reaches 0, close the underlying resource |
| `fcntl(fd, F_DUPFD, ...)` | Similar to `dup` with fd number control |
| `fcntl(fd, F_SETFD, FD_CLOEXEC)` | Set close-on-exec flag |
| `fcntl(fd, F_SETFL, flags)` | Set file flags (e.g., `O_NONBLOCK`) |

## Close-on-Exec

If the `FD_CLOEXEC` flag is set on a file descriptor, it is automatically closed when the process calls `exec`. This prevents file descriptors from leaking into child processes unexpectedly.

All fds opened via `socket(AF_..., SOCK_CLOEXEC)` or `open(O_CLOEXEC)` have this flag set by default — a security best practice.

## Fork and fd Inheritance

On `fork`:

- The child receives a copy of the parent's fd table.
- Each `struct file`'s refcount is incremented.
- Both parent and child share the same `struct file` (and thus the same file offset for seekable files — be careful with shared offsets).

## `select` / `poll` / `epoll`

`poll(fds, nfds, timeout)`:

- For each fd in `fds`: calls `file->f_ops->poll(file, ...)` to query ready events.
- Sleeps until at least one fd is ready or timeout expires.
- O(N) per call — suitable for small fd sets.

`epoll` (event poll):

- Maintains a persistent interest set (rbtree, O(log N) add/remove).
- `epoll_wait` returns only ready events — O(M) where M is the number of ready fds.
- Suitable for servers managing thousands of connections.
- Backed by a per-epoll-fd ready list (doubly linked, protected by a spinlock).

## Limits

| Parameter | Default | Adjustable via |
|---|---|---|
| Per-process open fds | 1024 | `setrlimit(RLIMIT_NOFILE)` |
| System-wide open fds | 1M | Kernel compile-time constant |
| `epoll` interest list | No limit beyond per-fd limit | — |

## Related Documents

- [pipes.md](pipes.md)
- [unix-sockets.md](unix-sockets.md)
- [filesystem/vfs-api.md](../filesystem/vfs-api.md)
- [networking/socket-api.md](../networking/socket-api.md)
- [syscalls/file-syscalls.md](../syscalls/file-syscalls.md)
