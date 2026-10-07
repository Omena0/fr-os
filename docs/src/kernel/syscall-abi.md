# Syscall ABI

## Overview

The syscall ABI is the stable contract between userspace and the kernel[^sysv-abi]. See [architecture/abi-stability.md](../architecture/abi-stability.md) for versioning policy.

## Syscall Table

Syscalls are dispatched by number. The table is a statically allocated array of function pointers, indexed by syscall number. The table is read-only after boot.

### Process Management (0–31)

| Number | Name | Description |
|---|---|---|
| 0 | `sys_fork` | Fork current process (COW) |
| 1 | `sys_exec` | Replace current image with new executable |
| 2 | `sys_exit` | Terminate current process |
| 3 | `sys_wait` | Wait for child process state change |
| 4 | `sys_getpid` | Get process ID |
| 5 | `sys_getppid` | Get parent process ID |
| 6 | `sys_clone` | Create thread or process with flags |
| 7 | `sys_kill` | Send signal to process |
| 8 | `sys_sigaction` | Set signal handler |
| 9 | `sys_sigprocmask` | Block/unblock signals |
| 10 | `sys_sigreturn` | Return from signal handler |
| 11 | `sys_getuid` / `sys_setuid` | User ID management |
| 12 | `sys_getgid` / `sys_setgid` | Group ID management |
| 13 | `sys_setrlimit` / `sys_getrlimit` | Resource limits |
| 14 | `sys_sched_yield` | Voluntarily yield CPU |
| 15 | `sys_sched_setaffinity` | Set CPU affinity |
| 16 | `sys_sched_getparam` | Get scheduler parameters |
| 17 | `sys_sched_setparam` | Set scheduler parameters (RT priority) |

### Memory Management (32–63)

| Number | Name | Description |
|---|---|---|
| 32 | `sys_mmap` | Map memory (anonymous or file-backed) |
| 33 | `sys_munmap` | Unmap memory region |
| 34 | `sys_mprotect` | Change page protection |
| 35 | `sys_brk` | Extend/shrink heap break |
| 36 | `sys_madvise` | Memory usage hints |
| 37 | `sys_mlock` | Lock pages in RAM |
| 38 | `sys_munlock` | Unlock pages |

### File System (64–127)

| Number | Name | Description |
|---|---|---|
| 64 | `sys_open` | Open file |
| 65 | `sys_close` | Close file descriptor |
| 66 | `sys_read` | Read from file descriptor |
| 67 | `sys_write` | Write to file descriptor |
| 68 | `sys_lseek` | Seek in file |
| 69 | `sys_stat` | Get file status |
| 70 | `sys_fstat` | Get file status by FD |
| 71 | `sys_mkdir` | Create directory |
| 72 | `sys_rmdir` | Remove directory |
| 73 | `sys_unlink` | Remove file |
| 74 | `sys_rename` | Rename file |
| 75 | `sys_link` | Create hard link |
| 76 | `sys_symlink` | Create symbolic link |
| 77 | `sys_readlink` | Read symbolic link target |
| 78 | `sys_chmod` | Change file permissions |
| 79 | `sys_chown` | Change file owner |
| 80 | `sys_mount` | Mount filesystem |
| 81 | `sys_umount` | Unmount filesystem |
| 82 | `sys_getdents` | Read directory entries |
| 83 | `sys_dup` / `sys_dup2` | Duplicate file descriptor |
| 84 | `sys_fcntl` | File control |
| 85 | `sys_ioctl` | Device control |
| 86 | `sys_select` | I/O multiplexing |
| 87 | `sys_poll` | I/O multiplexing |
| 88 | `sys_epoll_create` | Create epoll instance |
| 89 | `sys_epoll_ctl` | Modify epoll |
| 90 | `sys_epoll_wait` | Wait for epoll events |
| 91 | `sys_pipe` | Create pipe |

### IPC (128–159)

| Number | Name | Description |
|---|---|---|
| 128 | `sys_shm_open` | Open shared memory object |
| 129 | `sys_shm_unlink` | Remove shared memory object |
| 130 | `sys_sem_open` | Open semaphore |
| 131 | `sys_sem_post` / `sys_sem_wait` | Semaphore operations |
| 132 | `sys_futex` | Fast userspace mutex |

### Sockets (160–199)

| Number | Name | Description |
|---|---|---|
| 160 | `sys_socket` | Create socket |
| 161 | `sys_bind` | Bind socket to address |
| 162 | `sys_connect` | Connect socket |
| 163 | `sys_listen` | Listen for connections |
| 164 | `sys_accept` | Accept connection |
| 165 | `sys_send` / `sys_sendto` | Send data |
| 166 | `sys_recv` / `sys_recvfrom` | Receive data |
| 167 | `sys_setsockopt` / `sys_getsockopt` | Socket options |
| 168 | `sys_shutdown` | Shutdown socket |

### Security (200–219)

| Number | Name | Description |
|---|---|---|
| 200 | `sys_cap_get` | Get capability set |
| 201 | `sys_cap_drop` | Drop capability |
| 202 | `sys_seccomp` | Install syscall filter |
| 203 | `sys_unshare` | Create new namespace |
| 204 | `sys_setns` | Enter namespace |

## Dispatch Path

See [syscalls/dispatch-path.md](../syscalls/dispatch-path.md) for the detailed fast-path implementation.

## Related Documents

- [architecture/abi-stability.md](../architecture/abi-stability.md)
- [syscalls/overview.md](../syscalls/overview.md)
- [privilege-levels.md](privilege-levels.md)

## References

- [System V Application Binary Interface AMD64 Architecture Processor Supplement][sysv-abi]
- [Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A — System Call and SYSENTER/SYSEXIT][intel-sdm-syscall]

[sysv-abi]: https://gitlab.com/x86-psABIs/x86-64-ABI/-/blob/master/abi.md "System V AMD64 ABI"
[intel-sdm-syscall]: https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html#vol3a "Intel SDM Volume 3A: System Call and SYSENTER/SYSEXIT"
