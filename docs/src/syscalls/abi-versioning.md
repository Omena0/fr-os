# Syscall ABI Versioning

## Overview

The syscall ABI is versioned to allow the kernel and userspace to evolve independently. New syscalls may be added in minor versions; syscalls may be deprecated (but not removed) in major versions.

## Version Scheme

`KERNEL_ABI_VERSION = MAJOR * 1000 + MINOR`

- **MAJOR** increments when a syscall's semantics change incompatibly (rare).
- **MINOR** increments when new syscalls are added or arguments are extended.

Current version: `1.0` (ABI_VERSION = 1000).

## Discovery at execve

The kernel provides the ABI version in the ELF auxiliary vector at process startup:

```c
// In the auxiliary vector (from dl_info / _dl_aux_vector):
AT_ABI_VERSION = 1000    // MAJOR=1, MINOR=0
```

Userspace programs can read this via `getauxval(AT_ABI_VERSION)`:
```c
uint64_t abi_ver = getauxval(AT_ABI_VERSION);
if (abi_ver < REQUIRED_ABI_VERSION) {
    fprintf(stderr, "kernel too old\n");
    exit(1);
}
```

## Stability Guarantees

| Category | Guarantee |
|---|---|
| Syscall numbers | Never change after initial assignment |
| Syscall semantics (same MAJOR) | Fully backward-compatible |
| Syscall semantics (new MAJOR) | May change; compat dispatch table handles old callers |
| New syscalls | Added at new numbers; old programs unaffected |
| Return values | Always ≥0 on success, -errno on failure |
| Argument types | Never narrowed (may be widened with new flags) |

## Compat Dispatch

For the (rare) case of a MAJOR version bump that changes a syscall:
1. The old syscall number is retained but mapped to a compat wrapper.
2. The compat wrapper translates old-format arguments to the new internal format.
3. New programs use the new syscall number (allocated separately).

Example:
```c
// Old: sys_old_mmap (number 90) takes struct mmap_arg_struct *
// New: sys_mmap (number 32) takes individual arguments
// Compat wrapper:
long sys_old_mmap(struct mmap_arg_struct __user *arg) {
    struct mmap_arg_struct a;
    copy_from_user(&a, arg, sizeof(a));
    return sys_mmap(a.addr, a.len, a.prot, a.flags, a.fd, a.offset);
}
```

## Extended Flags Pattern

New functionality is added via new flag bits in existing syscalls where possible, to avoid allocating new syscall numbers:

```c
// Original: sys_open(path, flags, mode)
// Extended: O_CLOEXEC, O_NONBLOCK, O_TMPFILE added as new flag bits
// No new syscall needed; old code passes flags=0 and still works
```

When a new flag bit is passed to an old kernel that does not support it: the old kernel returns `-EINVAL`. Userspace can use this to detect feature support.

## VDSO Version

The VDSO (Virtual Dynamic Shared Object) also carries a version number. The VDSO provides fast userspace implementations of `clock_gettime`, `gettimeofday`, and `getpid` that avoid a full syscall. The VDSO version is tied to the kernel version.

## Related Documents

- [dispatch-path.md](dispatch-path.md)
- [syscalls/overview.md](overview.md)
- [architecture/abi-stability.md](../architecture/abi-stability.md)
