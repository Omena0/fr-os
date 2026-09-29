# Memory Syscalls

## Syscall Table: Memory Management (Numbers 32–38)

| Number | Name | Signature | Description |
|---|---|---|---|
| 32 | `sys_mmap` | `void *sys_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset)` | Map memory or file into address space |
| 33 | `sys_munmap` | `int sys_munmap(void *addr, size_t len)` | Remove mapping |
| 34 | `sys_mprotect` | `int sys_mprotect(void *addr, size_t len, int prot)` | Change protection flags on a mapping |
| 35 | `sys_mremap` | `void *sys_mremap(void *old_addr, size_t old_size, size_t new_size, int flags)` | Resize or move a mapping |
| 36 | `sys_madvise` | `int sys_madvise(void *addr, size_t len, int advice)` | Hint to the kernel about intended memory use |
| 37 | `sys_brk` | `void *sys_brk(void *new_brk)` | Extend or shrink the heap (data segment) |
| 38 | `sys_mlock` | `int sys_mlock(void *addr, size_t len)` | Lock pages in memory (prevent swap) |

## `sys_mmap` Flags

| `flags` | Description |
|---|---|
| `MAP_PRIVATE` | Copy-on-write; changes not visible to other mappings |
| `MAP_SHARED` | Changes visible to other processes mapping the same object |
| `MAP_ANONYMOUS` | Not backed by a file (ignore `fd`) |
| `MAP_FIXED` | Map at exactly `addr` (fail if cannot) |
| `MAP_HUGETLB` | Use 2 MB huge pages |
| `MAP_GROWSDOWN` | For stack mappings (auto-expand downward) |
| `MAP_POPULATE` | Pre-fault pages (load immediately) |

## `sys_mmap` Protection Flags (`prot`)

| `prot` | Page table bits |
|---|---|
| `PROT_NONE` | No access (P=0) |
| `PROT_READ` | Read only (U/S=user, W=0) |
| `PROT_WRITE` | Read+write (U/S=user, W=1) |
| `PROT_EXEC` | Read+execute (NX=0) |

## `sys_madvise` Advice Values

| Advice | Effect |
|---|---|
| `MADV_NORMAL` | Default behavior |
| `MADV_SEQUENTIAL` | Enable aggressive read-ahead |
| `MADV_RANDOM` | Disable read-ahead |
| `MADV_WILLNEED` | Pre-fault the region now |
| `MADV_DONTNEED` | Kernel may evict these pages |
| `MADV_FREE` | Pages can be reclaimed but will be zeroed on next access |
| `MADV_HUGEPAGE` | Enable transparent huge pages for this region |
| `MADV_NOHUGEPAGE` | Disable THP for this region |

## `sys_brk` Implementation

1. If `new_brk == 0`: return current `brk` (heap end).
2. If `new_brk > current_brk` (expand): round up to page boundary. Check against `RLIMIT_DATA` and overcommit policy. Map anonymous pages from current `brk` to `new_brk` (`PROT_READ | PROT_WRITE | MAP_ANONYMOUS | MAP_PRIVATE`).
3. If `new_brk < current_brk` (shrink): unmap pages from `new_brk` to old `brk`.
4. Update `current->mm->brk`. Return new `brk`.

## `sys_mlock` Requirements

Locking pages in memory (`mlock`) prevents them from being swapped out:
- Required by real-time processes, cryptographic key storage, and DMA buffers.
- Requires `CAP_IPC_LOCK` or a non-zero `RLIMIT_MEMLOCK` allowance.
- System-wide locked page limit: enforced to prevent DoS (one process locking all physical memory).

## Argument Validation

All memory syscall arguments are validated:
- `addr`: must be page-aligned (or rounded down for mmap).
- `len`: must be > 0 and < a reasonable maximum (e.g., `SIZE_MAX / 2`).
- `prot`: only known bits may be set.
- `flags`: only known bits may be set; incompatible combinations rejected (`MAP_SHARED | MAP_PRIVATE`).
- `fd + offset`: for file-backed mappings, `fd` must be a valid open file with appropriate permissions.

## Related Documents

- [process-syscalls.md](process-syscalls.md)
- [file-syscalls.md](file-syscalls.md)
- [memory/virtual-memory.md](../memory/virtual-memory.md)
- [memory/userspace-malloc.md](../memory/userspace-malloc.md)
