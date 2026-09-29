# Syscall Filtering

## Overview

Syscall filtering allows a process to restrict its own system call interface. Once a filter is applied, any attempt to call a filtered syscall results in the process receiving `SIGKILL` (or a configurable error). This is the OS's equivalent of Linux's seccomp.

## Filter Architecture

A syscall filter is a policy applied per-thread that intercepts every syscall at the dispatch layer, before the syscall handler runs:

```
SYSCALL instruction
    ↓
Kernel syscall entry (syscall_entry)
    ↓
Filter check: is a filter installed?
    │ No filter → dispatch to handler
    │ Filter installed → evaluate policy
         │ ALLOW → dispatch to handler
         │ DENY_ERRNO → return -errno, do not execute handler
         │ KILL → deliver SIGKILL to calling thread, do not execute handler
```

## Filter Format

A filter is a sorted array of rules:

```c
#define SCFILT_ACTION_ALLOW      0
#define SCFILT_ACTION_DENY_ERRNO 1
#define SCFILT_ACTION_KILL       2

struct scfilt_rule {
    uint32_t syscall_nr;   // syscall number to match (UINT32_MAX = default rule)
    uint32_t action;       // ALLOW / DENY_ERRNO / KILL
    int32_t  deny_errno;   // errno to return if action == DENY_ERRNO
};

struct scfilt {
    uint32_t         nrules;
    struct scfilt_rule rules[]; // sorted by syscall_nr; last rule is the default
    uint32_t         default_action; // action if no rule matches
};
```

## Installing a Filter

```c
// sys_seccomp(operation, flags, ufilter)
sys_seccomp(SECCOMP_SET_MODE_FILTER, 0, &filter);
```

Rules:

- A filter can only be installed once per thread (it cannot be replaced, only augmented with stricter rules).
- Installing a filter requires `PR_SET_NO_NEW_PRIVS` to have been set (preventing the thread from gaining privileges that would bypass the filter via setuid exec).
- Filters are inherited by forked children.
- `exec` preserves filters.

## Example: Display Driver Filter

```c
struct scfilt_rule display_filter[] = {
    { SYS_read,       ALLOW, 0 },
    { SYS_write,      ALLOW, 0 },
    { SYS_mmap,       ALLOW, 0 },
    { SYS_munmap,     ALLOW, 0 },
    { SYS_poll,       ALLOW, 0 },
    { SYS_futex,      ALLOW, 0 },
    { SYS_close,      ALLOW, 0 },
    { SYS_ioctl,      ALLOW, 0 },
    { SYS_exit_group, ALLOW, 0 },
    { UINT32_MAX,     KILL,  0 },  // default: kill
};
```

Any syscall not in the allow list kills the process.

## Audit Logging

If the default action is `DENY_ERRNO` (not `KILL`), the filter can be configured to log the denied syscall:

```c
#define SCFILT_ACTION_DENY_LOG  3  // deny + write audit record
```

Audit records go to the kernel audit log (see [debugging/event-logging.md](../debugging/event-logging.md)).

## Filter Evaluation Performance

The filter is evaluated as a binary search over the sorted rule array:

- O(log N) where N = number of rules (typical: 10–50 rules).
- Adds ~5–20 ns to every syscall — negligible for most workloads.

## Related Documents

- [capabilities.md](capabilities.md)
- [user-group-model.md](user-group-model.md)
- [drivers/driver-isolation.md](../drivers/driver-isolation.md)
- [syscalls/dispatch-path.md](../syscalls/dispatch-path.md)
