/*
 * syscall.c — the dispatch table and the handlers.
 *
 * Dispatch is a flat array of 288 function pointers, built once at init. The
 * number is bounds-checked before the table is indexed, and an unpopulated
 * slot returns -ENOSYS, so a caller that guesses a number gets a clean error
 * instead of an indirect call through whatever is in that slot.
 *
 * Argument discipline, which is the whole discipline:
 *
 *   - Nothing dereferences a user pointer. Every access goes through
 *     copy_from_user/copy_to_user, which validate the range and translate
 *     page by page. A handler that needs a struct copies it into a kernel
 *     local first.
 *   - Lengths come from a validated range, not from the caller, and a
 *     structure is only trusted as far as the bytes that were actually
 *     copied.
 *   - A handler that blocks (read, wait4, nanosleep) does so through the
 *     scheduler. When it is resumed it continues from exactly where it was,
 *     because the whole C frame lives on its own kernel stack.
 */
#include <stdbool.h>
#include <io.h>
#include <types.h>
#include <list.h>
#include <spinlock.h>
#include <klog.h>
#include <panic.h>
#include <kstring.h>
#include <kprintf.h>
#include <percpu.h>
#include <pmm.h>
#include <gdt.h>
#include <cpu_features.h>
#include <task.h>
#include <sched.h>
#include <process.h>
#include <ktime.h>
#include <syscall.h>
#include <console.h>
#include <tty.h>
#include <uapi/graphics.h>
#include <uapi/mouse.h>
#include <uapi/sysinfo.h>
#include <signals.h>
#include <version.h>
#include <drivers/ps2mouse.h>
#include <drivers/ps2kbd.h>

#define SYSCALL_LOG(level, ...)                                              \
    do {                                                               \
        if ((level) >= klog_runtime_level)                         \
            klog_emit((level), "syscall", __VA_ARGS__);         \
    } while (0)

struct syscall_cpu syscall_cpus[MAX_CPUS];

/* The table. Zero-initialised, so an unassigned number is a null pointer and
 * therefore -ENOSYS with no separate "is it implemented" table to keep in
 * sync. */
static long (*syscall_table[SYSCALL_MAX])(struct syscall_regs *);

void syscall_set_kernel_stack(void *stack_top)
{
    syscall_cpus[this_cpu_id()].kstack_top = (u64)stack_top;
}

void syscall_abandon_frame(void)
{
    syscall_cpus[this_cpu_id()].ret_to_user = 0;
}

u64 syscall_ret_to_user_get(void)
{
    return syscall_cpus[this_cpu_id()].ret_to_user;
}

void syscall_ret_to_user_set(u64 v)
{
    syscall_cpus[this_cpu_id()].ret_to_user = v;
}

__noreturn void syscall_lost_frame(void)
{
    /*
     * Reaching here means a syscall returned to a frame that was already
     * abandoned — a handler blocked and was never supposed to come back to
     * this point, or the switch-away bookkeeping is wrong. Returning to
     * user mode would execute a user stack frame that no longer exists,
     * so the only safe answer is to stop.
     */
    panic("syscall entry returned to an abandoned frame");
    __builtin_unreachable();
}

struct address_space *syscall_user_mm(void)
{
    struct task *t = current_task();

    return t ? t->mm : NULL;
}

static inline struct task *caller(void)
{
    return current_task();
}

/* ======================================================================= */

static __noreturn long sc_exit(struct syscall_regs *r)
{
    sys_exit((int)(s32)arg0(r));
    __builtin_unreachable();
}

static __noreturn long sc_exit_group(struct syscall_regs *r)
{
    sys_exit_group((int)(s32)arg0(r));
    __builtin_unreachable();
}

static long sc_getpid(struct syscall_regs *r)
{
    struct task *t = caller();

    UNUSED(r);
    return t ? (long)t->pid : 0;
}

static long sc_getppid(struct syscall_regs *r)
{
    struct task *t = caller();

    UNUSED(r);
    return t && t->parent ? (long)t->parent->pid : 0;
}

static long sc_gettid(struct syscall_regs *r)
{
    struct task *t = caller();

    UNUSED(r);
    return t ? (long)t->tid : 0;
}

/*
 * Byte offsets of the callee-saved user registers from &struct syscall_regs
 * in the frame built by syscall_entry.S. That file documents the layout:
 * after the struct's 88-byte register area, 8 bytes of padding, then the six
 * callee-saved registers were pushed in reverse, so r15 is nearest the struct.
 * Keep these in step with the push sequence there; the return path pops them
 * in the order r15,r14,r13,r12,rbx,rbp.
 */
#define SC_FRAME_R15_OFF 104
#define SC_FRAME_R14_OFF 112
#define SC_FRAME_R13_OFF 120
#define SC_FRAME_R12_OFF 128
#define SC_FRAME_RBX_OFF 136
#define SC_FRAME_RBP_OFF 144

static void fork_regs_from_frame(struct fork_regs *fr,
                                 const struct syscall_regs *r)
{
    fr->rip = r->rip;
    fr->rsp = r->rsp;
    fr->rflags = r->rflags;
    /* Callee-saved six, saved above struct syscall_regs by the entry path. */
    fr->r15 = ((const u64 *)((const u8 *)r + SC_FRAME_R15_OFF))[0];
    fr->r14 = ((const u64 *)((const u8 *)r + SC_FRAME_R14_OFF))[0];
    fr->r13 = ((const u64 *)((const u8 *)r + SC_FRAME_R13_OFF))[0];
    fr->r12 = ((const u64 *)((const u8 *)r + SC_FRAME_R12_OFF))[0];
    fr->rbx = ((const u64 *)((const u8 *)r + SC_FRAME_RBX_OFF))[0];
    fr->rbp = ((const u64 *)((const u8 *)r + SC_FRAME_RBP_OFF))[0];
    /* The syscall ABI preserves the rest too (only rcx/r11 are clobbered). */
    fr->rdi = r->rdi;
    fr->rsi = r->rsi;
    fr->rdx = r->rdx;
    fr->r10 = r->r10;
    fr->r8 = r->r8;
    fr->r9 = r->r9;
    fr->rcx = r->rcx;
    fr->r11 = r->r11;
}

static long sc_fork(struct syscall_regs *r)
{
    /*
     * The child resumes at the instruction after the syscall, which is only
     * knowable from the register frame the entry path built. A fresh child has
     * no frame of its own, so the whole register state travels to sys_fork();
     * it must re-enter the parent's compiled code with the parent's
     * callee-saved registers intact.
     */
    struct fork_regs fr;
    fork_regs_from_frame(&fr, r);
    return sys_fork(&fr);
}

static long sc_execve(struct syscall_regs *r)
{
    return sys_execve(arg0(r), arg1(r), arg2(r));
}

static long sc_wait4(struct syscall_regs *r)
{
    return sys_wait4((s64)arg0(r), arg1(r), arg2(r), arg3(r));
}

static long sc_sched_yield(struct syscall_regs *r)
{
    UNUSED(r);
    sched_yield();
    return 0;
}

static long sc_getcpu(struct syscall_regs *r)
{
    /* The ABI is `struct { u32 cpu; u32 node; }`, so cpu is the low
     * half. */
    u64 pair = (u64)0 << 32 | (u32)sched_task_cpu(caller());

    if (!user_range_ok(syscall_user_mm(), arg0(r), sizeof(pair)))
        return -EFAULT;
    return copy_to_user(arg0(r), &pair, sizeof(pair)) < 0 ? -EFAULT : 0;
}

static long sc_screen_claim(struct syscall_regs *r)
{
    UNUSED(r);
    return tty_screen_claim();
}

static long sc_screen_release(struct syscall_regs *r)
{
    UNUSED(r);
    tty_screen_release();
    return 0;
}

static long sc_uname(struct syscall_regs *r)
{
    struct utsname u;

    if (!arg0(r))
        return -EFAULT;

    memset(&u, 0, sizeof(u));
    strlcpy(u.sysname, FR_PROJECT_NAME, sizeof(u.sysname));
    strlcpy(u.nodename, "fr-os", sizeof(u.nodename));
    strlcpy(u.release, KERNEL_VERSION, sizeof(u.release));
    strlcpy(u.version, KERNEL_BUILD_STAMP, sizeof(u.version));
    strlcpy(u.machine, "x86_64", sizeof(u.machine));
    strlcpy(u.domainname, "", sizeof(u.domainname));

    return copy_to_user(arg0(r), &u, sizeof(u)) < 0 ? -EFAULT : 0;
}

static long sc_sysinfo(struct syscall_regs *r)
{
    struct sysinfo si;

    if (!arg0(r))
        return -EFAULT;

    memset(&si, 0, sizeof(si));
    si.uptime = ktime_mono_ns() / 1000000000ULL;
    si.totalram = (u64)pmm_total_bytes();
    si.freeram = (u64)pmm_free_bytes();
    si.mem_unit = 1;
    si.procs = 1;

    return copy_to_user(arg0(r), &si, sizeof(si)) < 0 ? -EFAULT : 0;
}

/*
 * Resource limits are stored per task and inherited across fork. They are
 * reported and validated, but nothing enforces them yet: a process can read
 * and lower its limits, which is what shells and configure scripts need.
 */
static long sc_rt_sigaction(struct syscall_regs *r)
{
    return sys_rt_sigaction((int)(s32)arg0(r), arg1(r), arg2(r), arg3(r));
}

static long sc_rt_sigprocmask(struct syscall_regs *r)
{
    return sys_rt_sigprocmask((int)(s32)arg0(r), arg1(r), arg2(r), arg3(r));
}

static long sc_rt_sigpending(struct syscall_regs *r)
{
    return sys_rt_sigpending(arg0(r), arg1(r));
}

static long sc_getrlimit(struct syscall_regs *r)
{
    struct task *t = caller();
    int resource = (int)(s32)arg0(r);

    if (arg1(r) == 0)
        return -EFAULT;
    if (resource < 0 || resource >= RLIMIT_NLIMITS)
        return -EINVAL;

    return copy_to_user(arg1(r), &t->rlim[resource], sizeof(struct rlimit)) < 0 ? -EFAULT : 0;
}

static long sc_setrlimit(struct syscall_regs *r)
{
    struct task *t = caller();
    int resource = (int)(s32)arg0(r);
    struct rlimit rl;

    if (arg1(r) == 0)
        return -EFAULT;
    if (resource < 0 || resource >= RLIMIT_NLIMITS)
        return -EINVAL;
    if (copy_from_user(&rl, arg1(r), sizeof(rl)) < 0)
        return -EFAULT;
    if (rl.rlim_cur > rl.rlim_max)
        return -EINVAL;
    /* A hard limit can only be lowered; raising it needs privilege. */
    if (rl.rlim_max > t->rlim[resource].rlim_max)
        return -EPERM;

    t->rlim[resource] = rl;
    return 0;
}

static long sc_gfx_info(struct syscall_regs *r)
{
    struct framebuffer_info fb;
    struct gfx_display_info info;

    if (!console_framebuffer_get_info(&fb))
        return -ENODEV;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    info.width = fb.width;
    info.height = fb.height;
    info.pitch = fb.pitch;
    info.bpp = fb.bpp;
    info.red_shift = fb.red_shift;
    info.green_shift = fb.green_shift;
    info.blue_shift = fb.blue_shift;
    return copy_to_user(arg0(r), &info, sizeof(info)) < 0 ? -EFAULT : 0;
}

/*
 * mouse_info(state) — copy the pointer snapshot out.
 *
 * The kernel owns the pointer position; a process only reads it. The return is
 * the number of bytes copied (the struct size) on success, so a future larger
 * struct can be negotiated by `size` without breaking old callers.
 */
/*
 * kbd_state(mask) — copy the held-scancode bitmap out. A program uses it to
 * implement its own auto-repeat: the keyboard's typematic delay cannot go
 * below 250 ms, which is too long for held movement in a game.
 */
static long sc_kbd_state(struct syscall_regs *r)
{
    u64 mask = ps2kbd_key_mask();

    if (!arg0(r))
        return -EFAULT;
    return copy_to_user(arg0(r), &mask, sizeof(mask)) < 0 ? -EFAULT : 0;
}

/*
 * kbd_typematic(value) — set, or query with a negative value, the keyboard's
 * typematic delay/rate byte. Returns the previous value (or the current one
 * for a query), so a program can restore it when it exits.
 */
static long sc_kbd_typematic(struct syscall_regs *r)
{
    long value = (long)arg0(r);

    if (value < 0)
        return ps2kbd_get_typematic();
    if (value > 0x7f)
        return -EINVAL;

    return ps2kbd_set_typematic((u8)value);
}

static long sc_mouse_info(struct syscall_regs *r)
{
    struct mouse_state state;
    u64 dst = arg0(r);

    if (!dst)
        return -EFAULT;
    if (!user_range_ok(syscall_user_mm(), dst, sizeof(state)))
        return -EFAULT;

    memset(&state, 0, sizeof(state));
    ps2mouse_get_state(&state);
    state.size = sizeof(state);

    return copy_to_user(dst, &state, sizeof(state)) < 0 ? -EFAULT
                                                        : (long)sizeof(state);
}

static long sc_gfx_present(struct syscall_regs *r)
{
    u64 src = arg0(r);
    u64 len = arg1(r);
    struct framebuffer_info fb;
    uint8_t chunk[4096];
    uint64_t total, off = 0;

    if (!console_framebuffer_get_info(&fb) || fb.bpp != 32)
        return -ENODEV;
    if (!tty_screen_owned() || tty_screen_owner_pid() != current_task()->pid)
        return -EPERM;
    total = (uint64_t)fb.width * fb.height * 4;
    if (len != total || !user_range_ok(syscall_user_mm(), src, len))
        return -EINVAL;
    while (off < total) {
        size_t n = (size_t)(total - off);
        if (n > sizeof(chunk))
            n = sizeof(chunk);
        if (copy_from_user(chunk, src + off, n) < 0)
            return -EFAULT;
        if (console_framebuffer_write(off, chunk, n) < 0)
            return -EIO;
        off += n;
    }
    return 0;
}

static long sc_nanosleep(struct syscall_regs *r)
{
    struct timespec req;
    u64 ns;

    if (copy_from_user(&req, arg0(r), sizeof(req)) < 0)
        return -EFAULT;
    if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= 1000000000LL)
        return -EINVAL;

    ns = (u64)req.tv_sec * 1000000000ULL + (u64)req.tv_nsec;
    sched_sleep_ns(ns);

    /* The remaining time is always zero: nothing interrupts a sleep yet,
     * so reporting a remainder would be a lie. */
    if (arg1(r)) {
        struct timespec rem = { 0, 0 };

        if (copy_to_user(arg1(r), &rem, sizeof(rem)) < 0)
            return -EFAULT;
    }
    return 0;
}

static long sc_clock_gettime(struct syscall_regs *r)
{
    struct timespec ts;
    int clock = (int)(s32)arg0(r);

    /* Only CLOCK_REALTIME (0) and CLOCK_MONOTONIC (1) are valid. */
    if (clock != 0 && clock != 1)
        return -EINVAL;

    /*
     * CLOCK_REALTIME is the RTC-derived Unix clock; CLOCK_MONOTONIC is the
     * TSC-based counter since ktime_init(). Both are read once so the pair
     * cannot straddle an update and step backwards.
     */
    {
        uint64_t now = (clock == 0) ? ktime_realtime_ns() : ktime_mono_ns();

        ts.tv_sec = (s64)(now / 1000000000ULL);
        ts.tv_nsec = (s64)(now % 1000000000ULL);
    }
    long rc = copy_to_user(arg1(r), &ts, sizeof(ts));
    if (rc < 0)
        return -EFAULT;
    return 0;
}

static long sc_shutdown(struct syscall_regs *r)
{
    uint16_t fn = (uint16_t)(s16)arg0(r);

    switch (fn) {
    case 0: /* Shutdown: pulse the reset register; QEMU exits with -no-reboot. */
        outb(0xCF9, 0x06);
        break;
    case 1: /* Halt: HLT loop forever. */
        for (;;)
            __asm__ __volatile__("hlt" ::: "memory");
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

/* ------------------------------------------------------------- files ------- */

static long sc_open(struct syscall_regs *r)
{
    char path[256];

    UNUSED(r);
    /*
     * Validated even though the answer is always the same. A syscall that
     * ignores its arguments silently reports "no such file" for a pointer
     * that does not exist; the validation is what tells the caller its
     * pointer was wrong.
     */
    if (copy_string_from_user(path, arg0(r), sizeof(path)) < 0)
        return -EFAULT;
    return -ENOENT;
}

static long sc_close(struct syscall_regs *r)
{
    struct task *t = caller();
    int fd = (int)(s32)arg0(r);
    struct file *f = fd_lookup(t, fd);

    if (!f)
        return -EBADF;
    return fd_close(t, fd);
}

/*
 * Shared read/write body. Bounce buffer on the kernel stack rather than a
 * per-CPU static: a single shared object would tear data between two CPUs
 * servicing a read and a write at the same time, and a per-CPU array of
 * 256 pages would not fit in the kernel image budget.
 */
#define IO_CHUNK PAGE_SIZE

/*
 * Common bounce-buffer IO loop for file_read and file_write.
 *
 * The two paths are mirror images -- read copies file->bounce->user,
 * write copies user->bounce->file -- and shared the same loop,
 * bounds checks and short-count handling. is_write selects the
 * direction; the per-op validation (mode bits, f_ops presence) is
 * the caller's responsibility.
 *
 * Returns the number of bytes transferred, or a negative errno.
 */
static long file_io(struct task *t, struct file *f, u64 buf,
            size_t count, bool is_write)
{
    ssize_t total = 0;
    u8 bounce[IO_CHUNK];

    (void)t;
    while (count) {
        size_t chunk = count > IO_CHUNK ? IO_CHUNK : count;
        long rc;
        ssize_t n;

        if (is_write) {
            rc = copy_from_user(bounce, buf + (u64)total, chunk);
            if (rc < 0)
                return rc;
            n = f->ops->write(f, bounce, chunk);
            if (n < 0)
                return (long)n;
        } else {
            n = f->ops->read(f, bounce, chunk);
            if (n < 0)
                return (long)n;
            /* Read into the bounce buffer first and only
             * then into the user page, so a short tty read
             * cannot leave a partial, unvalidated copy behind.
             */
            rc = copy_to_user(buf + (u64)total, bounce,
                      (size_t)n);
            if (rc < 0)
                return rc;
        }

        if (n == 0)
            break;
        total += n;
        count -= (size_t)n;
        if ((size_t)n < chunk)
            break;
    }
    return total;
}

static long file_read(struct task *t, int fd, u64 buf, size_t count)
{
    struct file *f = fd_lookup(t, fd);

    if (!f)
        return -EBADF;
    if (!f->ops || !f->ops->read)
        return -EBADF;
    if ((f->f_flags & O_ACCMODE) == O_WRONLY)
        return -EBADF;
    if (count == 0)
        return 0;
    if (user_write_range_prepare(t->mm, buf, count) < 0)
        return -EFAULT;

    return file_io(t, f, buf, count, false);
}

static long file_write(struct task *t, int fd, u64 buf, size_t count)
{
    struct file *f = fd_lookup(t, fd);

    if (!f)
        return -EBADF;
    if (!f->ops || !f->ops->write)
        return -EBADF;
    if ((f->f_flags & O_ACCMODE) == O_RDONLY)
        return -EBADF;
    if (count == 0)
        return 0;
    if (!user_range_ok(t->mm, buf, count))
        return -EFAULT;

    return file_io(t, f, buf, count, true);
}

static long sc_read(struct syscall_regs *r)
{
    return file_read(caller(), (int)(s32)arg0(r), arg1(r), (size_t)arg2(r));
}

static long sc_write(struct syscall_regs *r)
{
    return file_write(caller(), (int)(s32)arg0(r), arg1(r), (size_t)arg2(r));
}

static long sc_lseek(struct syscall_regs *r)
{
    struct file *f = fd_lookup(caller(), (int)(s32)arg0(r));

    if (!f)
        return -EBADF;
    if (arg2(r) != 0 && arg2(r) != 1 && arg2(r) != 2)
        return -EINVAL;
    /* A character device has no offsets to seek between. */
    return -ESPIPE;
}

static long sc_fstat(struct syscall_regs *r)
{
    struct file *f = fd_lookup(caller(), (int)(s32)arg0(r));
    struct kstat st;

    if (!f)
        return -EBADF;

    memset(&st, 0, sizeof(st));
    st.mode = 0020000u | 0666u;    /* S_IFCHR | rw-rw-rw- */
    st.nlink = 1;
    st.blksize = PAGE_SIZE;
    st.ino = f->ino;
    /* A terminal has no length. Reporting 0 is what makes isatty() and
     * the stdio size checks behave. */
    st.size = 0;
    return copy_to_user(arg1(r), &st, sizeof(st)) < 0 ? -EFAULT : 0;
}

static long sc_fcntl(struct syscall_regs *r)
{
    struct task *t = caller();
    int fd = (int)(s32)arg0(r);
    struct file *f = fd_lookup(t, fd);
    int cmd = (int)(s32)arg1(r);
    long arg = (long)(s32)arg2(r);

    if (!f)
        return -EBADF;

    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        if (arg < 0)
            return -EINVAL;
        for (u32 i = (u32)arg; i < t->fd_count; i++) {
            if (t->fds[i])
                continue;
            file_get(f);
            t->fds[i] = f;
            t->fd_flags[i] = (cmd == F_DUPFD_CLOEXEC) ? FD_CLOEXEC : 0;
            return (long)i;
        }
        return -EMFILE;
    }
    case F_GETFD:
        return t->fd_flags[fd] & FD_CLOEXEC;
    case F_SETFD:
        t->fd_flags[fd] = (u8)(arg & FD_CLOEXEC);
        return 0;
    case F_GETFL:
        return (long)f->f_flags;
    case F_SETFL:
        /* Only O_NONBLOCK is meaningful here. Preserve the access mode and
         * every other status flag; replace the one bit the caller owns. */
        f->f_flags = (f->f_flags & ~O_NONBLOCK) | ((u32)arg & O_NONBLOCK);
        return 0;
    default:
        return -EINVAL;
    }
}

static long sc_dup3(struct syscall_regs *r)
{
    struct task *t = caller();
    int oldfd = (int)(s32)arg0(r);
    int newfd = (int)(s32)arg1(r);
    u64 flags = arg2(r);
    struct file *f = fd_lookup(t, oldfd);

    if (!f)
        return -EBADF;
    if (oldfd == newfd)
        return -EINVAL;
    if ((u32)newfd >= t->fd_count)
        return -EBADF;
    if (flags & ~(u64)O_CLOEXEC)
        return -EINVAL;

    if (t->fds[newfd])
        fd_close(t, newfd);

    file_get(f);
    t->fds[newfd] = f;
    t->fd_flags[newfd] = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
    return newfd;
}

static long sc_ioctl(struct syscall_regs *r)
{
    struct file *f = fd_lookup(caller(), (int)(s32)arg0(r));

    if (!f)
        return -EBADF;
    if (!f->ops || !f->ops->ioctl)
        return -ENOTTY;
    /* The argument is a user pointer and the operation decides what it
     * means; the file op is the one that copies it out. */
    return f->ops->ioctl(f, (unsigned long)arg1(r), (void *)arg2(r));
}

static long sc_getdents(struct syscall_regs *r)
{
    struct file *f = fd_lookup(caller(), (int)(s32)arg0(r));

    UNUSED(r);
    if (!f)
        return -EBADF;
    /*
     * Not -ENOSYS: the request is well formed, the object it names
     * exists, and the object simply is not a directory. Returning ENOTDIR
     * is the truthful answer and is what a caller looping over a directory
     * needs in order to stop.
     */
    return -ENOTDIR;
}

static long sc_dup(struct syscall_regs *r)
{
    struct task *t = caller();
    struct file *f = fd_lookup(t, (int)(s32)arg0(r));
    int nfd;

    if (!f)
        return -EBADF;
    nfd = fd_install(t, f);
    if (nfd < 0)
        return nfd;
    file_get(f);
    return nfd;
}

/* ------------------------------------------------------------- pipes ------- */

#define PIPE_CAPACITY 65536

/*
 * An anonymous pipe. One ring buffer, one lock, and at most one waiter per
 * direction.
 *
 * The single-waiter limit is a real deviation from POSIX, which allows any
 * number of blocked readers. It is a deviation in the safe direction: with
 * several writers the second and subsequent wakeups come from the writer that
 * filled the buffer rather than from a waiter list, so nothing is lost but
 * fairness between equally-blocked readers. A general waiter queue is a
 * change confined to this file.
 */
struct pipe {
    spinlock_t lock;
    u8 *buf;
    u32 head;
    u32 tail;
    u32 count;
    refcount_t refs;
    struct task *read_waiter;
    struct task *write_waiter;
    bool read_closed;
    bool write_closed;
};

static struct pipe *pipe_alloc(void)
{
    struct pipe *p = kmalloc_zeroed(sizeof(*p));

    if (!p)
        return NULL;
    p->buf = kmalloc(PIPE_CAPACITY);
    if (!p->buf) {
        kfree(p, sizeof(*p));
        return NULL;
    }
    spinlock_init(&p->lock);
    refcount_set(&p->refs, 2);    /* two ends */
    return p;
}

static void pipe_put(struct pipe *p)
{
    if (!refcount_dec_and_test(&p->refs))
        return;
    kfree(p->buf, PIPE_CAPACITY);
    kfree(p, sizeof(*p));
}

static ssize_t pipe_read_op(struct file *f, void *buf, size_t count)
{
    struct pipe *p = f->private;
    struct task *self = caller();
    struct task *w;
    bool nonblock = (f->f_flags & O_NONBLOCK) != 0;

    if (count == 0)
        return 0;

    for (;;) {
        size_t avail;

        spinlock_acquire(&p->lock);
        if (p->count) {
            avail = p->count;
            if (avail > count)
                avail = count;
            for (size_t i = 0; i < avail; i++) {
                ((u8 *)buf)[i] = p->buf[p->head];
                p->head = (p->head + 1) % PIPE_CAPACITY;
            }
            p->count -= (u32)avail;
            /* A writer may be blocked because the buffer was
             * full; the space just made is enough for it. The
             * waiter is taken under the lock and woken after
             * it is dropped: sched_wake can switch, and
             * switching with the pipe lock held would deadlock
             * against the writer's own path. */
            w = p->write_waiter;
            p->write_waiter = NULL;
            spinlock_release(&p->lock);
            if (w)
                sched_wake(w);
            return (ssize_t)avail;
        }
        if (p->write_closed) {
            spinlock_release(&p->lock);
            return 0;    /* EOF */
        }
        if (nonblock) {
            spinlock_release(&p->lock);
            return -EAGAIN;
        }
        p->read_waiter = self;
        spinlock_release(&p->lock);
        /* Sleeping here is the only blocking operation on the whole
         * path; the loop re-checks under the lock, so a wakeup that
         * races the sleep cannot be lost. */
        sched_block_current();
    }
}

static ssize_t pipe_write_op(struct file *f, const void *buf, size_t count)
{
    struct pipe *p = f->private;
    struct task *self = caller();
    struct task *r;
    bool nonblock = (f->f_flags & O_NONBLOCK) != 0;

    for (;;) {
        size_t space;

        spinlock_acquire(&p->lock);
        space = PIPE_CAPACITY - p->count;
        if (space) {
            if (space > count)
                space = count;
            for (size_t i = 0; i < space; i++) {
                p->buf[p->tail] = ((const u8 *)buf)[i];
                p->tail = (p->tail + 1) % PIPE_CAPACITY;
            }
            p->count += (u32)space;
            r = p->read_waiter;
            p->read_waiter = NULL;
            spinlock_release(&p->lock);
            if (r)
                sched_wake(r);
            return (ssize_t)space;
        }
        if (p->read_closed) {
            spinlock_release(&p->lock);
            return -EPIPE;
        }
        if (nonblock) {
            spinlock_release(&p->lock);
            return -EAGAIN;
        }
        p->write_waiter = self;
        spinlock_release(&p->lock);
        sched_block_current();
    }
}

static int pipe_release(struct file *f)
{
    struct pipe *p = f->private;

    if (!p)
        return 0;
    spinlock_acquire(&p->lock);
    if (f->f_flags & O_WRONLY)
        p->write_closed = true;
    else
        p->read_closed = true;
    /* Waking both on close is what turns a blocked reader into an EOF
     * return and a blocked writer into EPIPE, rather than leaving it
     * asleep for a write that can never succeed. */
    if (p->read_waiter) {
        struct task *r = p->read_waiter;

        p->read_waiter = NULL;
        spinlock_release(&p->lock);
        sched_wake(r);
        spinlock_acquire(&p->lock);
    }
    if (p->write_waiter) {
        struct task *w = p->write_waiter;

        p->write_waiter = NULL;
        spinlock_release(&p->lock);
        sched_wake(w);
        spinlock_acquire(&p->lock);
    }
    spinlock_release(&p->lock);
    pipe_put(p);
    f->private = NULL;
    return 0;
}

static const struct file_ops pipe_ops = {
    .read = pipe_read_op,
    .write = pipe_write_op,
    .ioctl = NULL,
    .release = pipe_release,
};

static long sc_pipe(struct syscall_regs *r)
{
    struct task *t = caller();
    struct pipe *p;
    struct file *rf, *wf;
    int rfd, wfd;
    u64 pair;

    if (!t || !t->mm)
        return -ESRCH;
    if (!user_range_ok(t->mm, arg0(r), sizeof(pair)))
        return -EFAULT;

    p = pipe_alloc();
    if (!p)
        return -ENOMEM;

    /*
     * The pipe starts with two references, one per end. A file that was
     * never created still has to give its reference back, and a file that
     * was created gives it back through its release operation, so the
     * two cases are spelled out rather than balanced by an extra
     * pipe_put() that would free the object twice.
     */
    rf = file_alloc(&pipe_ops, p, O_RDONLY);
    wf = file_alloc(&pipe_ops, p, O_WRONLY);
    if (!rf || !wf) {
        if (rf)
            file_put(rf);
        else
            pipe_put(p);
        if (wf)
            file_put(wf);
        else
            pipe_put(p);
        return -ENOMEM;
    }

    rfd = fd_install(t, rf);
    if (rfd < 0) {
        file_put(rf);
        file_put(wf);
        return rfd;
    }
    wfd = fd_install(t, wf);
    if (wfd < 0) {
        fd_close(t, rfd);
        file_put(wf);
        return wfd;
    }

    pair = ((u64)(u32)wfd << 32) | (u32)rfd;
    return copy_to_user(arg0(r), &pair, sizeof(pair)) < 0 ? -EFAULT : 0;
}

static long sc_futex(struct syscall_regs *r)
{
    u64 op = arg1(r) & 0x7f;
    int value;

    if (op != 1) /* FUTEX_WAKE */
        return -ENOSYS;
    if (copy_from_user(&value, arg0(r), sizeof(value)) < 0)
        return -EFAULT;
    /* There is no wait queue implementation yet, so no waiters can be woken. */
    return 0;
}

/* ------------------------------------------------------------ memory ------- */

static long sc_brk(struct syscall_regs *r)
{
    struct address_space *mm = syscall_user_mm();
    u64 want = arg0(r);

    if (!mm)
        return -ENOMEM;
    if (want == 0)
        return (long)mm->brk;

    want = ALIGN_UP(want, PAGE_SIZE);

    /* Confine brk to the heap VMA only: never shrink below start_brk
     * (the initial heap base), and never grow past the RLIMIT_DATA bound. */
    if (want < mm->start_brk)
        return (long)mm->brk;

    if (want < mm->brk) {
        mm_remove_vma(mm, (virt_addr_t)want, (virt_addr_t)mm->brk);
        mm->brk = want;
        return (long)mm->brk;
    }
    /*
     * No RLIMIT_DATA yet, so the only bound is one the kernel enforces on
     * itself: a single brk that would reserve more than this is refused
     * rather than attempted, because a failed allocation halfway through a
     * large grow would leave the heap in a state libc cannot reason about.
     */
    if (want - mm->brk > (512ULL << 20))
        return -ENOMEM;

    int rc = mm_add_vma(mm, (virt_addr_t)mm->brk, (virt_addr_t)want,
                VM_READ | VM_WRITE | VM_USER, VM_ANON);

    if (rc < 0)
        return rc;
    mm->brk = want;
    return (long)mm->brk;
}

static long sc_mmap(struct syscall_regs *r)
{
    struct address_space *mm = syscall_user_mm();
    /* mmap(addr, len, prot, flags, fd, offset) */
    u64 addr = arg0(r);
    u64 len = arg1(r);
    u64 prot = arg2(r);
    u64 flags = arg3(r);
    virt_addr_t vaddr;
    uint32_t vmprot = VM_USER;
    int rc;

    if (!mm)
        return -ENOMEM;
    if (len == 0 || len > (1ULL << 40))
        return -EINVAL;
    if (prot & ~(u64)(PROT_READ | PROT_WRITE | PROT_EXEC))
        return -EINVAL;
    /* W^X enforcement: reject simultaneous write + execute.
     * JITs must use mprotect to toggle between writable and executable. */
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC))
        return -EINVAL;
    if ((flags & MAP_SHARED) && (flags & MAP_PRIVATE))
        return -EINVAL;
    /* No file descriptor, so only an anonymous mapping is meaningful. A
     * file-backed one would need a filesystem and an offset convention
     * that does not exist yet. */
    if (!(flags & MAP_ANONYMOUS))
        return -ENOSYS;
    UNUSED(arg4(r));

    len = ALIGN_UP(len, PAGE_SIZE);

    if (prot & PROT_READ)
        vmprot |= VM_READ;
    if (prot & PROT_WRITE)
        vmprot |= VM_WRITE;
    if (prot & PROT_EXEC)
        vmprot |= VM_EXEC;

    if (flags & MAP_FIXED) {
        if (addr & (PAGE_SIZE - 1))
            return -EINVAL;
        if (addr + len < addr)
            return -EINVAL;
        vaddr = (virt_addr_t)addr;
        /* MAP_FIXED replaces whatever is there, which is the whole
         * point and also the whole danger. */
        mm_remove_vma(mm, vaddr, vaddr + len);
    } else {
        vaddr = mm_find_free(mm, len, PAGE_SIZE);
        if (!vaddr)
            return -ENOMEM;
    }

    rc = mm_add_vma(mm, vaddr, vaddr + len, vmprot, VM_ANON);
    if (rc < 0)
        return rc;
    if (vaddr + len > mm->mmap_next)
        mm->mmap_next = vaddr + len;
    return (long)vaddr;
}

static long sc_munmap(struct syscall_regs *r)
{
    struct address_space *mm = syscall_user_mm();
    u64 addr = arg0(r);
    u64 len = arg1(r);
    u64 end;

    if (!mm)
        return -EINVAL;
    if (len == 0 || (addr & (PAGE_SIZE - 1)))
        return -EINVAL;
    len = ALIGN_UP(len, PAGE_SIZE);
    if (addr + len < addr)
        return -EINVAL;
    end = addr + len;
    if (!mm_find_vma(mm, (virt_addr_t)addr))
        return -EINVAL;
    return mm_remove_vma(mm, (virt_addr_t)addr, (virt_addr_t)end);
}

static long sc_mprotect(struct syscall_regs *r)
{
    struct address_space *mm = syscall_user_mm();
    u64 addr = arg0(r);
    u64 len = arg1(r);
    u64 prot = arg2(r);
    struct vma *v;
    uint32_t vmprot = VM_USER;

    if (!mm)
        return -EINVAL;
    if (len == 0 || (addr & (PAGE_SIZE - 1)))
        return -EINVAL;
    if (prot & ~(u64)(PROT_READ | PROT_WRITE | PROT_EXEC))
        return -EINVAL;
    /* W^X enforcement: reject simultaneous write + execute. */
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC))
        return -EINVAL;
    len = ALIGN_UP(len, PAGE_SIZE);
    if (addr + len < addr)
        return -EINVAL;

    if (prot & PROT_READ)
        vmprot |= VM_READ;
    if (prot & PROT_WRITE)
        vmprot |= VM_WRITE;
    if (prot & PROT_EXEC)
        vmprot |= VM_EXEC;

    /*
     * Only a range that lies inside a single VMA is changed. Splitting a
     * VMA is a five-line addition to the VMM, which owns the list; until
     * that exists, a straddling range is refused rather than silently
     * applied to the wrong region.
     */
    u64 flags = spinlock_irqsave(&mm->lock);

    v = mm_find_vma(mm, (virt_addr_t)addr);
    if (!v || addr < v->start || addr + len > v->end) {
        spinlock_unlock_irqrestore(&mm->lock, flags);
        return -EINVAL;
    }
    u32 old_prot = v->prot;
    u32 old_flags = v->flags;
    virt_addr_t start = v->start;
    virt_addr_t end = v->end;
    spinlock_unlock_irqrestore(&mm->lock, flags);

    if (old_prot == vmprot)
        return 0;

    {
        int rc = mm_remove_vma(mm, (virt_addr_t)addr,
                       (virt_addr_t)(addr + len));

        if (rc < 0)
            return rc;
    }
    /* Re-add with the new protection. If that fails the original mapping
     * is put back, so a failed mprotect leaves the process exactly as it
     * was rather than with a hole where its code used to be. */
    {
        int rc = mm_add_vma(mm, (virt_addr_t)addr,
                   (virt_addr_t)(addr + len), vmprot, old_flags);

        if (rc < 0) {
            mm_add_vma(mm, start, end, old_prot, old_flags);
            return rc;
        }
    }
    return 0;
}

/* ----------------------------------------------------- thread-local storage -- */

/*
 * arch_prctl(ARCH_SET_FS, addr) is how a process installs a thread pointer,
 * and it is the whole mechanism: without it a `__thread` access is a load from
 * MSR_FS_BASE + a constant, and nothing in ring 3 can move that register.
 *
 * Nothing is validated about the block `addr` describes, deliberately. Every
 * access through it goes through the MMU like any other access from the
 * process, with the same permissions, so setting the base grants no authority
 * the process did not already have -- and this kernel's own use of %gs (the
 * per-CPU area) is nowhere near %fs. What *is* checked is that the base is a
 * user address, because that is the one mistake with no recovery: FS is a
 * per-CPU register, so a base that faults leaves the process unable to run at
 * all until something else sets it, and the something else is a libc that has
 * already faulted.
 */
static bool fs_base_acceptable(u64 addr)
{
    /* The user half. A thread pointer names memory the process can name. */
    if (addr >= VMM_KERNEL_BASE)
        return false;
    /*
     * Canonical, in the four-level sense this kernel builds page tables in:
     * bit 47 has to be a copy of bits 63:48. A non-canonical base is not a
     * wrong address, it is a class of address for which the CPU raises #GP
     * on every access, so accepting one turns a rejected call into a dead
     * process.
     */
    if ((addr >> 47) != 0 && (addr >> 47) != 0x1FFFFULL)
        return false;
    return true;
}

static long sc_arch_prctl(struct syscall_regs *r)
{
    struct task *t = caller();
    u64 code = arg0(r);
    u64 addr = arg1(r);

    if (!t)
        return -ESRCH;

    switch (code) {
    case ARCH_SET_FS: {
        if (!fs_base_acceptable(addr))
            return -EPERM;
        wrmsr(MSR_FS_BASE, addr);
        /*
         * Recorded as well as written, because the register is per-CPU and
         * this value is per-task. The scheduler moves it across a switch
         * (see task_load_fs_base); a task whose thread pointer is only in
         * the register loses it the moment another task runs.
         */
        t->fs_base = addr;
        return 0;
    }
    case ARCH_GET_FS: {
        u64 val = rdmsr(MSR_FS_BASE);

        long rc = copy_to_user(addr, &val, sizeof(val));
        if (rc < 0)
            return -EINVAL;
        return 0;
    }
    default:
        /*
         * ARCH_SET_GS and ARCH_GET_GS are refused on purpose, and the
         * number is Linux's so the refusal is visible to anything ported.
         * %gs is where this kernel keeps its per-CPU area, read through
         * the hidden GS base by every this_cpu(); a process that wrote
         * the visible one would not break its own access to the register
         * so much as make every per-CPU read in ring 0 -- this_cpu_id(),
         * the scheduler's run queues, the syscall entry path's own
         * kstack lookup -- resolve to whatever the process chose.
         * Linux disables ARCH_SET_GS for a related reason on newer
         * kernels.
         */
        return -EINVAL;
    }
}

/*
 * The thread pointer this process's image was loaded with, or 0 if the image
 * has no PT_TLS.
 *
 * A query rather than an accessor for a struct field because the answer belongs
 * to the address space, and the address space is what the ELF loader was
 * handed: exec builds a new mm and a task can outlive several of them, so
 * anything cached on the task would be describing an image that is no longer
 * mapped. See the SYS_get_tls_base comment in uapi/syscall.h for the
 * arithmetic libc does with the answer.
 */
static long sc_get_tls_base(struct syscall_regs *r)
{
    struct address_space *mm = syscall_user_mm();

    UNUSED(r);
    if (!mm)
        return -ENOSYS;
    return (long)mm->tls_ptr;
}

/* ----------------------------------------------------------------- mremap -- */

static long sc_mremap(struct syscall_regs *r)
{
    struct address_space *mm = syscall_user_mm();
    u64 old_addr = arg0(r);
    u64 old_size = arg1(r);
    u64 new_size = arg2(r);
    u64 flags = arg3(r);
    int rc;

    if (!mm)
        return -EFAULT;
    if (!old_size || !new_size || (old_addr & (PAGE_SIZE - 1)) ||
        old_addr + old_size < old_addr)
        return -EINVAL;
    if (flags & ~(u64)0x0FU)
        return -EINVAL;

    old_size = ALIGN_UP(old_size, PAGE_SIZE);
    new_size = ALIGN_UP(new_size, PAGE_SIZE);
    if (old_addr + old_size < old_addr || old_addr + new_size < old_addr)
        return -EINVAL;

    u64 irqflags = spinlock_irqsave(&mm->lock);
    struct vma *v = mm_find_vma(mm, (virt_addr_t)old_addr);
    bool valid = v && old_addr >= v->start && old_addr + old_size <= v->end;
    u32 prot = valid ? v->prot : 0;
    u32 vflags = valid ? v->flags : 0;
    spinlock_unlock_irqrestore(&mm->lock, irqflags);
    if (!valid)
        return -EINVAL;

    /* Keep the mapping in place. Shrinking drops only the tail; growing adds
     * an adjacent anonymous range, preserving all existing mapped pages. */
    if (new_size < old_size) {
        rc = mm_remove_vma(mm, (virt_addr_t)(old_addr + new_size),
                           (virt_addr_t)(old_addr + old_size));
        return rc < 0 ? rc : (long)old_addr;
    }
    if (new_size > old_size) {
        rc = mm_add_vma(mm, (virt_addr_t)(old_addr + old_size),
                        (virt_addr_t)(old_addr + new_size), prot, vflags);
        if (rc < 0)
            return rc;
    }
    return (long)old_addr;
}

/* ---------------------------------------------------------------- mlock -- */

static long sc_mlock(struct syscall_regs *r)
{
    struct address_space *mm = syscall_user_mm();
    u64 addr = arg0(r);
    u64 len = arg1(r);

    if (!mm)
        return -EFAULT;
    if (!len || !user_range_ok(mm, addr, len))
        return -EINVAL;
    u64 irqflags = spinlock_irqsave(&mm->lock);
    struct vma *v = mm_find_vma(mm, (virt_addr_t)addr);
    bool valid = v && addr + len >= addr && addr + len <= v->end;
    spinlock_unlock_irqrestore(&mm->lock, irqflags);
    if (!valid)
        return -EINVAL;
    /* mlock is a no-op in this kernel; accept valid user ranges. */
    return 0;
}

/* ---------------------------------------------------------------- clone -- */

static long sc_clone(struct syscall_regs *r)
{
    struct fork_regs fr;
    fork_regs_from_frame(&fr, r);
    return sys_clone(arg0(r), arg1(r), arg2(r), arg3(r), arg4(r),
                     r->rip, r->rsp, r->rflags, &fr);
}

/* ------------------------------------------------------------------- getpgid --- */

static long sc_getpgid(struct syscall_regs *r)
{
    struct task *t = caller();
    s64 pid = (s64)arg0(r);

    UNUSED(r);
    if (pid == 0)
        return t ? (long)t->pgid : 0;
    struct task *o = task_find_by_pid((u32)pid);

    return o ? (long)o->pgid : -ESRCH;
}

static long sc_setpgid(struct syscall_regs *r)
{
    struct task *t = caller();
    s64 pid = (s64)arg0(r);
    s64 pgid = (s64)arg1(r);
    struct task *target;

    UNUSED(r);
    if (!t)
        return -ESRCH;
    if (pid == 0)
        pid = (s64)t->pid;
    target = task_find_by_pid((u32)pid);
    if (!target)
        return -ESRCH;
    if (pgid == 0)
        pgid = pid;
    /* Allow setting pgid only for self or children in the same session. */
    if (target != t && target->parent != t && target->sid != t->sid)
        return -EPERM;
    target->pgid = (u32)pgid;
    return 0;
}

static long sc_setsid(struct syscall_regs *r)
{
    struct task *t = caller();

    UNUSED(r);
    if (!t)
        return -EPERM;
    /* A process group leader (pgid == pid) may not start a new session. */
    if (t->pgid == t->pid)
        return -EPERM;
    t->pgid = t->pid;
    t->sid = t->pid;
    return (long)t->sid;
}

static long sc_kill(struct syscall_regs *r)
{
    return sys_kill((s64)arg0(r), (s64)arg1(r));
}

static long sc_rt_sigreturn(struct syscall_regs *r)
{
    return sys_rt_sigreturn(r);
}

static long sc_getuid(struct syscall_regs *r)
{
    struct task *t = caller();

    UNUSED(r);
    return t ? (long)t->cred.uid : 0;
}

static long sc_getgid(struct syscall_regs *r)
{
    struct task *t = caller();

    UNUSED(r);
    return t ? (long)t->cred.gid : 0;
}

static long sc_madvise(struct syscall_regs *r)
{
    u64 advice = arg2(r);
    if (advice != 0 && advice != 4) /* MADV_NORMAL, MADV_DONTNEED */
        return -EINVAL;
    return 0;
}

/* ======================================================================= */

long syscall_dispatch(struct syscall_regs *regs)
{
    /*
     * The bound is checked here rather than in the assembly, which keeps
     * the entry path to a straight line and puts the check next to the
     * table it protects. A garbage RAX lands here and leaves as -ENOSYS
     * without ever indexing the table.
     */
    if (!regs)
        return -ENOSYS;

    if (regs->rax >= SYSCALL_MAX) {
        struct task *t = current_task();
        SYSCALL_LOG(KLOG_WARN, "unknown syscall %llu from pid %u (%s): out of range, returning ENOSYS",
                    (unsigned long long)regs->rax, t ? t->pid : 0, t ? t->comm : "?");
        return -ENOSYS;
    }

    long (*fn)(struct syscall_regs *) = syscall_table[regs->rax];

    if (!fn) {
        struct task *t = current_task();
        SYSCALL_LOG(KLOG_WARN, "NOT IMPLEMENTED: syscall %llu requested by pid %u (%s), returning ENOSYS",
                    (unsigned long long)regs->rax, t ? t->pid : 0, t ? t->comm : "?");
        return -ENOSYS;
    }

    /*
     * Set ret_to_user = 1 before any early returns. This ensures the
     * invariant that the syscall entry path can rely on ret_to_user being
     * set for any syscall that reaches a handler, rather than depending
     * on the historical absence of early returns.
     */
    syscall_cpus[this_cpu_id()].ret_to_user = 1;

    struct task *t = current_task();

    if (t) {
        /*
         * Mirror the same fact on the task. The per-CPU slot is what the
         * entry path reads, but it is this task's attribute, and a context
         * switch has to be able to give it back after some other task has
         * used the CPU in between.
         */
        t->in_syscall_frame = true;

        /*
         * Mark the task as FPU-dirty when it is about to go back to
         * user mode with CR0.TS clear, which is exactly the condition
         * under which the next context switch has to save its x87/SSE
         * image. Doing it here rather than in the switch keeps the
         * save off the common path for kernel threads, which never
         * touch the FPU.
         */
        t->fpu_dirty = (read_cr0() & CR0_TS) == 0;
    }
    /*
     * The return value goes into the register frame first, so that a signal
     * frame built from it saves the value the caller is going to see.
     * rt_sigreturn is the one handler whose return value is not its own: it
     * returns the rax the interrupted code had, and that is what gets stored.
     */
    {
        long ret = fn(regs);

        regs->rax = (u64)ret;
        signal_deliver_syscall(regs);
        return ret;
    }
}

void syscall_init(void)
{
    for (u32 i = 0; i < SYSCALL_MAX; i++)
        syscall_table[i] = NULL;

    syscall_table[SYS_fork] = sc_fork;
    syscall_table[SYS_execve] = sc_execve;
    syscall_table[SYS_exit] = sc_exit;
    syscall_table[SYS_exit_group] = sc_exit_group;
    syscall_table[SYS_wait4] = sc_wait4;
    syscall_table[SYS_getpid] = sc_getpid;
    syscall_table[SYS_getppid] = sc_getppid;
    syscall_table[SYS_gettid] = sc_gettid;
    syscall_table[SYS_getpgid] = sc_getpgid;
    syscall_table[SYS_clone] = sc_clone;
    syscall_table[SYS_kill] = sc_kill;
    syscall_table[SYS_setpgid] = sc_setpgid;
    syscall_table[SYS_setsid] = sc_setsid;
    syscall_table[SYS_getuid] = sc_getuid;
    syscall_table[SYS_getgid] = sc_getgid;
    syscall_table[SYS_arch_prctl] = sc_arch_prctl;
    syscall_table[SYS_nanosleep] = sc_nanosleep;
    syscall_table[SYS_clock_gettime] = sc_clock_gettime;
    syscall_table[SYS_open] = sc_open;
    syscall_table[SYS_close] = sc_close;
    syscall_table[SYS_read] = sc_read;
    syscall_table[SYS_write] = sc_write;
    syscall_table[SYS_lseek] = sc_lseek;
    syscall_table[SYS_fstat] = sc_fstat;
    syscall_table[SYS_ioctl] = sc_ioctl;
    syscall_table[SYS_fcntl] = sc_fcntl;
    syscall_table[SYS_getdents] = sc_getdents;
    syscall_table[SYS_dup] = sc_dup;
    syscall_table[SYS_dup3] = sc_dup3;
    syscall_table[SYS_mmap] = sc_mmap;
    syscall_table[SYS_munmap] = sc_munmap;
    syscall_table[SYS_mprotect] = sc_mprotect;
    syscall_table[SYS_brk] = sc_brk;
    syscall_table[SYS_madvise] = sc_madvise;
    syscall_table[SYS_get_tls_base] = sc_get_tls_base;
    syscall_table[SYS_pipe] = sc_pipe;
    syscall_table[SYS_futex] = sc_futex;
    syscall_table[SYS_sched_yield] = sc_sched_yield;
    syscall_table[SYS_getcpu] = sc_getcpu;
    syscall_table[SYS_screen_claim] = sc_screen_claim;
    syscall_table[SYS_screen_release] = sc_screen_release;
    syscall_table[SYS_gfx_info] = sc_gfx_info;
    syscall_table[SYS_gfx_present] = sc_gfx_present;
    syscall_table[SYS_mremap] = sc_mremap;
    syscall_table[SYS_mlock] = sc_mlock;
    syscall_table[SYS_shutdown] = sc_shutdown;
    syscall_table[SYS_mouse_info] = sc_mouse_info;
    syscall_table[SYS_kbd_typematic] = sc_kbd_typematic;
    syscall_table[SYS_kbd_state] = sc_kbd_state;
    syscall_table[SYS_uname] = sc_uname;
    syscall_table[SYS_sysinfo] = sc_sysinfo;
    syscall_table[SYS_getrlimit] = sc_getrlimit;
    syscall_table[SYS_setrlimit] = sc_setrlimit;
    syscall_table[SYS_rt_sigaction] = sc_rt_sigaction;
    syscall_table[SYS_rt_sigprocmask] = sc_rt_sigprocmask;
    syscall_table[SYS_rt_sigpending] = sc_rt_sigpending;
    syscall_table[SYS_rt_sigreturn] = sc_rt_sigreturn;

    /*
     * STAR carries both selector bases. SYSCALL loads CS from bits 47:32
     * and SS from that plus 8; SYSRET loads CS from bits 63:48 and SS
     * from that plus 8. The +8 and +16 in the SYSRET case are why the
     * user selectors are chosen so that USER_CODE_SELECTOR + 8 is the
     * user data descriptor: the CPU computes the SS selector arithmetically
     * and there is no way to tell it otherwise.
     */
    u64 star = ((u64)USER_CODE_SELECTOR << 48) |
           ((u64)KERNEL_CODE_SELECTOR << 32) | 2ULL;

    /* Bits 0 and 1 of SYSRET are reserved and must read as 1. */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
    wrmsr(MSR_STAR, star);
    wrmsr(MSR_LSTAR, (u64)syscall_entry);
    /*
     * SFMASK clears interrupt flag, direction flag, alignment check and
     * trap flag on entry. IF because the handler runs with interrupts off
     * and re-enables them deliberately; DF because the ABI does not
     * define the direction on entry and a kernel that assumed one would
     * corrupt its own string operations; AC because an unaligned user
     * access must fault, not be silently fixed up; TF because a stray
     * single-step trap out of a syscall is never what anyone wanted.
     */
    wrmsr(MSR_SFMASK, (1ULL << 9) | (1ULL << 10) | (1ULL << 16) |
                    (1ULL << 8));

    /* A stack to land on before any task exists. */
    syscall_cpus[this_cpu_id()].ret_to_user = 1;
    syscall_set_kernel_stack(task_bootstrap_stack_top());

    SYSCALL_LOG(KLOG_INFO, "syscall entry installed at %#lx",
            (u64)syscall_entry);
}
