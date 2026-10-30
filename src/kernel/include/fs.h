/*
 * fs.h - the small VFS inode layer the syscalls use.
 *
 * One mounted filesystem (the root) today. The interfaces are shaped the way
 * docs/src/filesystem/vfs-layer.md describes so a second filesystem driver can
 * be added without changing the syscall code: an inode is metadata plus a set
 * of operations, and a mounted superblock names the block device and the root
 * inode.
 */
#ifndef FS_H
#define FS_H

#include <types.h>
#include <process.h>

#define FS_IFMT  0170000
#define FS_IFDIR 0040000
#define FS_IFREG 0100000

struct fs_inode {
    u32 ino;
    u32 mode;
    u32 nlink;
    u64 size;
    u64 blocks;          /* allocated 512-byte blocks */
    u32 direct[12];
    u32 indirect;
};

struct fs_super {
    struct block_device *dev;
    u32 block_size;
    u32 total_blocks;
    u32 inode_count;
    u32 inode_size;
    u32 inode_table_block;
    u32 data_block_start;
    u32 root_ino;
};

/* Mount the first block device that carries a FrFS superblock. */
int fs_mount_root(void);
bool fs_mounted(void);

int fs_read_inode(u32 ino, struct fs_inode *out);

/* Resolve a path (absolute or relative to the root). Returns 0 or -errno. */
int fs_path_lookup(const char *path, struct fs_inode *out);

/* Read file data at an offset. Returns bytes read, 0 at EOF, or -errno. */
ssize_t fs_read_inode_data(const struct fs_inode *in, u64 off, void *buf,
                           size_t len);

/* Read a whole regular file into a freshly allocated buffer. */
int fs_read_all(const struct fs_inode *in, void **buf, size_t *size);

/* The file operations for a regular file or directory on this filesystem. */
const struct file_ops *fs_file_ops(void);
struct file *fs_open_inode(const struct fs_inode *in, u32 flags);

/* Is this file an FrFS file (as opposed to the console or a pipe)? */
bool fs_file_is_fs(const struct file *f);

#endif
