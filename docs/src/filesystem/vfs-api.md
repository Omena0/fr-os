# VFS API

## Path Resolution

All file syscalls that take a path begin with VFS path resolution (`vfs_path_lookup`):

1. Start at the current directory (for relative paths) or the filesystem root (for absolute paths starting with `/`).
2. For each component in the path: look up the name in the dentry cache. If found: use the cached dentry. If not: call the parent directory's `inode_ops.lookup()` to load from disk, then cache the result.
3. Handle symlinks (up to `SYMLINK_MAX_DEPTH = 40` levels to prevent loops).
4. Handle mount point traversal: if a dentry is a mount point, switch to the mounted filesystem's root.
5. Return the final `struct dentry`.

Path resolution is protected by RCU on the read path (dentry cache lookups). Writes to the dentry tree require the dcache lock.

## `open` Path

`sys_open(path, flags, mode)`:

1. Path resolution → `struct dentry`.
2. If `O_CREAT` and file not found: call `inode_ops.create()`.
3. Permission check (uid/gid + mode bits + capabilities).
4. Call `file_ops.open()` (filesystem-specific open logic).
5. Allocate `struct file`, set `f_flags`, `f_pos = 0`, set `f_ops` from inode.
6. Install `struct file` in the process's file descriptor table.
7. Return the file descriptor integer.

## `read` / `write` Path

`sys_read(fd, buf, len)`:

1. Look up `struct file *` from the fd table.
2. Permission check (`O_RDONLY` or `O_RDWR`).
3. Call `file_ops.read(file, buf, len, &file->f_pos)`.
4. For page-cached files: locate the page cache for `(inode, offset/PAGE_SIZE)`. Copy data from cache page to `buf` via `copy_to_user`. If page not in cache: trigger read-ahead.
5. Update `f_pos`.

`sys_write(fd, buf, len)`:

1. Look up `struct file *`.
2. Call `file_ops.write(file, buf, len, &file->f_pos)`.
3. For page-cached files: write into the page cache page (marking it dirty). Writeback is asynchronous (triggered periodically or on `fsync`).
4. Update `f_pos`, update inode `i_mtime`.

## `stat` Path

`sys_stat(path, statbuf)`:

1. Path resolution → `struct inode`.
2. Call `inode_ops.getattr(inode, &stat)`.
3. `copy_to_user(statbuf, &stat, sizeof(stat))`.

## `mmap` on Files

`sys_mmap(..., fd, offset)` for file-backed mappings:

1. Look up `struct file *`.
2. Call `file_ops.mmap(file, vma)` — filesystem sets the VMA's fault handler to `page_cache_fault`.
3. On page fault: the fault handler reads the page from the page cache (or disk) and maps it into the process page table.
4. `MAP_SHARED`: writes go directly to the page cache (dirty bit set). `MAP_PRIVATE`: COW copy on write.

## `ioctl` Path

`sys_ioctl(fd, cmd, arg)`:

1. Look up `struct file *`.
2. Call `file_ops.ioctl(file, cmd, arg)`.
3. The filesystem or device driver handles the command. Returns 0 on success, negative errno on error.

## `poll` / `epoll`

`sys_poll(fds, nfds, timeout)`:

1. For each fd: call `file_ops.poll(file, poll_table)`.
2. The poll operation adds the current thread to the file's wait queue (for wakeup on event).
3. Returns a bitmask of ready events (POLLIN, POLLOUT, POLLERR).
4. If no events: sleep until timeout or any file becomes ready.

`epoll` is an efficient O(1) event notification mechanism for large fd sets. It maintains a persistent interest list and delivers events only for ready fds. Backed by an rbtree for fd tracking and a doubly-linked list for ready events.

## Related Documents

- [vfs-layer.md](vfs-layer.md)
- [page-cache.md](page-cache.md)
- [dentry-cache.md](dentry-cache.md)
- [ipc/fd-abstraction.md](../ipc/fd-abstraction.md)
- [syscalls/file-syscalls.md](../syscalls/file-syscalls.md)
