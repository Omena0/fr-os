# Overcommit Policy

## Overview

Memory overcommit is the practice of allowing processes to be allocated more virtual memory than the system has physical memory to back it. Overcommit is possible because most allocated virtual memory is never all accessed at the same time. It enables more efficient resource utilization at the cost of the possibility of OOM (Out-Of-Memory) conditions if all allocated memory is actually used.

## Overcommit Modes

Three overcommit modes are supported, configurable at boot via kernel parameter `vm.overcommit_memory`:

### Mode 0 — Heuristic Overcommit (Default)

The kernel uses a heuristic to decide whether to allow an allocation:

```
allowed = total_physical_memory + total_swap_space - currently_committed
```

If `requested_size > allowed * OVERCOMMIT_RATIO` (default ratio: 50%): reject with `ENOMEM`. Otherwise: allow.

This mode allows reasonable overcommit while rejecting obviously unreasonable allocations (e.g., a single process requesting 10× physical RAM).

### Mode 1 — Always Overcommit

All `mmap` and `brk` requests succeed regardless of available physical memory. Applications that use virtual address space speculatively (allocate a large range but only access a small part) can use this mode to avoid allocation failures. The risk of OOM is maximized.

### Mode 2 — Never Overcommit (Strict)

Allocations are refused if they would cause committed memory to exceed:

```
max_committed = (total_physical_memory + total_swap_space) * OVERCOMMIT_LIMIT
```

`OVERCOMMIT_LIMIT` default: 0.95 (95% of physical + swap). In this mode, `malloc` in userspace may return `NULL` for large allocations even when memory is not critically low.

## Commit Accounting

The kernel tracks committed virtual memory (sum of all `mmap` and `brk` allocations that are not backed by a file). This is distinct from physical memory usage:

- `committed_as`: Total committed virtual memory across all processes.
- `physical_used`: Resident physical pages.
- `committed_as` ≥ `physical_used` (committed can exceed physical by the overcommit amount).

## OOM Interaction

Regardless of overcommit mode, if physical memory is fully exhausted and the page reclamation subsystem cannot free enough pages, the OOM killer is invoked. Overcommit mode determines how likely OOM is to occur:

- Mode 0: OOM is possible but less likely.
- Mode 1: OOM is most likely on systems where allocations are large but sparse.
- Mode 2: OOM is least likely (allocations are refused before exhaustion).

## Configuration at Runtime

The overcommit policy can be changed at runtime via the kernel configuration interface (a `/proc/sys/vm/overcommit_memory`-equivalent path):

```
echo 0 > /proc/sys/vm/overcommit_memory   # heuristic
echo 1 > /proc/sys/vm/overcommit_memory   # always
echo 2 > /proc/sys/vm/overcommit_memory   # never
```

Requires `CAP_SYS_ADMIN`.

## Related Documents

- [page-reclamation.md](page-reclamation.md)
- [virtual-memory.md](virtual-memory.md)
- [userspace-malloc.md](userspace-malloc.md)
- [security/overview.md](../security/overview.md)
