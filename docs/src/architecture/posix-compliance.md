# POSIX Compliance

## Compliance Goal

The OS targets broad POSIX.1-2017 (IEEE Std 1003.1-2017)[^posix-2017][^ieee-posix] compatibility for the process model, file system semantics, IPC primitives, and socket API. The intent is practical compatibility: programs written for Linux using standard POSIX APIs should compile and run without modification.

## References

- [The Open Group Base Specifications Issue 7 (POSIX.1-2017)][posix-2017]
- [IEEE Std 1003.1-2017][ieee-posix]
- [Linux man-pages — POSIX Conformance][linux-man-pages]
- [musl libc — POSIX Conformance][musl-posix]

[posix-2017]: https://pubs.opengroup.org/onlinepubs/9699919799/ "The Open Group Base Specifications Issue 7, 2018 edition"
[ieee-posix]: https://ieeexplore.ieee.org/document/7894736 "IEEE Std 1003.1-2017"
[linux-man-pages]: https://www.kernel.org/doc/man-pages/ "Linux man-pages project"
[musl-posix]: https://musl.libc.org/ "musl libc - POSIX conformance"

## Conformance Table

### Process Model

| Feature | Status | Notes |
|---|---|---|
| `fork()` | Full | Copy-on-write semantics |
| `exec()` family | Full | `execve`, `execvp`, `execle` etc. via libc wrappers |
| `wait()` / `waitpid()` | Full | WUNTRACED, WCONTINUED supported |
| `clone()` | Full | Thread creation, namespace flags |
| `exit()` / `_exit()` | Full | |
| `getpid()` / `getppid()` | Full | |
| Signals | Full | All standard signals; `sigaction`, `sigprocmask`, `sigsuspend` |
| `setuid()` / `setgid()` | Full | Per-process credential management |
| `getrlimit()` / `setrlimit()` | Full | Resource limits enforced by kernel |

### File System

| Feature | Status | Notes |
|---|---|---|
| `open()` / `close()` / `read()` / `write()` | Full | |
| `lseek()` | Full | |
| `stat()` / `fstat()` / `lstat()` | Full | |
| `mkdir()` / `rmdir()` | Full | |
| `link()` / `unlink()` / `rename()` | Full | |
| `symlink()` / `readlink()` | Full | |
| `chmod()` / `chown()` | Full | |
| `mmap()` / `munmap()` | Full | `MAP_SHARED`, `MAP_PRIVATE`, `MAP_ANONYMOUS` |
| `fcntl()` | Partial | `F_GETFL`, `F_SETFL`, `F_DUPFD`, `FD_CLOEXEC` |
| `ioctl()` | Partial | Device-specific; standard terminal ioctls (`TIOCGWINSZ` etc.) |
| `select()` / `poll()` / `epoll` | Full | |
| Directory traversal (`opendir`, `readdir`, `closedir`) | Full | via libc on top of `getdents` |

### IPC

| Feature | Status | Notes |
|---|---|---|
| Anonymous pipes (`pipe()`) | Full | Zero-copy ring-buffer implementation |
| Named pipes (FIFOs, `mkfifo()`) | Full | |
| POSIX shared memory (`shm_open()`, `mmap()`) | Full | |
| POSIX semaphores | Full | Named and unnamed |
| POSIX message queues | Partial | Basic send/receive; no priority queuing |
| System V IPC (semget, shmget, msgget) | Not planned | Use POSIX equivalents |

### Sockets

| Feature | Status | Notes |
|---|---|---|
| `socket()` / `bind()` / `connect()` | Full | |
| `listen()` / `accept()` | Full | |
| `send()` / `recv()` / `sendto()` / `recvfrom()` | Full | |
| `setsockopt()` / `getsockopt()` | Partial | Common options; not all socket options |
| `AF_INET` (IPv4) | Full | |
| `AF_UNIX` (Unix domain sockets) | Full | |
| `AF_INET6` (IPv6) | Not planned | Future milestone |
| Raw sockets (`SOCK_RAW`) | Full | |

### Threading (pthreads)

| Feature | Status | Notes |
|---|---|---|
| `pthread_create()` / `pthread_join()` | Full | Backed by `clone()` |
| Mutexes | Full | `pthread_mutex_*` |
| Condition variables | Full | `pthread_cond_*` |
| Semaphores | Full | `sem_post`, `sem_wait` |
| Thread-local storage | Full | `__thread` / `pthread_key_*` |

## Non-Conformance and Limitations

- **`/proc` filesystem**: A minimal read-only `/proc` is available for observability but does not implement the full Linux `/proc` interface.
- **`/sys` filesystem**: Not implemented. Device configuration is done through driver-specific interfaces.
- **Mandatory locking**: Not supported. Advisory locking (`fcntl` `F_SETLK`) is supported.
- **POSIX real-time signals** (`SIGRTMIN`–`SIGRTMAX`): Supported at signal number level; queuing depth may be limited.

## Related Documents

- [syscalls/overview.md](../syscalls/overview.md)
- [ipc/overview.md](../ipc/overview.md)
- [networking/socket-api.md](../networking/socket-api.md)
- [userspace/libc-overview.md](../userspace/libc-overview.md)
