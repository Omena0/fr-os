# Filesystem Overview

## Architecture

The filesystem subsystem has three layers:

```
Applications (syscalls: open, read, write, stat, ...)
        ↓
VFS (Virtual File System) — filesystem-independent abstraction
        ↓
Filesystem drivers (ext4, tmpfs, procfs, ...)
        ↓
Block device layer (bio queues, caching)
        ↓
Storage drivers (virtio-blk, IDE, NVMe ...)
```

This layered model allows multiple filesystem types to coexist under a single unified interface.

## VFS

The VFS is a set of abstract data types and interfaces that every filesystem driver must implement. Userspace never interacts with filesystem drivers directly — all file operations go through VFS.

Key VFS abstractions:

- **Inode**: Represents a file system object (file, directory, symlink, device). Contains metadata (permissions, timestamps, size) but not the filename.
- **Dentry**: Directory entry. Associates a name with an inode. Dentries form the directory tree.
- **File**: An open file descriptor. References a dentry + current offset + flags.
- **Superblock**: Represents a mounted filesystem. Contains filesystem-wide metadata and a pointer to the filesystem driver's operations.

See [vfs-layer.md](vfs-layer.md) and [vfs-api.md](vfs-api.md).

## ext4

The primary on-disk filesystem is a subset of ext4:

- Journaling (metadata journal, data=ordered mode by default).
- Extents-based block mapping (not legacy indirect blocks).
- Large file support.
- Directory indexing via HTree (for large directories).

See [ext4-overview.md](ext4-overview.md), [ext4-journaling.md](ext4-journaling.md), [ext4-extents.md](ext4-extents.md).

## Caches

Three distinct caches reduce disk I/O:

| Cache | Contents | Indexed by |
|---|---|---|
| Page cache | File data (4 KB pages) | (inode, page_offset) |
| Inode cache | `struct inode` objects | (superblock, inode number) |
| Dentry cache | `struct dentry` objects | (parent dentry, filename) |

All caches are reclaim candidates under memory pressure. See [page-cache.md](page-cache.md), [inode-cache.md](inode-cache.md), [dentry-cache.md](dentry-cache.md).

## Mount System

The VFS supports a hierarchical mount tree rooted at `/`. Any directory can be a mount point. Multiple filesystems are visible simultaneously under a single namespace. See [mount-system.md](mount-system.md).

## File Descriptor Abstraction

All open files are represented as `struct file` objects referenced by integer file descriptors in the process's file descriptor table. This abstraction is uniform across all filesystem types, pipes, sockets, and device files.

## Performance

Fast path (sequential read, page cache hit):

1. `read` syscall → VFS `file_read` → page cache lookup → `copy_to_user`.
2. No disk I/O. Latency dominated by `copy_to_user`.
3. No locks beyond the page cache entry lock (brief).

## Related Documents

- [vfs-layer.md](vfs-layer.md)
- [vfs-api.md](vfs-api.md)
- [ext4-overview.md](ext4-overview.md)
- [ext4-journaling.md](ext4-journaling.md)
- [ext4-extents.md](ext4-extents.md)
- [page-cache.md](page-cache.md)
- [inode-cache.md](inode-cache.md)
- [dentry-cache.md](dentry-cache.md)
- [mount-system.md](mount-system.md)
