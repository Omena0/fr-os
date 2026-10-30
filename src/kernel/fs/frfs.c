/*
 * frfs.c - the FrFS on-disk filesystem.
 *
 * Layout (tools/mkfs.py writes it):
 *   block 0            superblock
 *   block 1            inode bitmap
 *   block 2            block bitmap
 *   block 3..          inode table, 128 bytes per inode
 *   data_block_start.. data blocks, 12 direct per inode plus one single
 *                      indirect block
 *
 * The mount is read-only: open(2) and exec work, write(2) returns EROFS. The
 * interface in fs.h is what a writer, an ext4 driver or a page cache plugs
 * into; none of them has to know this layout.
 */
#include <block.h>
#include <fs.h>
#include <klog.h>
#include <kstring.h>
#include <sched.h>
#include <process.h>
#include <uapi/errno.h>
#include <uapi/syscall.h>

#define FRFS_MAGIC 0x53465246u

struct frfs_superblock {
    u32 magic;
    u32 version;
    u32 block_size;
    u32 total_blocks;
    u32 inode_count;
    u32 inode_size;
    u32 inode_table_block;
    u32 inode_bitmap_block;
    u32 block_bitmap_block;
    u32 data_block_start;
    u32 root_inode;
    u32 flags;
    u64 created;
} __attribute__((packed));

struct frfs_disk_inode {
    u16 mode;
    u16 nlink;
    u32 uid;
    u32 gid;
    u64 size;
    u64 blocks;
    u32 mtime;
    u32 flags;
    u32 direct[12];
    u32 indirect;
    u32 reserved[10];
} __attribute__((packed));

struct frfs_dirent {
    u32 ino;
    u16 rec_len;
    u8 name_len;
    u8 type;
    char name[];
} __attribute__((packed));

static struct fs_super frfs;
static bool frfs_mounted;

static int frfs_read_block(u32 block, void *buf)
{
    if (!frfs.dev || block >= frfs.total_blocks)
        return -EIO;
    return block_read(frfs.dev,
                      (u64)block * (frfs.block_size / BLOCK_SECTOR_SIZE),
                      frfs.block_size / BLOCK_SECTOR_SIZE, buf) < 0 ? -EIO : 0;
}

static int frfs_read_inode(u32 ino, struct fs_inode *out)
{
    u8 block[4096];
    u64 byte_off;
    u32 blk;
    size_t within;
    struct frfs_disk_inode di;
    int r;

    if (!frfs_mounted)
        return -EIO;
    if (ino == 0 || ino > frfs.inode_count)
        return -EINVAL;

    byte_off = (u64)(ino - 1) * frfs.inode_size;
    blk = frfs.inode_table_block + (u32)(byte_off / frfs.block_size);
    within = (size_t)(byte_off % frfs.block_size);
    if (within + sizeof(di) > frfs.block_size)
        return -EIO;

    r = frfs_read_block(blk, block);
    if (r < 0)
        return r;
    memcpy(&di, block + within, sizeof(di));

    out->ino = ino;
    out->mode = di.mode;
    out->nlink = di.nlink;
    out->size = di.size;
    out->blocks = di.blocks;
    memcpy(out->direct, di.direct, sizeof(out->direct));
    out->indirect = di.indirect;
    return 0;
}

static u32 inode_block_at(const struct fs_inode *in, u32 index, u8 *scratch)
{
    u32 *ptrs;

    if (index < 12)
        return in->direct[index];

    if (!in->indirect)
        return 0;
    if (frfs_read_block(in->indirect, scratch) < 0)
        return 0;

    ptrs = (u32 *)scratch;
    index -= 12;
    if (index >= 4096 / sizeof(u32))
        return 0;
    return ptrs[index];
}

ssize_t fs_read_inode_data(const struct fs_inode *in, u64 off, void *buf,
                           size_t len)
{
    u8 block[4096];
    u8 indirect[4096];
    size_t done = 0;

    if (!in || off >= in->size)
        return 0;
    if (len > in->size - off)
        len = (size_t)(in->size - off);

    while (done < len) {
        u64 pos = off + done;
        u32 index = (u32)(pos / frfs.block_size);
        size_t within = (size_t)(pos % frfs.block_size);
        size_t n = frfs.block_size - within;
        u32 b;

        if (n > len - done)
            n = len - done;

        b = inode_block_at(in, index, indirect);
        if (b == 0) {
            /* A hole reads as zeroes, the same as an unwritten tail. */
            memset((u8 *)buf + done, 0, n);
            done += n;
            continue;
        }
        if (frfs_read_block(b, block) < 0)
            return done ? (ssize_t)done : -EIO;
        memcpy((u8 *)buf + done, block + within, n);
        done += n;
    }
    return (ssize_t)done;
}

int fs_read_all(const struct fs_inode *in, void **buf, size_t *size)
{
    void *p;
    size_t bytes;
    ssize_t n;

    if (!in)
        return -EINVAL;
    if (!(in->mode & FS_IFREG))
        return -EISDIR;
    if (in->size > (u64)SIZE_MAX)
        return -EFBIG;

    bytes = (size_t)in->size;
    p = kmalloc(bytes ? bytes : 1);
    if (!p)
        return -ENOMEM;
    n = bytes ? fs_read_inode_data(in, 0, p, bytes) : 0;
    if (n < 0) {
        kfree(p, bytes ? bytes : 1);
        return (int)n;
    }
    *buf = p;
    *size = (size_t)n;
    return 0;
}

/* Look up one name in a directory inode. */
static int frfs_lookup(const struct fs_inode *dir, const char *name,
                       size_t name_len, struct fs_inode *out)
{
    u8 block[4096];
    u8 indirect[4096];
    u64 pos = 0;

    while (pos < dir->size) {
        u32 index = (u32)(pos / frfs.block_size);
        size_t within = (size_t)(pos % frfs.block_size);
        u32 b = inode_block_at(dir, index, indirect);
        struct frfs_dirent *de;
        int r;

        if (b == 0) {
            pos = (u64)(index + 1) * frfs.block_size;
            continue;
        }
        r = frfs_read_block(b, block);
        if (r < 0)
            return r;
        if (within >= frfs.block_size)
            break;
        de = (struct frfs_dirent *)(block + within);
        if (de->rec_len < sizeof(*de) || within + de->rec_len > frfs.block_size)
            return -EIO;
        if (de->ino && de->name_len == name_len &&
            memcmp(de->name, name, name_len) == 0)
            return frfs_read_inode(de->ino, out);
        pos += de->rec_len;
    }
    return -ENOENT;
}

int fs_path_lookup(const char *path, struct fs_inode *out)
{
    struct fs_inode cur;
    char name[256];
    const char *p = path;
    int r;

    if (!frfs_mounted)
        return -ENOENT;
    if (!path || !*path)
        return -ENOENT;

    r = frfs_read_inode(frfs.root_ino, &cur);
    if (r < 0)
        return r;

    while (*p) {
        size_t len = 0;

        while (*p == '/')
            p++;
        if (!*p)
            break;
        while (*p && *p != '/') {
            if (len + 1 >= sizeof(name))
                return -ENAMETOOLONG;
            name[len++] = *p++;
        }
        name[len] = '\0';

        if (!(cur.mode & FS_IFDIR))
            return -ENOTDIR;
        r = frfs_lookup(&cur, name, len, out);
        if (r < 0)
            return r;
        cur = *out;
    }
    *out = cur;
    return 0;
}

int fs_read_inode(u32 ino, struct fs_inode *out)
{
    return frfs_read_inode(ino, out);
}

bool fs_mounted(void)
{
    return frfs_mounted;
}

int fs_mount_root(void)
{
    u8 sb[BLOCK_SECTOR_SIZE];
    struct frfs_superblock s;
    int count;

    if (frfs_mounted)
        return 0;

    count = block_init();

    for (int i = 0; i < count; i++) {
        struct block_device *dev = block_device_at(i);

        if (!dev || block_read(dev, 0, 1, sb) < 0)
            continue;
        memcpy(&s, sb, sizeof(s));
        if (s.magic != FRFS_MAGIC)
            continue;
        if (s.version != 1 || s.block_size != 4096 ||
            s.inode_size != 128 || s.root_inode == 0) {
            klog_emit(KLOG_WARN, "frfs", "%s: unsupported FrFS image "
                      "(version %u, block %u, inode %u)",
                      dev->name, s.version, s.block_size, s.inode_size);
            continue;
        }
        frfs.dev = dev;
        frfs.block_size = s.block_size;
        frfs.total_blocks = s.total_blocks;
        frfs.inode_count = s.inode_count;
        frfs.inode_size = s.inode_size;
        frfs.inode_table_block = s.inode_table_block;
        frfs.data_block_start = s.data_block_start;
        frfs.root_ino = s.root_inode;
        frfs_mounted = true;
        klog_emit(KLOG_INFO, "frfs",
                  "mounted %s: %u blocks of %u bytes, %u inodes, root %u",
                  dev->name, s.total_blocks, s.block_size,
                  s.inode_count, s.root_inode);
        return 0;
    }

    klog_emit(KLOG_WARN, "frfs", "no FrFS volume found");
    return -ENOENT;
}

/* ------------------------------------------------------------- file ops ---- */

static ssize_t fs_file_read(struct file *f, void *buf, size_t count)
{
    struct fs_inode *in = f->private;
    ssize_t n;

    if (!in || !(in->mode & FS_IFREG))
        return -EISDIR;
    if (f->f_pos >= in->size)
        return 0;
    n = fs_read_inode_data(in, f->f_pos, buf, count);
    if (n > 0)
        f->f_pos += (u64)n;
    return n;
}

static ssize_t fs_file_write(struct file *f, const void *buf, size_t count)
{
    (void)f;
    (void)buf;
    (void)count;
    return -EROFS;
}

static int fs_file_stat(struct file *f, struct kstat *st)
{
    struct fs_inode *in = f->private;

    if (!in)
        return -EBADF;
    memset(st, 0, sizeof(*st));
    st->size = in->size;
    st->blksize = frfs.block_size;
    st->blocks = in->blocks;
    st->ino = in->ino;
    st->mode = in->mode;
    st->nlink = in->nlink;
    return 0;
}

static int fs_file_readdir(struct file *f, struct kdirent *out, size_t cap)
{
    struct fs_inode *in = f->private;
    u8 block[4096];
    u8 indirect[4096];
    u64 pos = f->f_pos;

    if (!in || !(in->mode & FS_IFDIR))
        return -ENOTDIR;

    while (pos < in->size) {
        u32 index = (u32)(pos / frfs.block_size);
        size_t within = (size_t)(pos % frfs.block_size);
        u32 b = inode_block_at(in, index, indirect);
        struct frfs_dirent *de;
        int r;

        if (b == 0) {
            pos = (u64)(index + 1) * frfs.block_size;
            continue;
        }
        r = frfs_read_block(b, block);
        if (r < 0)
            return r;
        de = (struct frfs_dirent *)(block + within);
        if (de->rec_len < sizeof(*de) ||
            within + de->rec_len > frfs.block_size)
            return -EIO;
        if (de->ino == 0) {
            pos += de->rec_len;
            f->f_pos = pos;
            continue;
        }
        if (sizeof(*out) + de->name_len > cap)
            return -EINVAL;
        out->d_ino = de->ino;
        out->d_off = (s64)(pos + de->rec_len);
        out->d_reclen = (u16)(sizeof(*out) + de->name_len);
        out->d_type = de->type == 2 ? 4 : 8;   /* DT_DIR : DT_REG */
        out->d_namlen = de->name_len;
        memcpy(out->d_name, de->name, de->name_len);
        out->d_name[de->name_len] = '\0';
        f->f_pos = pos + de->rec_len;
        return 1;
    }
    return 0;
}

static int fs_file_release(struct file *f)
{
    if (f->private)
        kfree(f->private, sizeof(struct fs_inode));
    f->private = NULL;
    return 0;
}

static const struct file_ops frfs_file_ops = {
    .read = fs_file_read,
    .write = fs_file_write,
    .ioctl = NULL,
    .poll = NULL,
    .stat = fs_file_stat,
    .readdir = fs_file_readdir,
    .release = fs_file_release,
};

const struct file_ops *fs_file_ops(void)
{
    return &frfs_file_ops;
}

bool fs_file_is_fs(const struct file *f)
{
    return f && f->ops == &frfs_file_ops;
}

struct file *fs_open_inode(const struct fs_inode *in, u32 flags)
{
    struct fs_inode *copy;
    struct file *f;

    if (!in)
        return NULL;
    copy = kmalloc(sizeof(*copy));
    if (!copy)
        return NULL;
    *copy = *in;
    f = file_alloc(&frfs_file_ops, copy, flags);
    if (!f) {
        kfree(copy, sizeof(*copy));
        return NULL;
    }
    f->ino = in->ino;
    return f;
}
