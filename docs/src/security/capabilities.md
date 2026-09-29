# Capabilities

## Overview

Capabilities decompose the traditional "root or not root" binary privilege model into a set of distinct, individually grantable privileges. Instead of giving a service full root access, it is given only the specific capabilities it needs.

## Capability Implementation

Capabilities are stored as a 64-bit bitmask in `struct cred`:

```c
typedef uint64_t cap_t;

// Test, set, clear capability bits:
#define cap_isset(caps, cap)   ((caps) & (1ULL << (cap)))
#define cap_set(caps, cap)     ((caps) |= (1ULL << (cap)))
#define cap_clear(caps, cap)   ((caps) &= ~(1ULL << (cap)))
```

Each process has three capability sets:

- **Permitted** (`cap_permitted`): The maximum set the process may ever have.
- **Effective** (`cap_effective`): The set currently checked against.
- **Inheritable** (`cap_inheritable`): The set that can be passed to `execve`.

## Defined Capabilities

| Bit | Name | Grants |
|---|---|---|
| 0 | `CAP_NET_ADMIN` | Configure network interfaces, routing, firewall |
| 1 | `CAP_NET_BIND` | Bind to ports < 1024 |
| 2 | `CAP_NET_RAW` | Use raw and packet sockets |
| 3 | `CAP_SYS_ADMIN` | Create namespaces, mount filesystems, load modules |
| 4 | `CAP_SYS_MODULE` | Load/unload kernel modules |
| 5 | `CAP_SYS_RAWIO` | Access raw I/O ports and physical memory |
| 6 | `CAP_SYS_PTRACE` | ptrace any process |
| 7 | `CAP_SYS_NICE` | Set scheduler class and priority |
| 8 | `CAP_SYS_RESOURCE` | Override resource limits |
| 9 | `CAP_SETUID` | Make arbitrary setuid changes |
| 10 | `CAP_SETGID` | Make arbitrary setgid changes |
| 11 | `CAP_SETCAP` | Modify capability sets |
| 12 | `CAP_DAC_OVERRIDE` | Bypass file permission checks |
| 13 | `CAP_CHOWN` | Change file ownership arbitrarily |
| 14 | `CAP_KILL` | Send signals to any process |
| 15 | `CAP_IPC_LOCK` | Lock memory (mlock) |
| 16 | `CAP_AUDIT_WRITE` | Write audit records (future) |

## Capability Checks

Example: `sys_mount` requires `CAP_SYS_ADMIN`:

```c
if (!cap_isset(current->cred->cap_effective, CAP_SYS_ADMIN))
    return -EPERM;
```

Root (euid == 0) is a special case: if the process has `euid == 0` and `cap_permitted` is the full set (legacy root), all capability checks pass.

## Capability Transition on execve

When a process calls `execve`:

1. New effective set = (inheritable & binary_inheritable) | (permitted & binary_permitted_file).
2. For non-setuid binaries: effective set typically drops all capabilities.
3. For setuid-root binaries: permitted and effective sets become the full set (legacy behavior).

Privilege-separated services should be setuid to a dedicated UID with only the needed capabilities in the file capability set (stored in the binary's extended attributes — future feature), so they never need to be root.

## Dropping Capabilities

Services should drop unneeded capabilities as early as possible:

```c
cap_t keep = 0;
cap_set(keep, CAP_NET_BIND);
sys_capset(keep, keep, 0);  // permitted=keep, effective=keep, inheritable=0
```

After dropping, even if the process is exploited, the attacker has only the remaining capabilities.

## Related Documents

- [user-group-model.md](user-group-model.md)
- [syscall-filtering.md](syscall-filtering.md)
- [security/overview.md](overview.md)
- [drivers/driver-isolation.md](../drivers/driver-isolation.md)
