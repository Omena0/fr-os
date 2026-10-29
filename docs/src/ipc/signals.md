# Signals

## Overview

Signals are an asynchronous notification mechanism. They deliver a notification to a process or thread, interrupting whatever it is currently doing and invoking a handler function.

## Signal Model

- 64 signals are defined (1–64). Signals 1–31 are standard POSIX signals. Signals 32–64 are real-time signals.
- Each process has a signal disposition table: for each signal, the action is one of: default, ignore, or a userspace handler function pointer.
- Each thread has a signal mask: a set of signals that are blocked (pending delivery until unblocked).

## Signal Delivery

Signals are delivered at defined safe points:

1. On return from a syscall (checked in the syscall exit path).
2. On return from an interrupt handler (checked in the interrupt exit path).
3. Never in the middle of a non-preemptible kernel section.

**Delivery procedure**:

1. Find the lowest-numbered pending, unblocked signal.
2. If the disposition is SIG_DFL: apply the default action (terminate, stop, or ignore).
3. If the disposition is SIG_IGN: discard the signal.
4. If the disposition is a handler: inject a signal frame onto the user stack and set the thread's `RIP` to the handler.

## Signal Frame

When a handler is invoked, the kernel pushes a `sigframe` onto the user stack:

```c
struct sigframe {
    struct sigaction  sa;           // signal action for re-entrance protection
    siginfo_t         info;         // signal information (si_signo, si_code, ...)
    ucontext_t        uc;           // saved user registers (all GPRs + RIP + RFLAGS)
    // guard word (random canary) follows
};
```

The thread begins executing the handler with `RSP` pointing to the `sigframe`. When the handler calls `sigreturn()`, the kernel restores the saved `ucontext_t` and returns to the original execution point.

## Pending Signal Bitmap

```c
struct task {
    uint64_t sig_pending;       // bitmap of pending signals (signals 1–64)
    uint64_t sig_mask;          // currently blocked signals
    struct sigaction sig_actions[64]; // per-signal disposition
};
```

A signal is pending if its bit is set in `sig_pending` and not set in `sig_mask`.

## Sending Signals

```c
int sys_kill(pid_t pid, int sig);         // send to process
int sys_tkill(tid_t tid, int sig);        // send to specific thread
int sys_rt_sigqueueinfo(pid_t pid, int sig, siginfo_t *info);  // with info
```

Sending a signal sets the corresponding bit in the target process/thread's `sig_pending`.

## Real-Time Signals

Signals 32–64 are real-time signals:

- **Queued**: Multiple instances can be pending simultaneously (unlike standard signals which are deduplicated).
- **Ordered delivery**: Delivered in lowest-signal-number order.
- **Carry data**: `siginfo_t.si_value` carries an integer or pointer payload.
- Used for timer notifications (`SIGRTMIN+N`), custom application events, and POSIX AIO completion.

## Signal Safety

Signal handlers have restricted access to functions. Only **async-signal-safe** functions (as defined by POSIX) may be called from a signal handler. Key safe functions: `write`, `_exit`, `signal`, `kill`, `sem_post`. Notably unsafe: `malloc`, `printf`, any function using global state.

## Related Documents

- [ipc/overview.md](overview.md)
- [kernel/interrupt-handling.md](../kernel/interrupt-handling.md)
- [syscalls/process-syscalls.md](../syscalls/process-syscalls.md)

## Implementation Status

What exists today, measured against the sections above:

- **Dispositions and masks.** `rt_sigaction`, `rt_sigprocmask` and
  `rt_sigpending` store per-task state. SIGKILL and SIGSTOP cannot be caught,
  ignored or blocked. execve resets caught signals to default, and fork inherits
  dispositions and the mask but not the pending set.
- **Sending.** `kill` marks the signal pending on the named process, on a process
  group (`pid == 0`, `pid < -1`), or on every process except init and the caller
  (`pid == -1`). `sig == 0` checks existence and permission only. A sender may
  signal a process with the same uid, or any process when it is root.
- **Delivery points.** Pending signals are delivered on return from a syscall and
  on return from an interrupt to ring 3. Fault signals (SIGSEGV, SIGFPE, SIGILL,
  SIGTRAP) are raised by the exception layer and take the same path.
- **Default actions.** Terminate for most signals; the process then reports the
  signal number through `wait4` (`WIFSIGNALED`, `WTERMSIG`). Ignored by default:
  SIGCHLD, SIGCONT, SIGURG, SIGWINCH. The stop signals (SIGSTOP, SIGTSTP,
  SIGTTIN, SIGTTOU) are also ignored by default, because there is no job control
  yet.
- **Handlers.** A handler is entered with its return address set to the libc
  restorer, which issues `rt_sigreturn`. The handler's signal is blocked during
  the call unless `SA_NODEFER`; `sa_mask` is added to the mask; `SA_RESETHAND`
  restores the default disposition on entry.

Not implemented yet, so the sections above are not yet true for these:

- **Frame layout.** The frame holds the saved general registers, RIP, RFLAGS, RSP
  and the old mask. It has no `sigframe.sa`, `siginfo_t`, `ucontext_t` or guard
  word, so `SA_SIGINFO` handlers receive no `siginfo_t`.
- **Interruption of blocking calls.** A task asleep in a blocking syscall
  (`read` on the console, `wait4`, `nanosleep`) is not woken by a signal. It sees
  the signal when the call returns, not with `EINTR`.
- **`sigaltstack`, `SA_ONSTACK`.** Handlers always run on the interrupted stack.
- **Job control.** Stop and continue are not implemented.
- **Real-time signals.** Signals 32–64 are accepted, but they are not queued: each
  is a single pending bit, and `siginfo_t.si_value` does not exist.
- **Other sending calls.** `sys_tkill` and `sys_rt_sigqueueinfo` do not exist.
