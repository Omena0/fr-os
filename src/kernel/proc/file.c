/*
 * file.c — file objects, the console file, and the per-process descriptor
 * table.
 *
 * There is no filesystem yet; this is the whole VFS-shaped surface. A `struct
 * file` is a reference-counted open object with an operations table, and a task
 * holds an array of pointers to them. The console is the only device, exposed
 * as fds 0/1/2 for every process.
 *
 * Kept separate from the process lifecycle: descriptor and file lifetimes are
 * a reusable component (a future filesystem or device driver plugs into the
 * same ops table), while fork/exec/wait are process policy.
 */
#include <stdbool.h>
#include <types.h>
#include <list.h>
#include <spinlock.h>
#include <kstring.h>
#include <task.h>
#include <process.h>
#include <vmm.h>
#include <tty.h>
#include <uapi/syscall.h>
#include <uapi/errno.h>
#include "console.h"

/* ---------------------------------------------------------------- files ----- */

static spinlock_t file_lock = SPINLOCK_INIT;
static struct list_head file_list = LIST_HEAD_INIT(file_list);

struct file *file_alloc(const struct file_ops *ops, void *private, u32 flags)
{
    struct file *f = kmalloc_zeroed(sizeof(*f));

    if (!f)
        return NULL;

    f->ops = ops;
    f->private = private;
    f->f_flags = flags;
    f->f_pos = 0;
    refcount_set(&f->refs, 1);
    /* A synthetic inode. There is no filesystem yet, so the number only has
     * to be stable and distinct per open object, which the address is. */
    f->ino = (u32)(((u64)f >> 4) & 0xffffff);
    list_init(&f->all_node);

    u64 flags_saved = spinlock_irqsave(&file_lock);

    list_add_tail(&f->all_node, &file_list);
    spinlock_unlock_irqrestore(&file_lock, flags_saved);
    return f;
}

void file_get(struct file *f)
{
    if (f)
        refcount_inc(&f->refs);
}

void file_put(struct file *f)
{
    if (!f)
        return;
    if (!refcount_dec_and_test(&f->refs))
        return;

    if (f->ops && f->ops->release)
        f->ops->release(f);

    u64 flags = spinlock_irqsave(&file_lock);

    if (!list_empty(&f->all_node)) {
        list_del(&f->all_node);
        list_init(&f->all_node);
    }
    spinlock_unlock_irqrestore(&file_lock, flags);
    kfree(f, sizeof(*f));
}

/* --------------------------------------------------------------- console ---- */

/*
 * The console file. read() and write() both go to the tty, which is the only
 * character device that exists; there is no filesystem underneath, so this is
 * the entire content of fds 0, 1 and 2 for every process in the system.
 *
 * The names are prefixed because console.h already owns `console_write` and a
 * file operation with the same name would be a redefinition, not an overload.
 */
static ssize_t cons_file_read(struct file *f, void *buf, size_t count)
{
    bool block = (f->f_flags & O_NONBLOCK) == 0;

    /* The buffer here is already a kernel bounce buffer: the caller copied
     * out of user space into it. tty_read never sees a user address. */
    /* Returns the byte count, not a success flag. A bool here silently
     * truncates every userspace read(0, ...) to a single byte. */
    return (ssize_t)tty_read((char *)buf, count, block);
}

static ssize_t cons_file_write(struct file *f, const void *buf, size_t count)
{
    UNUSED(f);
    tty_write((const char *)buf, count);
    return (ssize_t)count;
}

static long cons_file_ioctl(struct file *f, unsigned long request, void *arg)
{
    UNUSED(f);

    switch (request) {
    case TIOCGWINSZ: {
        struct winsize ws;
        long rc;

        ws.ws_row = (uint16_t)console_rows();
        ws.ws_col = (uint16_t)console_columns();
        ws.ws_xpixel = 0;
        ws.ws_ypixel = 0;
        /* The caller's buffer is a user pointer: it is copied out, not
         * written, like every other boundary crossing. ioctl returns 0
         * on success, not a byte count. */
        rc = copy_to_user((u64)arg, &ws, sizeof(ws));
        return rc < 0 ? rc : 0;
    }
    case TCGETS: {
        /* A termios with everything at its default. libc checks c_cflag
         * for the line discipline and would treat a zeroed struct as a
         * terminal with no modes, which is close enough to be honest
         * for a console with no modes to set. */
        struct termios t;
        long rc;

        memset(&t, 0, sizeof(t));
        t.c_cflag = 0xbf;
        t.c_lflag = 0x8a3f;
        rc = copy_to_user((u64)arg, &t, sizeof(t));
        return rc < 0 ? rc : 0;
    }
    default:
        break;
    }
    return -ENOTTY;
}

static int cons_file_release(struct file *f)
{
    UNUSED(f);
    return 0;
}

static const struct file_ops console_ops = {
    .read = cons_file_read,
    .write = cons_file_write,
    .ioctl = cons_file_ioctl,
    .release = cons_file_release,
};

struct file *console_file_create(void)
{
    return file_alloc(&console_ops, NULL, O_RDWR);
}

/* ------------------------------------------------------------------ fds ----- */

int fd_install(struct task *t, struct file *f)
{
    if (!t->fds)
        return -EMFILE;

    for (u32 i = 0; i < t->fd_count; i++) {
        if (t->fds[i])
            continue;
        t->fds[i] = f;
        t->fd_flags[i] = 0;
        return (int)i;
    }
    return -EMFILE;
}

struct file *fd_lookup(struct task *t, int fd)
{
    if (fd < 0 || (u32)fd >= t->fd_count || !t->fds)
        return NULL;
    return t->fds[fd];
}

int fd_close(struct task *t, int fd)
{
    if (fd < 0 || (u32)fd >= t->fd_count || !t->fds)
        return -EBADF;
    if (!t->fds[fd])
        return -EBADF;

    struct file *f = t->fds[fd];

    t->fds[fd] = NULL;
    t->fd_flags[fd] = 0;
    file_put(f);
    return 0;
}

void fd_close_on_exec(struct task *t)
{
    if (!t->fds)
        return;
    for (u32 i = 0; i < t->fd_count; i++) {
        if (!t->fds[i] || !(t->fd_flags[i] & FD_CLOEXEC))
            continue;
        file_put(t->fds[i]);
        t->fds[i] = NULL;
        t->fd_flags[i] = 0;
    }
}

void fd_table_close_all(struct task *t)
{
    if (!t->fds)
        return;
    for (u32 i = 0; i < t->fd_count; i++) {
        if (!t->fds[i])
            continue;
        file_put(t->fds[i]);
        t->fds[i] = NULL;
        t->fd_flags[i] = 0;
    }
}
