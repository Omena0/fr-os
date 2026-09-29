# Security Syscalls

## Syscall Table: Security (Numbers 200–210)

| Number | Name | Signature | Description |
|---|---|---|---|
| 200 | `sys_getuid` | `uid_t sys_getuid(void)` | Get real UID |
| 201 | `sys_geteuid` | `uid_t sys_geteuid(void)` | Get effective UID |
| 202 | `sys_getgid` | `gid_t sys_getgid(void)` | Get real GID |
| 203 | `sys_getegid` | `gid_t sys_getegid(void)` | Get effective GID |
| 204 | `sys_setuid` | `int sys_setuid(uid_t uid)` | Set UID (requires `CAP_SETUID` if not own UID) |
| 205 | `sys_setgid` | `int sys_setgid(gid_t gid)` | Set GID (requires `CAP_SETGID`) |
| 206 | `sys_setresuid` | `int sys_setresuid(uid_t ruid, uid_t euid, uid_t suid)` | Set real/effective/saved UIDs |
| 207 | `sys_setresgid` | `int sys_setresgid(gid_t rgid, gid_t egid, gid_t sgid)` | Set real/effective/saved GIDs |
| 208 | `sys_getgroups` | `int sys_getgroups(int size, gid_t *list)` | Get supplementary groups |
| 209 | `sys_setgroups` | `int sys_setgroups(int size, const gid_t *list)` | Set supplementary groups (`CAP_SETGID`) |
| 210 | `sys_capget` | `int sys_capget(struct cap_header *hdrp, struct cap_data *datap)` | Get capability sets |
| 211 | `sys_capset` | `int sys_capset(struct cap_header *hdrp, const struct cap_data *datap)` | Set capability sets |
| 212 | `sys_prctl` | `int sys_prctl(int option, uint64_t arg2, ...)` | Process control (capabilities, name, etc.) |
| 213 | `sys_seccomp` | `int sys_seccomp(uint32_t op, uint32_t flags, void *uargs)` | Install syscall filter |
| 214 | `sys_unshare` | `int sys_unshare(int flags)` | Create new namespaces for this process |
| 215 | `sys_setns` | `int sys_setns(int nsfd, int nstype)` | Join an existing namespace |

## `sys_capget` / `sys_capset` Structures

```c
struct cap_header {
    uint32_t version;   // LINUX_CAPABILITY_VERSION_3
    int32_t  pid;       // 0 = self
};

struct cap_data {
    uint32_t effective;   // low 32 bits of effective capability set
    uint32_t permitted;   // low 32 bits of permitted set
    uint32_t inheritable; // low 32 bits of inheritable set
    // Second struct cap_data provides bits 32–63 (version 3)
};
```

## `sys_prctl` Options

| `option` | Effect |
|---|---|
| `PR_SET_NAME` | Set thread name (up to 16 bytes, for debugging) |
| `PR_GET_NAME` | Get thread name |
| `PR_SET_NO_NEW_PRIVS` | Prevent any future privilege gain (required before `sys_seccomp`) |
| `PR_GET_NO_NEW_PRIVS` | Check if no-new-privs is set |
| `PR_SET_DUMPABLE` | Control if core dumps are produced |
| `PR_GET_DUMPABLE` | Get dumpable status |
| `PR_SET_PDEATHSIG` | Set signal sent to process when parent dies |
| `PR_CAP_AMBIENT_RAISE` | Raise ambient capability (for capability inheritance across exec) |

## `sys_seccomp` Operations

| `op` | Description |
|---|---|
| `SECCOMP_SET_MODE_STRICT` | Allow only `read`, `write`, `exit`, `sigreturn`. Kill on any other. |
| `SECCOMP_SET_MODE_FILTER` | Install BPF-like filter (our scfilt format). |
| `SECCOMP_GET_ACTION_AVAIL` | Check if a filter action type is supported. |

Requires `PR_SET_NO_NEW_PRIVS` to have been set, or `CAP_SYS_ADMIN`.

## `sys_setuid` Implementation

1. `copy_from_user(uid)` (no pointer — it's a value argument).
2. Permission check:
   - If `euid == 0`: set `uid`, `euid`, `suid` all to `uid`.
   - Else if `uid == current->uid` or `uid == current->suid`: set `euid = uid` (drop privileges).
   - Else: return `-EPERM`.
3. Update `current->cred`.
4. If `euid` changed to non-zero: drop all capabilities (unless `CAP_SETUID` is in the permitted set and the process explicitly retained them via `sys_capset`).

## `sys_unshare` Implementation

1. Validate `flags` (only known `CLONE_NEW*` bits).
2. Requires `CAP_SYS_ADMIN`.
3. For each namespace type in `flags`:
   - Allocate a new namespace of that type.
   - Initialize it (copy parent namespace state where appropriate).
   - Update `current->ns_ptrs[type]` to point to the new namespace.
4. Return 0 on success.

## Related Documents

- [overview.md](overview.md)
- [security/capabilities.md](../security/capabilities.md)
- [security/syscall-filtering.md](../security/syscall-filtering.md)
- [security/user-group-model.md](../security/user-group-model.md)
- [security/namespaces.md](../security/namespaces.md)
