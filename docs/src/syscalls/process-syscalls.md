# Process Syscalls

## Syscall Table: Process Management (Numbers 0–17)

| Number | Name | Signature | Description |
|---|---|---|---|
| 0 | `sys_fork` | `pid_t sys_fork(void)` | Clone calling process. Child returns 0; parent returns child PID. |
| 1 | `sys_exec` | `int sys_exec(const char *path, char *const argv[], char *const envp[])` | Replace current process image with new program. |
| 2 | `sys_exit` | `void sys_exit(int status)` | Terminate the calling thread. |
| 3 | `sys_exit_group` | `void sys_exit_group(int status)` | Terminate all threads in the calling process. |
| 4 | `sys_wait` | `pid_t sys_wait(int *wstatus)` | Wait for any child process to exit. |
| 5 | `sys_waitpid` | `pid_t sys_waitpid(pid_t pid, int *wstatus, int options)` | Wait for a specific child (or group). |
| 6 | `sys_getpid` | `pid_t sys_getpid(void)` | Return the calling process's PID. |
| 7 | `sys_getppid` | `pid_t sys_getppid(void)` | Return the parent process's PID. |
| 8 | `sys_gettid` | `tid_t sys_gettid(void)` | Return the calling thread's TID. |
| 9 | `sys_clone` | `int sys_clone(uint64_t flags, void *stack, void *parent_tid, void *child_tid, void *tls)` | Create a new thread or process with fine-grained flag control. |
| 10 | `sys_kill` | `int sys_kill(pid_t pid, int sig)` | Send signal `sig` to process `pid`. |
| 11 | `sys_tkill` | `int sys_tkill(tid_t tid, int sig)` | Send signal to specific thread. |
| 12 | `sys_getpgid` | `pid_t sys_getpgid(pid_t pid)` | Return process group ID. |
| 13 | `sys_setpgid` | `int sys_setpgid(pid_t pid, pid_t pgid)` | Set process group ID. |
| 14 | `sys_setsid` | `pid_t sys_setsid(void)` | Create new session (detach from terminal). |
| 15 | `sys_prctl` | `int sys_prctl(int option, uint64_t arg2, ...)` | Process-specific control operations. |
| 16 | `sys_nanosleep` | `int sys_nanosleep(const struct timespec *req, struct timespec *rem)` | Sleep for at least `req` nanoseconds. |
| 17 | `sys_clock_gettime` | `int sys_clock_gettime(clockid_t clk, struct timespec *tp)` | Read a POSIX clock (`CLOCK_MONOTONIC`, `CLOCK_REALTIME`). |

## `sys_fork` Implementation

1. Allocate a new `struct process` for the child.
2. Copy the parent's page table (copy-on-write — mark all writable pages read-only in both).
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

## `sys_exec` Implementation

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
