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
