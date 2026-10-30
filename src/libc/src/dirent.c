/*
 * dirent.c - opendir/readdir/closedir over the kernel's getdents entries.
 */
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <uapi/syscall.h>

struct __dirstream {
    int fd;
    size_t pos;
    size_t len;
    struct dirent cur;
    unsigned char buf[1024];
};

extern long sys_getdents(int fd, void *buf, unsigned long count);

DIR *opendir(const char *name)
{
    DIR *d;
    int fd = open(name, O_RDONLY | O_DIRECTORY);

    if (fd < 0)
        return NULL;
    d = malloc(sizeof(*d));
    if (!d) {
        close(fd);
        errno = ENOMEM;
        return NULL;
    }
    d->fd = fd;
    d->pos = d->len = 0;
    return d;
}

struct dirent *readdir(DIR *dirp)
{
    struct kdirent *de;
    size_t namelen;

    if (!dirp) {
        errno = EINVAL;
        return NULL;
    }
    for (;;) {
        if (dirp->pos >= dirp->len) {
            long n = sys_getdents(dirp->fd, dirp->buf, sizeof(dirp->buf));
            if (n <= 0)
                return NULL;
            dirp->pos = 0;
            dirp->len = (size_t)n;
        }
        de = (struct kdirent *)(dirp->buf + dirp->pos);
        if (de->d_reclen < sizeof(*de)) {
            errno = EIO;
            return NULL;
        }
        dirp->pos += de->d_reclen;

        namelen = de->d_namlen;
        if (namelen >= sizeof(dirp->cur.d_name))
            namelen = sizeof(dirp->cur.d_name) - 1;
        dirp->cur.d_ino = de->d_ino;
        dirp->cur.d_off = (unsigned long)de->d_off;
        dirp->cur.d_reclen = de->d_reclen;
        dirp->cur.d_type = de->d_type;
        memcpy(dirp->cur.d_name, de->d_name, namelen);
        dirp->cur.d_name[namelen] = '\0';
        return &dirp->cur;
    }
}

int closedir(DIR *dirp)
{
    int rc;

    if (!dirp) {
        errno = EINVAL;
        return -1;
    }
    rc = close(dirp->fd);
    free(dirp);
    return rc;
}
