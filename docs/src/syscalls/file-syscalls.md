# File Syscalls

## Syscall Table: Filesystem (Numbers 64–91)

| Number | Name | Signature | Description |
|---|---|---|---|
| 64 | `sys_open` | `int sys_open(const char *path, int flags, mode_t mode)` | Open or create a file |
| 65 | `sys_openat` | `int sys_openat(int dirfd, const char *path, int flags, mode_t mode)` | Open relative to `dirfd` |
| 66 | `sys_close` | `int sys_close(int fd)` | Close a file descriptor |
| 67 | `sys_read` | `ssize_t sys_read(int fd, void *buf, size_t len)` | Read from fd |
| 68 | `sys_write` | `ssize_t sys_write(int fd, const void *buf, size_t len)` | Write to fd |
| 69 | `sys_lseek` | `off_t sys_lseek(int fd, off_t offset, int whence)` | Change file offset |
| 70 | `sys_pread` | `ssize_t sys_pread(int fd, void *buf, size_t len, off_t offset)` | Read at offset (no f_pos change) |
| 71 | `sys_pwrite` | `ssize_t sys_pwrite(int fd, const void *buf, size_t len, off_t offset)` | Write at offset |
| 72 | `sys_readv` | `ssize_t sys_readv(int fd, const struct iovec *iov, int iovcnt)` | Scatter read |
| 73 | `sys_writev` | `ssize_t sys_writev(int fd, const struct iovec *iov, int iovcnt)` | Gather write |
| 74 | `sys_stat` | `int sys_stat(const char *path, struct stat *buf)` | Get file metadata |
| 75 | `sys_fstat` | `int sys_fstat(int fd, struct stat *buf)` | Get metadata by fd |
| 76 | `sys_lstat` | `int sys_lstat(const char *path, struct stat *buf)` | stat without following symlinks |
| 77 | `sys_access` | `int sys_access(const char *path, int mode)` | Check file access permission |
| 78 | `sys_mkdir` | `int sys_mkdir(const char *path, mode_t mode)` | Create directory |
| 79 | `sys_rmdir` | `int sys_rmdir(const char *path)` | Remove directory |
| 80 | `sys_unlink` | `int sys_unlink(const char *path)` | Remove file |
| 81 | `sys_rename` | `int sys_rename(const char *oldpath, const char *newpath)` | Rename file |
| 82 | `sys_link` | `int sys_link(const char *oldpath, const char *newpath)` | Create hard link |
| 83 | `sys_symlink` | `int sys_symlink(const char *target, const char *path)` | Create symbolic link |
| 84 | `sys_readlink` | `ssize_t sys_readlink(const char *path, char *buf, size_t len)` | Read symlink target |
| 85 | `sys_chmod` | `int sys_chmod(const char *path, mode_t mode)` | Change file permissions |
| 86 | `sys_chown` | `int sys_chown(const char *path, uid_t uid, gid_t gid)` | Change file owner |
| 87 | `sys_fcntl` | `int sys_fcntl(int fd, int cmd, ...)` | File control operations |
| 88 | `sys_ioctl` | `long sys_ioctl(int fd, unsigned int cmd, unsigned long arg)` | Device control |
| 89 | `sys_fsync` | `int sys_fsync(int fd)` | Flush file data and metadata to disk |
| 90 | `sys_truncate` | `int sys_truncate(const char *path, off_t length)` | Truncate file to `length` |
| 91 | `sys_mount` | `int sys_mount(const char *src, const char *tgt, const char *type, uint64_t flags, const void *data)` | Mount filesystem |

## `sys_open` Flags

| Flag | Effect |
|---|---|
| `O_RDONLY` / `O_WRONLY` / `O_RDWR` | Open mode |
| `O_CREAT` | Create if not exists (requires `mode`) |
| `O_EXCL` | With `O_CREAT`: fail if exists |
| `O_TRUNC` | Truncate to zero on open |
| `O_APPEND` | All writes go to end of file |
| `O_NONBLOCK` | Non-blocking I/O |
| `O_CLOEXEC` | Set close-on-exec flag |
| `O_SYNC` | Each write is synced to disk before returning |
| `O_DIRECTORY` | Fail if not a directory |

## `sys_fcntl` Commands

| `cmd` | Description |
|---|---|
| `F_DUPFD` | Duplicate fd to next available ≥ arg |
| `F_GETFD` / `F_SETFD` | Get/set `FD_CLOEXEC` flag |
| `F_GETFL` / `F_SETFL` | Get/set file status flags (`O_NONBLOCK`, etc.) |
| `F_GETLK` / `F_SETLK` / `F_SETLKW` | POSIX advisory file locking |

## Common Error Returns

| Errno | Meaning |
|---|---|
| `ENOENT` | Path component does not exist |
| `EACCES` | Permission denied |
| `EEXIST` | File already exists (`O_CREAT | O_EXCL`) |
| `EISDIR` | Is a directory (cannot open directory with `O_WRONLY`) |
| `ENOTDIR` | Path component is not a directory |
| `EBADF` | Invalid fd |
| `EMFILE` | Per-process fd limit reached |
| `ENFILE` | System-wide fd limit reached |
| `ENOSPC` | No space left on device |
| `EIO` | I/O error (disk failure) |

## Related Documents

- [overview.md](overview.md)
- [dispatch-path.md](dispatch-path.md)
- [filesystem/vfs-api.md](../filesystem/vfs-api.md)
- [ipc/fd-abstraction.md](../ipc/fd-abstraction.md)
