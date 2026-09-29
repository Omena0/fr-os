# IPC Syscalls

## Syscall Table: IPC and Signals (Numbers 128–139)

| Number | Name | Signature | Description |
|---|---|---|---|
| 128 | `sys_pipe` | `int sys_pipe(int fds[2])` | Create anonymous pipe. `fds[0]` = read end, `fds[1]` = write end. |
| 129 | `sys_pipe2` | `int sys_pipe2(int fds[2], int flags)` | Like `pipe` but with `O_CLOEXEC | O_NONBLOCK` flags. |
| 130 | `sys_shm_open` | `int sys_shm_open(const char *name, int flags, mode_t mode)` | Open/create POSIX shared memory object. |
| 131 | `sys_shm_unlink` | `int sys_shm_unlink(const char *name)` | Remove a POSIX shared memory object name. |
| 132 | `sys_sem_open` | `int sys_sem_open(const char *name, int flags, mode_t mode, uint32_t value)` | Open/create POSIX named semaphore. Returns fd. |
| 133 | `sys_sem_post` | `int sys_sem_post(int semfd)` | Increment semaphore. Wake one waiter. |
| 134 | `sys_sem_wait` | `int sys_sem_wait(int semfd)` | Decrement semaphore. Block if zero. |
| 135 | `sys_sem_trywait` | `int sys_sem_trywait(int semfd)` | Decrement if > 0, else return `EAGAIN`. |
| 136 | `sys_futex` | `int sys_futex(uint32_t *uaddr, int op, uint32_t val, const struct timespec *timeout, uint32_t *uaddr2, uint32_t val3)` | Fast userspace mutex primitive. |
| 137 | `sys_rt_sigaction` | `int sys_rt_sigaction(int sig, const struct sigaction *act, struct sigaction *oact, size_t sigsetsize)` | Set signal handler. |
| 138 | `sys_rt_sigprocmask` | `int sys_rt_sigprocmask(int how, const sigset_t *set, sigset_t *oset, size_t sigsetsize)` | Modify signal mask. |
| 139 | `sys_rt_sigsuspend` | `int sys_rt_sigsuspend(const sigset_t *mask, size_t sigsetsize)` | Replace signal mask and wait for signal. |

## `sys_futex` Operations

| `op` | Description |
|---|---|
| `FUTEX_WAIT` | If `*uaddr == val`: sleep until FUTEX_WAKE or timeout |
| `FUTEX_WAKE` | Wake up to `val` threads waiting on `uaddr` |
| `FUTEX_WAIT_PRIVATE` | Like `FUTEX_WAIT` but for process-private futex (faster hash key) |
| `FUTEX_WAKE_PRIVATE` | Like `FUTEX_WAKE` but process-private |
| `FUTEX_REQUEUE` | Wake `val` threads; requeue remaining from `uaddr` to `uaddr2` (for `pthread_cond_broadcast`) |

## `sys_rt_sigaction` Structure

```c
struct sigaction {
    void     (*sa_handler)(int);   // SIG_DFL, SIG_IGN, or function pointer
    sigset_t   sa_mask;            // signals blocked during handler execution
    uint32_t   sa_flags;           // SA_RESTART, SA_SIGINFO, SA_NODEFER, etc.
    void     (*sa_restorer)(void); // must be set to __restore (libc internal)
};
```

`SA_SIGINFO`: use `sa_sigaction(int, siginfo_t *, ucontext_t *)` instead of `sa_handler`.
`SA_RESTART`: automatically restart interrupted syscalls on signal return.

## `sys_rt_sigprocmask` Operations

| `how` | Effect |
|---|---|
| `SIG_BLOCK` | Add `set` to current signal mask |
| `SIG_UNBLOCK` | Remove `set` from current signal mask |
| `SIG_SETMASK` | Replace current signal mask with `set` |

Signals `SIGKILL` and `SIGSTOP` cannot be blocked.

## `sys_pipe` Implementation

1. Allocate a `struct pipe` (with 64 KB ring buffer).
2. Allocate two `struct file` objects (read end and write end), backed by the pipe.
3. Install both into the process fd table. Return both fd numbers via `copy_to_user(fds, ...)`.

## `sys_futex` Implementation

`FUTEX_WAIT`:
1. Read `*uaddr` (via `copy_from_user` with SMAP).
2. If `*uaddr != val`: return `EAGAIN` (value changed before we slept).
3. Compute futex key: `(page_phys, offset_in_page)`.
4. Add current thread to the futex wait queue at that key.
5. Schedule (block).
6. On wakeup: return 0.

## Related Documents

- [overview.md](overview.md)
- [ipc/pipes.md](../ipc/pipes.md)
- [ipc/shared-memory.md](../ipc/shared-memory.md)
- [ipc/synchronization-primitives.md](../ipc/synchronization-primitives.md)
- [ipc/signals.md](../ipc/signals.md)
