# VFS Layer

## Overview

The Virtual File System (VFS) is a kernel subsystem that provides a unified file system interface to userspace, regardless of the underlying filesystem type. All filesystem syscalls (`open`, `read`, `write`, `stat`, `mkdir`, etc.) are dispatched through VFS.

## Core Objects

### Superblock (`struct super_block`)

One superblock per mounted filesystem:

```c
struct super_block {
    dev_t           s_dev;          // device identifier
    uint64_t        s_blocksize;    // filesystem block size in bytes
    uint64_t        s_magic;        // filesystem type magic number
    struct inode   *s_root_inode;   // root directory inode
    struct fs_ops  *s_ops;          // filesystem operations (mount, sync, unmount)
    struct list_head s_inodes;      // all inodes belonging to this superblock
    void           *s_private;      // filesystem-specific data (e.g., ext4 sb)
    struct mount   *s_mount;        // mount point entry
};
```

### Inode (`struct inode`)

One inode per file system object (file, directory, symlink, device node):

```c
struct inode {
    uint64_t        i_ino;          // inode number
    uint32_t        i_mode;         // file type and permissions (S_IFREG, etc.)
    uint32_t        i_uid, i_gid;   // owner user and group
    uint64_t        i_size;         // file size in bytes
    uint64_t        i_blocks;       // number of 512-byte blocks allocated
    struct timespec i_atime, i_mtime, i_ctime; // access/modification/change times
    atomic_t        i_refcount;
    struct inode_ops *i_ops;        // inode operations (lookup, create, link, ...)
    struct file_ops  *i_fops;       // file operations for open files on this inode
    struct address_space *i_mapping;// page cache for this inode's data
    void            *i_private;     // filesystem-specific inode data
    struct super_block *i_sb;
    spinlock_t      i_lock;
};
```

### Dentry (`struct dentry`)

Maps a filename to an inode within a directory:

```c
struct dentry {
    struct dentry  *d_parent;       // parent directory dentry
    struct qstr     d_name;         // filename (quick string with hash)
    struct inode   *d_inode;        // NULL if negative (non-existent) dentry
    struct list_head d_child;       // siblings in parent
    struct list_head d_subdirs;     // children (if this is a directory)
    struct hlist_node d_hash;       // dentry cache hash table node
    atomic_t        d_refcount;
    uint32_t        d_flags;        // DCACHE_NEGATIVE, DCACHE_MOUNTED, etc.
    struct dentry_ops *d_ops;
    struct super_block *d_sb;
};
```

### File (`struct file`)

Represents an open file descriptor:

```c
struct file {
    struct dentry  *f_dentry;       // what file is open
    struct file_ops *f_ops;         // operations for this open file
    uint64_t        f_pos;          // current file offset
    uint32_t        f_flags;        // O_RDONLY, O_WRONLY, O_NONBLOCK, etc.
    atomic_t        f_refcount;     // reference count (for dup, fork)
    void           *f_private;      // filesystem-specific per-open-file data
};
```

## Operations Interfaces

Each filesystem driver implements three operation tables:

### `struct inode_ops`

```c
struct inode_ops {
    struct dentry *(*lookup)(struct inode *dir, const char *name);
    int (*create)(struct inode *dir, const char *name, uint32_t mode);
    int (*link)(struct dentry *old, struct inode *dir, const char *name);
    int (*unlink)(struct inode *dir, const char *name);
    int (*symlink)(struct inode *dir, const char *name, const char *target);
    int (*mkdir)(struct inode *dir, const char *name, uint32_t mode);
    int (*rmdir)(struct inode *dir, const char *name);
    int (*rename)(struct inode *old_dir, const char *old_name,
                  struct inode *new_dir, const char *new_name);
    int (*getattr)(struct inode *inode, struct stat *stat);
    int (*setattr)(struct inode *inode, struct iattr *attr);
};
```

### `struct file_ops`

```c
struct file_ops {
    ssize_t (*read)(struct file *f, void __user *buf, size_t len, off_t *offset);
    ssize_t (*write)(struct file *f, const void __user *buf, size_t len, off_t *offset);
    int     (*open)(struct inode *inode, struct file *f);
    int     (*release)(struct inode *inode, struct file *f);
    off_t   (*lseek)(struct file *f, off_t offset, int whence);
    int     (*fsync)(struct file *f);
    int     (*mmap)(struct file *f, struct vma *vma);
    int     (*poll)(struct file *f, struct poll_table *pt);
    long    (*ioctl)(struct file *f, unsigned int cmd, unsigned long arg);
    int     (*readdir)(struct file *f, struct dirent __user *dirp, int count);
};
```

### `struct fs_ops`

```c
struct fs_ops {
    int  (*mount)(struct super_block *sb, const char *dev, uint32_t flags);
    int  (*unmount)(struct super_block *sb);
    int  (*sync)(struct super_block *sb);
    struct inode *(*alloc_inode)(struct super_block *sb);
    void (*free_inode)(struct inode *inode);
    int  (*write_inode)(struct inode *inode);
};
```

## Filesystem Registration

Filesystem drivers register with VFS at startup (or module load time):

```c
struct filesystem_type {
    const char *name;          // e.g., "ext4", "tmpfs"
    struct fs_ops *ops;
    struct list_head list;     // linked list of registered filesystem types
};

int vfs_register_filesystem(struct filesystem_type *fs);
int vfs_unregister_filesystem(struct filesystem_type *fs);
```

`sys_mount` looks up the filesystem type by name and calls its `mount` operation.

## Related Documents

- [vfs-api.md](vfs-api.md)
- [dentry-cache.md](dentry-cache.md)
- [inode-cache.md](inode-cache.md)
- [page-cache.md](page-cache.md)
- [mount-system.md](mount-system.md)
