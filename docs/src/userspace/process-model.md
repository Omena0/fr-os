# Process Model

## Overview

This document describes how processes and process groups are modeled in userspace — building on the kernel primitives described in the kernel and syscall documents.

## Process Hierarchy

All processes form a tree rooted at PID 1 (init):
```
PID 1 (init)
  ├─ PID 2 (udevd)
  ├─ PID 3 (dhcpd)
  └─ PID 4 (login shell)
       ├─ PID 5 (bash)
       │    ├─ PID 6 (ls)
       │    └─ PID 7 (grep)
       └─ PID 8 (vi)
```

When a parent dies before its child, the child is **reparented** to PID 1. PID 1 must call `waitpid(-1, ...)` to reap all of its adopted orphan children and prevent them becoming zombies.

## Process Groups and Sessions

| Concept | Description |
|---|---|
| **PID** | Unique process identifier |
| **PGID** (process group ID) | Group of related processes (e.g., a pipeline). Default PGID = PID of group leader. |
| **SID** (session ID) | Session: one or more process groups. Created by `setsid()`. |
| **Controlling terminal** | A terminal device associated with a session. |
| **Foreground process group** | The PGID that currently receives terminal input and signals. |

A typical shell session:
```
Session (SID=5, tty=/dev/tty1)
  Foreground group: PGID=7 (the running pipeline)
  Background group: PGID=6 (stopped job)
  Background group: PGID=5 (the shell itself)
```

## Process States

| State | Description |
|---|---|
| **Running** | Actively executing on a CPU |
| **Runnable** | Ready to run, waiting for CPU |
| **Sleeping (interruptible)** | Waiting for I/O, signal, or event; can be woken by a signal |
| **Sleeping (uninterruptible)** | Waiting for I/O that must complete (disk reads); cannot be interrupted by signals |
| **Stopped** | Suspended by `SIGSTOP` or `SIGTSTP`; resumed by `SIGCONT` |
| **Zombie** | Exited, but parent has not yet called `wait()` to collect exit status |

## Fork/Exec Pattern

The standard way to start a new program:
```c
pid_t pid = fork();
if (pid == 0) {
    // child: set up fd redirections, process group, etc.
    execvp(program, argv);
    // only reached on exec failure:
    perror("exec"); _exit(127);
}
// parent: optionally waitpid(pid, ...) or continue (background job)
```

## `_exit` vs `exit`

- `exit(status)`: calls `atexit` handlers, flushes stdio buffers, then `sys_exit_group`.
- `_exit(status)`: goes directly to `sys_exit_group` without any cleanup. Used in the child after `fork()` (before `exec`) to avoid double-flushing stdio buffers shared with the parent.

## `wait` Variants

| Function | Description |
|---|---|
| `wait(status)` | Wait for any child |
| `waitpid(pid, status, opts)` | Wait for specific child; `WNOHANG` for non-blocking |
| `waitid(which, id, info, opts)` | More flexible (wait for process group, etc.) |

Status decoding:
```c
if (WIFEXITED(status))   printf("exit code: %d\n", WEXITSTATUS(status));
if (WIFSIGNALED(status)) printf("killed by signal: %d\n", WTERMSIG(status));
if (WIFSTOPPED(status))  printf("stopped by signal: %d\n", WSTOPSIG(status));
```

## Related Documents

- [init-system.md](init-system.md)
- [shell.md](shell.md)
- [syscalls/process-syscalls.md](../syscalls/process-syscalls.md)
- [ipc/signals.md](../ipc/signals.md)
