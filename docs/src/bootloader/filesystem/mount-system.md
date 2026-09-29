# Mount System

## Overview

The VFS mount system maintains a hierarchical tree of mounted filesystems. All filesystems visible under a single namespace share one mount tree, rooted at `/`.

## Mount Tree

```c
struct mount {
    struct mount     *mnt_parent;     // parent mount (NULL for root)
    struct dentry    *mnt_mountpoint; // dentry in parent where this is mounted
    struct dentry    *mnt_root;       // root dentry of this filesystem
    struct super_block *mnt_sb;       // superblock of the mounted filesystem
    struct list_head  mnt_mounts;     // list of child mounts
    struct list_head  mnt_child;      // link in parent's mnt_mounts list
    uint32_t          mnt_flags;      // MS_RDONLY, MS_NOSUID, MS_NODEV, etc.
    char              mnt_devname[64];// device name string (e.g., "/dev/sda1")
    char              mnt_type[32];   // filesystem type string (e.g., "ext4")
};
```

## `sys_mount` Syscall

```c
int sys_mount(const char *source, const char *target, const char *fstype,
              unsigned long flags, const void *data);
```

**Steps**:

1. Look up `target` via path resolution → find the mount point dentry.
2. Look up `fstype` in the registered filesystem type list.
3. Allocate a new `struct super_block`. Call `fstype->ops->mount(sb, source, flags)`.
   - The filesystem driver reads the disk superblock, validates magic, reads the root inode.
4. Allocate a `struct mount`. Link it to the target dentry in the parent mount.
5. Mark the target dentry as `DCACHE_MOUNTED`.
6. Register the mount in the namespace's mount list.

Requires `CAP_SYS_ADMIN`.

## `sys_umount` Syscall

```c
int sys_umount(const char *target, int flags);
```

**Steps**:

1. Find the mount associated with `target`.
2. Check that no files are open on this filesystem (busy check). If busy and `MNT_FORCE` not set: return `EBUSY`.
3. Sync all dirty pages (writeback for this superblock's inodes and pages).
4. Unlink the mount from the parent mount's child list.
5. Clear `DCACHE_MOUNTED` on the mount point dentry.
6. Call `superblock->s_ops->unmount(sb)`. Release superblock resources.

## Mount Flags

| Flag | Effect |
|---|---|
| `MS_RDONLY` | Mount read-only; all writes return `EROFS` |
| `MS_NOSUID` | Ignore setuid/setgid bits on executables |
| `MS_NODEV` | Disallow device files on this filesystem |
| `MS_NOEXEC` | Disallow execution of files on this filesystem |
| `MS_BIND` | Bind-mount: expose a subtree at another point |
| `MS_REMOUNT` | Change mount flags without remounting |
| `MS_SYNCHRONOUS` | All writes are synchronous (no writeback delay) |

## Bind Mounts

A bind mount makes a directory subtree visible at another point in the filesystem tree without copying data:

```bash
mount --bind /home/user/data /mnt/data
```

Internally: a new `struct mount` is created pointing to the same `struct super_block` and root dentry as the source, mounted at the target. Both mount points see the same underlying inode and page cache data.

## Initial Mount (Root Filesystem)

The root filesystem is mounted by the kernel during early boot (`mount_root`):

1. The boot device is determined from the kernel command line (`root=/dev/vda`).
2. ext4 is tried as the filesystem type.
3. The root mount is created with `mnt_parent = self` (root of the tree).
4. All subsequent mounts attach as children.

## Special Filesystems

| Mount | Type | Description |
|---|---|---|
| `/proc` | procfs | Process information (read-only, synthetic) |
| `/dev` | devfs | Device nodes (managed by the driver framework) |
| `/tmp` | tmpfs | RAM-backed temporary storage |
| `/sys` | sysfs | Planned (kernel object hierarchy) |

## Related Documents

- [vfs-layer.md](vfs-layer.md)
- [vfs-api.md](vfs-api.md)
- [ipc/namespace-isolation.md](../ipc/namespace-isolation.md)
- [security/namespaces.md](../security/namespaces.md)
