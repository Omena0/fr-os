# Process Syscalls

## Syscall Table: Process Management (Numbers 0–17)

| Number | Name | Signature | Description |
|---|---|---|---|
| 0 | `sys_fork` | `pid_t sys_fork(void)` | Clone calling process. Child returns 0; parent returns child PID. |
| 1 | `sys_execve` | `int sys_execve(const char *path, char *const argv[], char *const envp[])` | Replace current process image with new program. |
| 2 | `sys_exit` | `void sys_exit(int status)` | Terminate the calling thread. |
| 3 | `sys_exit_group` | `void sys_exit_group(int status)` | Terminate all threads in the calling process. |
| 4 | `sys_wait4` | `pid_t sys_wait4(pid_t pid, int *wstatus, int options, struct rusage *ru)` | Wait for a child process to exit. |
| 5 | `sys_getpid` | `pid_t sys_getpid(void)` | Return the calling process's PID. |
| 6 | `sys_getppid` | `pid_t sys_getppid(void)` | Return the parent process's PID. |
| 7 | `sys_gettid` | `tid_t sys_gettid(void)` | Return the calling thread's TID. |
| 8 | `sys_clone` | `int sys_clone(uint64_t flags, void *stack, void *parent_tid, void *child_tid, void *tls)` | Create a new thread or process with fine-grained flag control. |
| 9 | `sys_kill` | `int sys_kill(pid_t pid, int sig)` | Send signal `sig` to process `pid`. |
| 10 | `sys_getpgid` | `pid_t sys_getpgid(pid_t pid)` | Return process group ID. |
| 11 | `sys_setpgid` | `int sys_setpgid(pid_t pid, pid_t pgid)` | Set process group ID. |
| 12 | `sys_setsid` | `pid_t sys_setsid(void)` | Create new session (detach from terminal). |
| 13 | `sys_nanosleep` | `int sys_nanosleep(const struct timespec *req, struct timespec *rem)` | Sleep for at least `req` nanoseconds. |
| 14 | `sys_clock_gettime` | `int sys_clock_gettime(clockid_t clk, struct timespec *tp)` | Read a POSIX clock (`CLOCK_MONOTONIC`, `CLOCK_REALTIME`). |
| 15 | `sys_getuid` | `uid_t sys_getuid(void)` | Return the calling process's real UID. |
| 16 | `sys_getgid` | `gid_t sys_getgid(void)` | Return the calling process's real GID. |
| 17 | `sys_arch_prctl` | `int sys_arch_prctl(int code, uint64_t addr)` | Per-thread register control (FS/GS base). |

The full syscall number assignment is in `src/include/uapi/syscall.h`. Numbers are grouped by domain: process (0–31), memory (32–63), files (64–127), IPC (128–159), sockets (160–199), scheduling (200–231), security (256–287).

## `sys_fork` Implementation

1. Allocate a new `struct process` for the child.
2. Copy the parent's page table (copy-on-write: pages are shared read-only and a write allocates a private copy).
3. Copy the fd table (increment refcounts on all open `struct file` objects).
4. Copy signal dispositions, credentials.
5. Child's `RAX` = 0, parent's `RAX` = child PID.
6. Place both child and parent on the scheduler run queues.

## `sys_clone` Flags

| Flag | Effect |
|---|---|
| `CLONE_VM` | Share virtual address space (thread, not process) |
| `CLONE_FS` | Share filesystem root and cwd |
| `CLONE_FILES` | Share file descriptor table |
| `CLONE_SIGHAND` | Share signal handlers |
| `CLONE_THREAD` | Same thread group (for `getpid()` to return same value) |
| `CLONE_NEWPID` | New PID namespace |
| `CLONE_NEWNS` | New mount namespace |
| `CLONE_SETTLS` | Set TLS base (FSBASE) to `tls` argument |

`CLONE_VM | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SETTLS`: creates a POSIX thread (used by `pthread_create`).

## `sys_execve` Implementation

1. Path resolution and permission check (execute bit, not a directory).
2. Parse ELF64 header and program headers.
3. Check ELF magic (`0x7F 'E' 'L' 'F'`), machine (`EM_X86_64`), type (`ET_EXEC` or `ET_DYN`).
4. Discard current address space (unmap all VMAs, free page tables).
5. Apply ASLR bases for the new address space.
6. Map ELF segments (LOAD segments) into the new address space.
7. Map VDSO and stack.
8. Build argument/environment/auxiliary vector on the new stack.
9. Set `RIP` = ELF entry point, `RSP` = new stack top. Jump (SYSRET).

## Related Documents

- [overview.md](overview.md)
- [dispatch-path.md](dispatch-path.md)
- [memory-syscalls.md](memory-syscalls.md)
- [ipc/signals.md](../ipc/signals.md)
- [scheduling/multicore-overview.md](../scheduling/multicore-overview.md)
