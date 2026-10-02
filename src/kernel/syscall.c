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
#include <syscall.h>

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

static long sc_exit(struct syscall_regs *r)
{
	sys_exit((int)(s32)arg0(r));
	return 0;
}

static long sc_exit_group(struct syscall_regs *r)
{
	sys_exit_group((int)(s32)arg0(r));
	return 0;
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

static long sc_fork(struct syscall_regs *r)
{
	/*
	 * The child has to resume at the instruction after the syscall, which
	 * is only knowable from the register frame the entry path built. It is
	 * published before the fork so sys_fork() can copy it into the new
	 * task, which has no frame of its own to inherit.
	 */
	process_set_fork_context(r->rip, r->rsp, r->rflags);
	return sys_fork();
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
	return copy_to_user(arg0(r), &pair, sizeof(pair));
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

	UNUSED(r);
	/* Both clocks are the same counter. There is no RTC driver, so
	 * CLOCK_REALTIME is uptime; the difference matters only to a program
	 * that compares the two. */
	{
		/*
		 * Read the clock once.
		 *
		 * Two reads, one per field, can straddle a tick. The second read is
		 * the later one, so if it lands in the next second the pair
		 * reconstructs as sec = N, nsec = (small), which is *earlier* than
		 * the N-1 that the previous call returned. A timestamp that goes
		 * backwards is worse than an imprecise one: init's uptime computes
		 * `ms = (now - last) * 1e6 / 1e3` as unsigned, so one backwards step
		 * underflows and prints a number in the billions of milliseconds.
		 */
		uint64_t now = sched_now_ns();

		ts.tv_sec = (s64)(now / 1000000000ULL);
		ts.tv_nsec = (s64)(now % 1000000000ULL);
	}
	return copy_to_user(arg1(r), &ts, sizeof(ts));
}

/* ------------------------------------------------------------- files ------- */

static long sc_open(struct syscall_regs *r)
{
	char path[256];

	UNUSED(r);
	/*
	 * Validated even though the answer is always the same. A syscall that
	 * ignores its arguments cannot be used to probe for a valid pointer,
	 * and more importantly the validation is what tells a caller its
	 * pointer was wrong instead of silently reporting "no such file" for
	 * an address that does not exist.
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
 * Shared read/write body. The bounce buffer is on the kernel stack rather than
 * a per-CPU static for one reason that is not performance: a static would be a
 * single shared object, and two CPUs servicing a read and a write at the same
 * time would tear each other's data. A per-CPU array of 256 pages would not fit
 * in the kernel image budget.
 */
#define IO_CHUNK PAGE_SIZE

static long file_read(struct task *t, int fd, u64 buf, size_t count)
{
	struct file *f = fd_lookup(t, fd);
	ssize_t total = 0;

	if (!f)
		return -EBADF;
	if (!f->ops || !f->ops->read)
		return -EBADF;
	if ((f->f_flags & O_ACCMODE) == O_WRONLY)
		return -EBADF;
	if (count == 0)
		return 0;
	if (!user_range_ok(t->mm, buf, count))
		return -EFAULT;

	u8 bounce[IO_CHUNK];

	while (count) {
		size_t chunk = count > IO_CHUNK ? IO_CHUNK : count;
		ssize_t n = f->ops->read(f, bounce, chunk);

		if (n < 0)
			return (long)n;
		if (n == 0)
			break;
		/* Read into the bounce buffer first and only then into the
		 * user page, so a short tty read cannot leave a partial,
		 * unvalidated copy behind. */
		long rc = copy_to_user(buf + (u64)total, bounce, (size_t)n);

		if (rc < 0)
			return rc;
		total += n;
		count -= (size_t)n;
		if ((size_t)n < chunk)
			break;
	}
	return total;
}

static long file_write(struct task *t, int fd, u64 buf, size_t count)
{
	struct file *f = fd_lookup(t, fd);
	ssize_t total = 0;

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

	u8 bounce[IO_CHUNK];

	while (count) {
		size_t chunk = count > IO_CHUNK ? IO_CHUNK : count;
		long rc = copy_from_user(bounce, buf + (u64)total, chunk);

		if (rc < 0)
			return rc;
		ssize_t n = f->ops->write(f, bounce, chunk);

		if (n < 0)
			return (long)n;
		if (n == 0)
			break;
		total += n;
		count -= (size_t)n;
		if ((size_t)n < chunk)
			break;
	}
	return total;
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

	UNUSED(r);
	if (!f)
		return -EBADF;
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
	st.mode = 0020000u | 0666u;	/* S_IFCHR | rw-rw-rw- */
	st.nlink = 1;
	st.blksize = PAGE_SIZE;
	st.ino = f->ino;
	/* A terminal has no length. Reporting 0 is what makes isatty() and
	 * the stdio size checks behave. */
	st.size = 0;
	return copy_to_user(arg1(r), &st, sizeof(st));
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
	refcount_set(&p->refs, 2);	/* two ends */
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
	size_t done = 0;
	bool nonblock = (f->f_flags & O_NONBLOCK) != 0;

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
			return (ssize_t)done;
		}
		if (p->write_closed) {
			spinlock_release(&p->lock);
			return 0;	/* EOF */
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
	size_t done = 0;
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
			return (ssize_t)done;
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
	return copy_to_user(arg0(r), &pair, sizeof(pair));
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

	if (!mm)
		return -EINVAL;
	if (len == 0 || (addr & (PAGE_SIZE - 1)))
		return -EINVAL;
	len = ALIGN_UP(len, PAGE_SIZE);
	if (addr + len < addr)
		return -EINVAL;
	return mm_remove_vma(mm, (virt_addr_t)addr, (virt_addr_t)(addr + len));
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
		return -ENOMEM;
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

		return copy_to_user(addr, &val, sizeof(val));
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

/* ---------------------------------------------------------------- misc ----- */

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
	UNUSED(r);
	/* Advice is advisory by definition, and every value this kernel
	 * understands means "do nothing"; rejecting them would make a libc
	 * that calls it on every allocation fail. */
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
	if (!regs || regs->rax >= SYSCALL_MAX)
		return -ENOSYS;

	long (*fn)(struct syscall_regs *) = syscall_table[regs->rax];

	if (!fn)
		return -ENOSYS;

	/*
	 * Cleared before every dispatch and only cleared again by a handler
	 * that is going to switch away for good. The entry path consults it
	 * to decide whether its frame is still the right place to sysretq to.
	 */
	syscall_cpus[this_cpu_id()].ret_to_user = 1;

	struct task *t = current_task();

	if (t) {
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
	return fn(regs);
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
	syscall_table[SYS_getdents] = sc_getdents;
	syscall_table[SYS_dup] = sc_dup;
	syscall_table[SYS_mmap] = sc_mmap;
	syscall_table[SYS_munmap] = sc_munmap;
	syscall_table[SYS_mprotect] = sc_mprotect;
	syscall_table[SYS_brk] = sc_brk;
	syscall_table[SYS_madvise] = sc_madvise;
	syscall_table[SYS_get_tls_base] = sc_get_tls_base;
	syscall_table[SYS_pipe] = sc_pipe;
	syscall_table[SYS_sched_yield] = sc_sched_yield;
	syscall_table[SYS_getcpu] = sc_getcpu;

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
