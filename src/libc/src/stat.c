/*
 * stat.c - stat-family wrappers.
 *
 * fstat works today (the kernel fills its kstat). stat, lstat, mkdir, chmod
 * and chown call syscall numbers the kernel has assigned but not implemented;
 * they return ENOSYS and the kernel logs a NOT IMPLEMENTED warning, which is
 * the intended signal while those paths are being built.
 */
#include <sys/stat.h>
#include <sys/syscall.h>
#include <uapi/syscall.h>
#include <errno.h>
#include <string.h>

extern int sys_stat(const char *path, struct kstat *st);
extern int sys_lstat(const char *path, struct kstat *st);
extern int sys_fstat(int fd, struct kstat *st);
extern int sys_mkdir(const char *path, unsigned int mode);
extern int sys_chmod(const char *path, unsigned int mode);
extern int sys_chown(const char *path, int uid, int gid);

static int from_kstat(const struct kstat *k, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_ino = k->ino;
    st->st_mode = k->mode;
    st->st_nlink = k->nlink;
    st->st_uid = k->uid;
    st->st_gid = k->gid;
    st->st_size = k->size;
    st->st_blksize = k->blksize;
    st->st_blocks = k->blocks;
    st->st_atime = k->atime_sec;
    st->st_mtime = k->mtime_sec;
    st->st_ctime = k->ctime_sec;
    return 0;
}

int stat(const char *path, struct stat *buf)
{
    struct kstat k;

    if (sys_stat(path, &k) < 0)
        return -1;
    return from_kstat(&k, buf);
}

int lstat(const char *path, struct stat *buf)
{
    struct kstat k;

    if (sys_lstat(path, &k) < 0)
        return -1;
    return from_kstat(&k, buf);
}

int fstat(int fd, struct stat *buf)
{
    struct kstat k;

    if (sys_fstat(fd, &k) < 0)
        return -1;
    return from_kstat(&k, buf);
}

int mkdir(const char *path, mode_t mode)
{
    return sys_mkdir(path, mode);
}

int chmod(const char *path, mode_t mode)
{
    return sys_chmod(path, mode);
}

int chown(const char *path, uid_t owner, gid_t group)
{
    return sys_chown(path, owner, group);
}
