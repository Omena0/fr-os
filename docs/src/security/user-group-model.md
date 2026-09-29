# User and Group Model

## Overview

The OS uses a standard Unix user/group identity model. Every process has credentials: a user ID (UID) and group ID (GID), plus supplementary groups. These are checked by the kernel for every access to a file, IPC object, or privileged operation.

## Credential Structure

```c
struct cred {
    uid_t  uid;       // real user ID
    uid_t  euid;      // effective user ID (used for permission checks)
    uid_t  suid;      // saved set-user-ID (for setuid programs)
    gid_t  gid;       // real group ID
    gid_t  egid;      // effective group ID
    gid_t  sgid;      // saved set-group-ID
    uint32_t ngroups; // number of supplementary groups
    gid_t  groups[NGROUPS_MAX]; // supplementary groups (max 32)
    cap_t  caps;      // capability bitmask (see capabilities.md)
};
```

Each process has one `struct cred` (reference-counted; `fork` shares, `execve` may create a new one).

## Permission Check Algorithm

For file access (`open`, `stat`, `mkdir`, etc.):

1. If `euid == 0` (root): grant all access (bypass mode/owner check). Still subject to capability checks for special operations.
2. If `euid == file.uid`: use the **owner** permission bits (rwx in bits 6–8).
3. Else if `egid == file.gid` or any supplementary group matches: use **group** permission bits (bits 3–5).
4. Else: use **other** permission bits (bits 0–2).

## Setuid / Setgid Programs

If a binary has the setuid bit set (`-rwsr-xr-x`):

- On `execve`, the process's `euid` is set to the binary's owner UID.
- This allows a non-root process to temporarily gain the privileges of a specific user (typically root).
- The real UID (`uid`) remains the original user's UID (for `getuid()` introspection).

Setuid programs must be carefully audited. They represent the highest-risk executables on the system.

## Standard System Users

| UID | Username | Purpose |
|---|---|---|
| 0 | root | Superuser — all privileges |
| 1 | daemon | Background daemons without specific user |
| 2 | network | Network daemon group |
| 100+ | user accounts | Regular users |

## `sys_getuid` / `sys_setuid`

```c
uid_t sys_getuid(void);    // returns cred.uid
uid_t sys_geteuid(void);   // returns cred.euid
int sys_setuid(uid_t uid); // if euid==0: sets uid,euid,suid; else: sets euid (to suid or uid)
int sys_setresuid(uid_t ruid, uid_t euid, uid_t suid); // full credential control
```

`sys_setuid(0)` (become root) requires `euid == 0` or `CAP_SETUID`.

## Related Documents

- [capabilities.md](capabilities.md)
- [syscall-filtering.md](syscall-filtering.md)
- [security/overview.md](overview.md)
- [syscalls/security-syscalls.md](../syscalls/security-syscalls.md)
