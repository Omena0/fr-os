/*
 * process.c — file objects, descriptors, user memory access, and the process
 * lifecycle syscalls.
 *
 * Two things live here that a reader might not expect.
 *
 * First, the user-pointer access path. Nothing in this kernel dereferences an
 * address a user process supplied without going through user_range_ok() and a
 * page-by-page vmm_translate(). A pointer is data, and treating it as data is
 * the difference between a kernel that survives a hostile program and one that
 * does not. The page walk also materialises a page that is mapped but not
 * present, because "the user wrote to a byte in a page it has not touched yet"
 * is ordinary behaviour, not an attack.
 *
 * Second, fork is an eager page-table copy rather than copy-on-write. See
 * sys_fork() for the reasoning; the short version is that a real COW
 * implementation needs a write-fault hook in a file this subsystem does not
 * own, and a slower correct fork beats a fast one that corrupts memory.
 */
#include <stdbool.h>
#include <io.h>
#include <types.h>
#include <list.h>
#include <spinlock.h>
#include <klog.h>
#include <panic.h>
#include <kstring.h>
#include <percpu.h>
#include <pmm.h>
#include <task.h>
#include <sched.h>
#include <process.h>
#include <uapi/syscall.h>
#include <uapi/errno.h>
#include "console.h"

#define PROC_LOG(level, ...)                                                 \
	do {                                                               \
		if ((level) >= klog_runtime_level)                         \
			klog_emit((level), "process", __VA_ARGS__);        \
	} while (0)

/*
 * The user half of the address space. Anything at or above this is either
 * kernel, non-canonical, or the direct map, and none of it is reachable from
 * ring 3, so an address here is a necessary (not sufficient) condition for a
 * user pointer to be plausible.
 */
#define USER_ADDRESS_MAX 0x0000800000000000ULL

/* User RFLAGS for a fresh process: IF set, bit 1 reserved, the rest clear. */
#define USER_DEFAULT_RFLAGS 0x202ULL

/* Where the initial user stack lives, and how big it is. The top is the
 * canonical boundary minus one page so that a push that runs off the end
 * faults instead of wrapping into non-canonical space. */
#define USER_STACK_TOP    0x00007FFFFFFFF000ULL
#define USER_STACK_SIZE   (8ULL * 1024 * 1024)
#define USER_STACK_BASE   (USER_STACK_TOP - USER_STACK_SIZE)

/* Limits on what a user process can hand the kernel in one execve. */
#define MAX_EXEC_ARGS  16
#define MAX_ARG_LEN    128

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
	file_put(f);
	return 0;
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
	}
}

/* Give a task the console on 0/1/2. Every process starts with one, and a
 * forked task shares its parent's by reference. */
static int setup_console_fds(struct task *t)
{
	for (int i = 0; i < 3; i++) {
		struct file *f = console_file_create();

		if (!f)
			return -ENOMEM;
		t->fds[i] = f;
	}
	return 0;
}

/* -------------------------------------------------------- user memory ------- */

u64 user_range_end(u64 addr, size_t len)
{
	if (len == 0)
		return addr;
	if (len > USER_ADDRESS_MAX - addr)
		return USER_ADDRESS_MAX;
	return addr + len;
}

bool user_range_ok(struct address_space *mm, u64 addr, size_t len)
{
	if (!mm)
		return false;
	if (len == 0)
		return true;
	/*
	 * Two conditions in one comparison: `addr` must be below the user
	 * boundary, and the whole range must fit under it. A length that
	 * overflows u64 also fails the second test, so there is no separate
	 * wrap check and no way for addr+len to wrap onto a valid-looking
	 * address.
	 */
	if (addr >= USER_ADDRESS_MAX)
		return false;
	if (len > USER_ADDRESS_MAX - addr)
		return false;
	return true;
}

/*
 * Resolve one user page to a kernel pointer, faulting it in if it is mapped but
 * not present. Returns NULL if the page belongs to no VMA.
 *
 * `write` selects the fault error code: a copy out of user space only needs the
 * page readable, while a copy in needs it writable. Getting this wrong would
 * let a read fault pre-fault a page the process has no write permission on,
 * which is a smaller hole but still a hole.
 */
static void *user_page(struct address_space *mm, virt_addr_t vaddr, bool write)
{
	/*
	 * Bound-check before translating, not after.
	 *
	 * A process PML4 carries the direct map at PML4[256], so the four-level
	 * walk happily resolves a *user* address such as 0x800000001000: PML4 index
	 * 256 lands in the direct map and the translation succeeds, returning
	 * physical 0x1000. phys_to_virt() then hands the caller a kernel pointer
	 * to a frame the process does not own, and every user-supplied address
	 * that reaches here without a prior range check is a read or write of
	 * arbitrary kernel-reachable memory.
	 *
	 * `vmm_translate` cannot do this check itself: it is also used to walk
	 * kernel addresses, which are of course far above USER_ADDRESS_MAX. The
	 * user half has to be policed where the user half is known, which is here.
	 *
	 * The callers that validate ranges already did, so this is a second line
	 * of defence -- but it is the one that does not depend on every future
	 * caller remembering. Both routes that reach user_page() with an
	 * ELF-controlled or syscall-controlled address rely on some other check
	 * today, and a check that depends on every caller being careful is not
	 * a check.
	 */
	if (vaddr >= USER_ADDRESS_MAX)
		return NULL;

	phys_addr_t phys = 0;

	/*
	 * vmm_lookup_page(), not vmm_translate(). The latter returns 0 for "not
	 * translated", which is indistinguishable from a legitimate mapping of
	 * physical frame 0 -- and a caller testing it for non-zero is reading a
	 * *physical address* as a *presence flag*. Frame 0 is not mapped today, so
	 * the two have not yet collided, but this is a user-supplied address and
	 * the cost of being wrong is reading a kernel frame.
	 */
	if (!vmm_lookup_page(mm->pgd, vaddr, &phys, NULL))
		return NULL;

	return phys_to_virt(phys);

	uint64_t error = PTE_PRESENT | PTE_USER;

	if (write)
		error |= PTE_WRITE;

	/* `mm`, not the current address space: this runs from the ELF loader
	 * during execve, where the image is being built in a new address space
	 * while the CPU is still on the old PML4. */
	if (vmm_handle_page_fault(mm, vaddr, error) != 0)
		return NULL;

	if (!vmm_lookup_page(mm->pgd, vaddr, &phys, NULL))
		return NULL;
	return phys_to_virt(phys);
}

/*
 * The one copy primitive. `to_user` selects the direction; everything else —
 * range validation, page-by-page translation, the fault-in — is shared, so
 * there is no second path that could forget a check.
 */
static long user_copy(struct address_space *mm, void *kbuf, u64 user,
		      const void *kother, size_t n, bool to_user)
{
	if (!user_range_ok(mm, user, n))
		return -EFAULT;
	if (n == 0)
		return 0;

	size_t done = 0;

	while (done < n) {
		u64 cur = user + done;
		virt_addr_t vaddr = (virt_addr_t)ALIGN_DOWN(cur, PAGE_SIZE);
		size_t off = (size_t)(cur & (PAGE_SIZE - 1));
		size_t chunk = PAGE_SIZE - off;

		if (chunk > n - done)
			chunk = n - done;

		u8 *page = user_page(mm, vaddr, to_user);

		if (!page) {
			/*
			 * A page that will not fault in is not an error when
			 * reading: an untouched byte of an anonymous mapping
			 * reads as zero, and forcing the fault is what makes
			 * that true on a lazy-mapping kernel too. Only a copy
			 * *into* such a page is a real fault.
			 */
			if (to_user)
				return -EFAULT;
			memset((u8 *)kbuf + done, 0, chunk);
			done += chunk;
			continue;
		}

		/*
		 * Direction matters, and both halves are wrong without it.
		 *
		 *   to_user   : kbuf is the kernel *source*, the page is the user
		 *               destination  ->  page <- kbuf
		 *   !to_user  : kbuf is the kernel *destination*, the page is the
		 *               user source  ->  kbuf <- page
		 *
		 * The !to_user branch used `memcpy(page + off, kother + done)`,
		 * which copies *into* the user page from kother and never touches
		 * the kernel destination at all. copy_from_user passes kother =
		 * NULL, so every byte came from address `done` -- 0, 1, 2, ... --
		 * and the caller's buffer was left holding whatever was already in
		 * it. A read() that "succeeded" would return uninitialised stack.
		 *
		 * The kernel destination being untouched is why this never showed up
		 * as a fault: nothing is ever read from an unmapped address except
		 * the first byte or two, and the write target is always valid.
		 */
		if (to_user)
			memcpy((u8 *)page + off, (const u8 *)kbuf + done, chunk);
		else
			memcpy((u8 *)kbuf + done, page + off, chunk);
		done += chunk;
	}
	return (long)done;
}

long copy_from_user(void *dst, u64 src, size_t n)
{
	struct task *t = current_task();

	if (!t || !t->mm)
		return -EFAULT;
	return user_copy(t->mm, dst, src, NULL, n, false);
}

long copy_to_user(u64 dst, const void *src, size_t n)
{
	struct task *t = current_task();

	if (!t || !t->mm)
		return -EFAULT;
	return user_copy(t->mm, (void *)src, dst, src, n, true);
}

long copy_string_from_user(char *dst, u64 src, size_t max)
{
	if (max == 0)
		return -EFAULT;
	struct task *t = current_task();

	if (!t || !t->mm)
		return -EFAULT;
	if (src >= USER_ADDRESS_MAX)
		return -EFAULT;

	size_t i = 0;

	while (i < max - 1) {
		u64 page = ALIGN_DOWN(src + i, PAGE_SIZE);
		size_t off = (size_t)((src + i) & (PAGE_SIZE - 1));
		u8 *kp = user_page(t->mm, (virt_addr_t)page, false);

		if (!kp)
			return -EFAULT;
		char c = (char)kp[off];

		dst[i] = c;
		if (c == '\0')
			return (long)i;
		i++;
	}
	/* Ran out of room without finding a terminator: the caller's buffer is
	 * too small for the string, which is a bounds failure, not a fault. */
	dst[i] = '\0';
	return -ENAMETOOLONG;
}

long user_map_zero_page(struct address_space *mm, virt_addr_t addr, uint32_t prot)
{
	struct page *p = pmm_alloc_page(GFP_KERNEL);

	if (!p)
		return -ENOMEM;

	phys_addr_t phys = page_to_phys(p);

	/* Zeroed here rather than trusting GFP_ZERO: the loader and the stack
	 * builder both write a whole page and then partially overwrite it, and a
	 * page that leaked the previous tenant's data is a real information
	 * leak even if nothing reads the tail today. */
	memset(phys_to_virt(phys), 0, PAGE_SIZE);

	int r = vmm_map_page(mm->pgd, addr, phys, prot | VM_USER);

	if (r < 0) {
		pmm_free_pages(p, 0);
		return r;
	}
	return 0;
}

long user_memory_write(struct address_space *mm, virt_addr_t addr,
		       const void *src, size_t len, uint32_t prot)
{
	size_t done = 0;

	/*
	 * The whole range must be in the user half before a single byte moves.
	 *
	 * This is called from the ELF loader with a `p_vaddr` straight out of the
	 * image, so the address is exactly as trustworthy as the file. Without
	 * this, a segment with a p_vaddr of 0x800000000000 reaches user_page(),
	 * lands on PML4[256], and is written through a direct-map pointer.
	 *
	 * Checking the whole range up front rather than per page is deliberate:
	 * a partial write that faults halfway leaves the process with some of an
	 * image mapped and some not, which is worse than refusing.
	 */
	if (!user_range_ok(mm, addr, len))
		return -EFAULT;

	while (done < len) {
		virt_addr_t vaddr = (virt_addr_t)ALIGN_DOWN(addr + done, PAGE_SIZE);
		size_t off = (size_t)((addr + done) & (PAGE_SIZE - 1));
		size_t chunk = PAGE_SIZE - off;

		if (chunk > len - done)
			chunk = len - done;

		u8 *page = user_page(mm, vaddr, true);

		if (!page) {
			long r = user_map_zero_page(mm, vaddr, prot);

			if (r < 0)
				return r;
			page = user_page(mm, vaddr, true);
			if (!page)
				return -EFAULT;
		}
		memcpy(page + off, (const u8 *)src + done, chunk);
		done += chunk;
	}
	return (long)done;
}

/* ----------------------------------------------------------------- exit ----- */

__noreturn void sys_exit(int code)
{
	task_exit_current(code);
	__builtin_unreachable();
}

__noreturn void sys_exit_group(int code)
{
	struct task *me = current_task();
	struct address_space *mm = me ? me->mm : NULL;

	/*
	 * Every other task sharing this address space dies too. In a
	 * single-threaded system that is a no-op, which is why it is written as
	 * a scan rather than special-cased: a CLONE_VM thread group is the
	 * normal case, and the scan is O(tasks) on a path that runs once per
	 * process.
	 */
	if (mm) {
		struct list_head *pos, *tmp;
		u64 flags = spinlock_irqsave(&task_all_lock);

		list_for_each_safe(pos, tmp, &task_all_list) {
			struct task *t = list_entry(pos, struct task, all_node);

			if (t == me || t->mm != mm || t->detached)
				continue;
			t->exit_code = code;
			t->state = TASK_ZOMBIE;
			t->detached = true;
			sched_remove(t);
			task_release_resources(t);
			if (t->parent && t->parent->state == TASK_BLOCKED)
				sched_wake(t->parent);
		}
		spinlock_unlock_irqrestore(&task_all_lock, flags);
	}
	task_exit_current(code);
	__builtin_unreachable();
}

/* ---------------------------------------------------------------- wait ------ */

static bool child_matches(struct task *child, s64 pid, u64 options)
{
	if (pid > 0 && (s64)child->pid != pid)
		return false;
	if (pid < -1) {
		/* A negative pid addresses a process group. */
		if ((s64)child->pgid != -pid)
			return false;
	}
	UNUSED(options);
	return true;
}

long sys_wait4(s64 pid, u64 wstatus, u64 options, u64 rusage)
{
	struct task *me = current_task();

	if (!me)
		return -ESRCH;
	if (rusage != 0)
		/* No accounting subsystem yet; claiming success would make a
		 * caller believe it got zeros it never received. */
		return -ENOSYS;

	for (;;) {
		struct task *reaped = NULL;

		u64 flags = spinlock_irqsave(&task_all_lock);
		struct list_head *pos;

		list_for_each(pos, &me->children) {
			struct task *c = list_entry(pos, struct task, sibling);

			if (c->state != TASK_ZOMBIE)
				continue;
			if (!child_matches(c, pid, options))
				continue;
			list_del(&c->sibling);
			list_init(&c->sibling);
			c->reaped = true;
			reaped = c;
			break;
		}
		spinlock_unlock_irqrestore(&task_all_lock, flags);

		if (reaped) {
			/*
			 * POSIX wait status for a normal exit: the low byte is
			 * the exit code and the next says "exited normally",
			 * which is what every libc's WIFEXITED looks for.
			 */
			int status = (reaped->exit_code & 0xff) << 8;
			u32 rp = (u32)reaped->pid;

			if (wstatus) {
				long rc = copy_to_user(wstatus, &status,
						       sizeof(status));

				if (rc < 0)
					return rc;
			}
			task_put(reaped);
			return (long)rp;
		}

		if (options & WNOHANG)
			return 0;

		/*
		 * Nothing to reap. Block; task_exit_current() wakes the parent
		 * of a task that exits, so the wakeup is the loop's only exit
		 * condition and no re-check race is possible — the re-check
		 * happens with the child list locked.
		 */
		sched_block_current();
	}
}

/* ---------------------------------------------------------------- fork ------ */

/*
 * Page-table cloning, used by fork.
 *
 * Trade-off, stated plainly: this copies every writable user page immediately
 * rather than marking it read-only and copying on the first write fault. The
 * eager version needs no cooperation from the page-fault path, which is owned
 * by the VMM subsystem and has no notion of which pages are shared, so it is
 * correct on its own. The cost is that a fork of a 2 GiB process copies 2 GiB
 * of page tables and page contents at the moment of the call, where COW would
 * have copied only the tables.
 *
 * The change that would fix it is small and localised: mark the PTEs
 * read-only in both address spaces and have a hook at the top of the #PF
 * handler do the copy when the write bit is clear. That hook is a single
 * function this file would export; it is deliberately absent rather than
 * present-and-wrong, because a COW marker that no fault handler honours
 * corrupts memory silently.
 */
static phys_addr_t clone_table(phys_addr_t src, unsigned level)
{
	struct page *np = pmm_alloc_page(GFP_KERNEL | GFP_ZERO);

	if (!np)
		return 0;

	phys_addr_t nphys = page_to_phys(np);
	u64 *dst = (u64 *)phys_to_virt(nphys);
	u64 *src_tbl = (u64 *)phys_to_virt(src);

	for (unsigned i = 0; i < 512; i++) {
		u64 e = src_tbl[i];

		if (!(e & PTE_PRESENT))
			continue;

		u64 child;

		if (level == 1 && !(e & PTE_PS)) {
			/* Leaf: a fresh frame with the same contents. */
			struct page *cp = pmm_alloc_page(GFP_KERNEL);

			if (!cp)
				return 0;
			child = page_to_phys(cp);
			memcpy(phys_to_virt(child), phys_to_virt(e & PTE_ADDR_MASK),
			       PAGE_SIZE);
		} else if (level == 2 && (e & PTE_PS)) {
			/* A 2 MiB leaf: the frame is 512 contiguous pages. */
			struct page *cp = pmm_alloc_pages(9, GFP_KERNEL);

			if (!cp)
				return 0;
			child = page_to_phys(cp);
			memcpy(phys_to_virt(child), phys_to_virt(e & PTE_ADDR_MASK),
			       2ULL * 1024 * 1024);
		} else {
			child = clone_table(e & PTE_ADDR_MASK, level - 1);
			if (!child)
				return 0;
		}

		/*
		 * The copied entry keeps the parent's protection bits, with
		 * PTE_USER forced on: every entry below the PML4's upper half
		 * is a user mapping by construction, and losing the bit here
		 * would make the child fault on its own text.
		 */
		dst[i] = (e & ~(PTE_ADDR_MASK | PTE_PRESENT)) | child |
			 PTE_PRESENT | PTE_USER;
	}
	return nphys;
}

static int clone_vmas(struct address_space *dst, struct address_space *src)
{
	/*
	 * Refused rather than handled. mm_add_vma() takes dst->lock while this
	 * holds src->lock, which is a self-deadlock when the two are the same
	 * object -- and spinlocks in this kernel are not recursive. Fork always
	 * builds a fresh address space today, so this is unreachable, but a
	 * future in-place reuse would hang the machine with interrupts off rather
	 * than fail, and a hang is much more expensive to diagnose than an error.
	 */
	if (dst == src)
		return -EINVAL;

	u64 flags = spinlock_irqsave(&src->lock);
	struct list_head *pos;
	int r = 0;

	list_for_each(pos, &src->vma_list) {
		struct vma *v = list_entry(pos, struct vma, list);

		r = mm_add_vma(dst, v->start, v->end, v->prot, v->flags);
		if (r < 0)
			break;
	}
	spinlock_unlock_irqrestore(&src->lock, flags);
	return r;
}

/* Where the parent was suspended, for the child to resume at. */
static u64 fork_ctx[MAX_CPUS][3];

void process_set_fork_context(u64 rip, u64 rsp, u64 rflags)
{
	fork_ctx[this_cpu_id()][0] = rip;
	fork_ctx[this_cpu_id()][1] = rsp;
	fork_ctx[this_cpu_id()][2] = rflags;
}

/*
 * The child's entry point. It does not run the parent's code path from the top;
 * it iretq's straight back to the user's syscall return site with RAX = 0.
 */
static __noreturn void fork_child_start(void *arg)
{
	struct task *t = arg;

	/*
	 * RAX = 0 is the entire contract of fork's return value in the child.
	 * Every other register is caller-saved across the syscall, so the
	 * parent's own code has already promised not to rely on them.
	 */
	t->thread_fn = NULL;
	t->thread_arg = NULL;
	ret_to_user(t->user_rip, t->user_rsp, t->user_rflags, 0, 0);
	__builtin_unreachable();
}

long sys_fork(void)
{
	struct task *me = current_task();
	struct address_space *child_mm;
	struct address_space *parent_mm;
	struct task *child;

	if (!me || !me->mm)
		return -EFAULT;

	parent_mm = me->mm;
	child_mm = mm_create();
	if (!child_mm)
		return -ENOMEM;

	/* Copy the user half of the page tables. The upper half is already
	 * correct: mm_create() seeded it with the kernel's own mappings. */
	{
		u64 *src = (u64 *)phys_to_virt(parent_mm->pgd);
		u64 *dst = (u64 *)phys_to_virt(child_mm->pgd);

		for (unsigned i = 0; i < 256; i++) {
			if (!(src[i] & PTE_PRESENT))
				continue;
			phys_addr_t p = clone_table(src[i] & PTE_ADDR_MASK, 3);

			if (!p) {
				mm_put(child_mm);
				return -ENOMEM;
			}
			dst[i] = (src[i] & ~(PTE_ADDR_MASK | PTE_PRESENT)) | p |
				 PTE_PRESENT | PTE_USER;
		}
	}

	if (clone_vmas(child_mm, parent_mm) < 0) {
		mm_put(child_mm);
		return -ENOMEM;
	}

	/* The bookkeeping fields the VMA list does not carry. */
	child_mm->start_brk = parent_mm->start_brk;
	child_mm->brk = parent_mm->brk;
	child_mm->stack_top = parent_mm->stack_top;
	child_mm->mmap_base = parent_mm->mmap_base;
	child_mm->mmap_next = parent_mm->mmap_next;
	child_mm->start_code = parent_mm->start_code;
	child_mm->end_code = parent_mm->end_code;
	child_mm->start_data = parent_mm->start_data;
	child_mm->end_data = parent_mm->end_data;

	child = task_alloc(me->comm, child_mm);
	if (!child) {
		mm_put(child_mm);
		return -ENOMEM;
	}

	/* The descriptor table is shared by reference: both tasks get a pointer
	 * to the same array and the refcount on the array's owner decides when
	 * the files actually go away. Until CLONE_FILES arrives, a full private
	 * copy is the correct behaviour for fork. */
	for (u32 i = 0; i < me->fd_count; i++) {
		child->fds[i] = me->fds[i];
		file_get(child->fds[i]);
	}

	task_inherit(child, me);
	child->ppid = me->pid;
	child->pgid = me->pgid;
	child->sid = me->sid;

	/* Resume where the parent was suspended. */
	child->user_rip = fork_ctx[this_cpu_id()][0];
	child->user_rsp = fork_ctx[this_cpu_id()][1];
	child->user_rflags = fork_ctx[this_cpu_id()][2];
	child->thread_fn = fork_child_start;
	child->thread_arg = child;

	u64 flags = spinlock_irqsave(&task_all_lock);

	list_add_tail(&child->sibling, &me->children);
	spinlock_unlock_irqrestore(&task_all_lock, flags);

	/* The child does not run until the parent blocks or is preempted, so
	 * the parent's return value is not yet at risk. */
	sched_add(child);
	return (long)child->pid;
}

/* ----------------------------------------------------------------- exec ------ */

/*
 * Build the initial user stack.
 *
 * From the top down: the string area, then the vector (argc, argv pointers, a
 * NULL, envp pointers, a NULL, and the auxiliary vector). The auxiliary vector
 * is not optional decoration — a dynamic loader reads AT_PHDR from it to find
 * the program headers, and libc reads AT_PAGESZ, so a process whose auxv is
 * empty cannot start even with a correct entry point.
 */
static __noreturn void process_user_start(void *arg);

static int count_user_strings(u64 array, char *buf, char **out, u64 max,
			      size_t bufbytes, size_t *used)
{
	u64 i = 0;
	size_t off = 0;

	if (!array) {
		out[0] = NULL;
		return 0;
	}

	for (;;) {
		if (i >= max)
			return -E2BIG;
		u64 ptr = 0;

		if (copy_from_user(&ptr, array + i * 8, 8) < 0)
			return -EFAULT;
		if (ptr == 0)
			break;
		if (off + MAX_ARG_LEN + 1 > bufbytes)
			return -E2BIG;
		long n = copy_string_from_user(buf + off, ptr, MAX_ARG_LEN);

		if (n < 0)
			return (int)n;
		out[i] = buf + off;
		off += (size_t)n + 1;
		i++;
	}
	out[i] = NULL;
	*used = off;
	return (int)i;
}

static int build_user_stack(struct address_space *mm, char *const argv[],
			    char *const envp[], const struct elf_info *info,
			    u64 *out_sp)
{
	u64 argc = 0, envc = 0;
	/* The relocated addresses of the strings, kept in kernel arrays: the
	 * caller's argv is `char *const` and must not be written back to. */
	u64 argv_at[MAX_EXEC_ARGS + 1];
	u64 envp_at[MAX_EXEC_ARGS + 1];
	size_t strbytes = 0;
	u64 abi_version = KERNEL_ABI_VERSION;

	while (argv && argv[argc]) {
		strbytes += strlen(argv[argc]) + 1;
		argc++;
	}
	while (envp && envp[envc]) {
		strbytes += strlen(envp[envc]) + 1;
		envc++;
	}

	/* Eleven auxiliary entries, each a key/value pair. */
	const u32 aux_count = 11;
	u64 vec_words = 1 + (argc + 1) + envc + 1 + 2 * aux_count;
	u64 str_area = ALIGN_UP(strbytes ? strbytes : 1, 16);
	u64 vec_bytes = ALIGN_UP(vec_words * 8, 16);
	u64 sp = ALIGN_DOWN(USER_STACK_TOP - str_area - vec_bytes, 16);

	/* Copy the strings to the top of the stack first, so the pointers the
	 * vector holds are known before the vector is written. */
	u64 str_top = sp + vec_bytes + str_area;
	u64 cursor = str_top;

	for (u64 i = 0; i < argc; i++) {
		size_t n = strlen(argv[i]) + 1;

		cursor -= n;
		if (user_memory_write(mm, (virt_addr_t)cursor, argv[i], n,
				      VM_READ | VM_WRITE) < 0)
			return -EFAULT;
		argv_at[i] = cursor;
	}
	for (u64 i = 0; i < envc; i++) {
		size_t n = strlen(envp[i]) + 1;

		cursor -= n;
		if (user_memory_write(mm, (virt_addr_t)cursor, envp[i], n,
				      VM_READ | VM_WRITE) < 0)
			return -EFAULT;
		envp_at[i] = cursor;
	}

	/* AT_RANDOM points at 16 bytes of kernel-supplied entropy. It has to
	 * live in the process's own address space, so it is written to the
	 * stack rather than to some shared page. */
	u64 random_addr = ALIGN_DOWN(sp - 16, 16);
	u64 random_val = tsc_read() ^ ((u64)(uintptr_t)mm << 12);

	if (user_memory_write(mm, (virt_addr_t)random_addr, &random_val,
			      sizeof(random_val), VM_READ | VM_WRITE) < 0)
		return -EFAULT;

	u64 p = sp;
	int rc = 0;

#define STACK_PUT(word)                                                     \
	do {                                                               \
		if (rc == 0 &&                                            \
		    user_memory_write(mm, (virt_addr_t)p, &(word), 8,      \
				      VM_READ | VM_WRITE) < 0)            \
			rc = -EFAULT;                                    \
		p += 8;                                                   \
	} while (0)

	STACK_PUT(argc);
	for (u64 i = 0; i < argc; i++)
		STACK_PUT(argv_at[i]);
	{
		u64 zero = 0;

		STACK_PUT(zero);
	}
	for (u64 i = 0; i < envc; i++)
		STACK_PUT(envp_at[i]);
	{
		u64 zero = 0;

		STACK_PUT(zero);
	}

	{
		struct {
			u32 key;
			u64 val;
		} aux[aux_count];
		u32 n = 0;

		aux[n].key = AT_PHDR;   aux[n++].val = info->phdr_vaddr;
		aux[n].key = AT_PHNUM;  aux[n++].val = info->phdr_count;
		aux[n].key = AT_PAGESZ; aux[n++].val = PAGE_SIZE;
		aux[n].key = AT_BASE;   aux[n++].val = 0;  /* no interpreter */
		aux[n].key = AT_ENTRY;  aux[n++].val = info->entry;
		aux[n].key = AT_UID;    aux[n++].val = 0;
		aux[n].key = AT_EUID;   aux[n++].val = 0;
		aux[n].key = AT_GID;    aux[n++].val = 0;
		aux[n].key = AT_RANDOM; aux[n++].val = random_addr;
		aux[n].key = AT_ABI_VERSION; aux[n++].val = abi_version;
		aux[n].key = AT_NULL;   aux[n++].val = 0;

		for (u32 i = 0; i < n && rc == 0; i++) {
			u32 key = aux[i].key;
			u64 val = aux[i].val;

			if (user_memory_write(mm, (virt_addr_t)p, &key, 4,
					      VM_READ | VM_WRITE) < 0) {
				rc = -EFAULT;
				break;
			}
			p += 8;
			if (user_memory_write(mm, (virt_addr_t)p, &val, 8,
					      VM_READ | VM_WRITE) < 0) {
				rc = -EFAULT;
				break;
			}
			p += 8;
		}
	}
#undef STACK_PUT

	if (rc < 0)
		return rc;

	*out_sp = sp;
	return 0;
}

/*
 * Reject a thread pointer this kernel is not willing to install.
 *
 * The value checked here is elf.c's, and elf.c is where the arithmetic lives:
 * mm->tls_ptr is ALIGN_UP(p_vaddr + p_memsz, p_align), *not* the unaligned
 * end of the block. That distinction is the one in §3 of STATE.md and it
 * cannot be re-checked from this side, because neither `mm` nor `elf_info`
 * carries p_align -- a p_align of 1 is legal for a block of chars, so the
 * kernel genuinely cannot tell an aligned thread pointer from an unaligned one
 * by looking at the number. What it *can* do is refuse the states that are
 * wrong regardless of alignment, so they fail here where there is a log line
 * rather than in ring 3 where there is none:
 *
 *   - a block with bytes in it and no thread pointer. libc's crt1 asks the
 *     kernel for the pointer and refuses to start when the answer is 0, so
 *     this is caught with a diagnosis instead of by a #PF on the first
 *     `%fs:-16` access;
 *   - a thread pointer with no block behind it, which is a loader that
 *     computed one and forgot to store it;
 *   - a thread pointer at or below the block's own size, so `tp - tls_size`
 *     -- the first byte of the block, which is what the negative-displacement
 *     addressing is measured against -- would underflow into the top of the
 *     address space;
 *   - a thread pointer outside the user half. MSR_FS_BASE is per-CPU and
 *     shared with whatever runs next, so a base that #GPs every access through
 *     it leaves the machine unable to run any process until something else
 *     writes it. syscall.c's fs_base_acceptable() enforces the same rule on
 *     the arch_prctl path; this is the exec path's copy of it.
 *
 * Not checked, and deliberately so: that the pointer is p_align-aligned. See
 * above -- it is not derivable here, and inventing a fixed alignment
 * requirement would be a false invariant rather than a check.
 */
static int exec_thread_pointer_ok(const struct address_space *mm)
{
	if (mm->tls_size == 0)
		/* No PT_TLS. 0 is the honest answer and is what libc treats as
		 * "this image has no __thread storage", not a missing value. */
		return mm->tls_ptr == 0 ? 0 : -EINVAL;

	if (mm->tls_ptr == 0)
		return -EINVAL;
	if (mm->tls_ptr < mm->tls_size)
		return -EINVAL;
	if (mm->tls_ptr >= USER_ADDRESS_MAX)
		return -EINVAL;
	return 0;
}

/*
 * The thread pointer must survive the trip from `mm` to the per-task field
 * unchanged. These are widths, not values, which is the only part of the
 * contract a _Static_assert can express -- but it is the part that fails
 * silently: a narrowed field would still compile, still assign, and would
 * truncate a 64-bit thread pointer to 32 bits, and every access through it
 * would land somewhere in the middle of nowhere rather than faulting at the
 * assignment.
 */
STATIC_ASSERT(sizeof(virt_addr_t) == sizeof(u64),
	      "the thread pointer is a 64-bit address; virt_addr_t must be too");
STATIC_ASSERT(sizeof(((struct address_space *)0)->tls_ptr) ==
	      sizeof(((struct task *)0)->fs_base),
	      "mm->tls_ptr and task::fs_base must be the same width, or the "
	      "thread pointer is truncated on its way to MSR_FS_BASE");

int exec_load_and_run(struct task *t, const void *image, size_t size,
		      char *const argv[], char *const envp[])
{
	struct address_space *new_mm;
	struct elf_info info;
	u64 sp = 0;
	int r;

	if (!t)
		return -EINVAL;

	new_mm = mm_create();
	if (!new_mm)
		return -ENOMEM;

	r = elf_load(new_mm, image, size, 0, &info);
	if (r < 0) {
		/* Say what failed *before* releasing the address space.
		 *
		 * mm_put() tears down the page tables, and a fault inside that
		 * teardown takes the machine down before anything gets here. The
		 * error being returned is the entire diagnosis of why init does not
		 * start, so it is written first, where it survives whatever the
		 * teardown does next.
		 */
		PROC_LOG(KLOG_ERROR, "exec: elf_load failed: %d", r);
		mm_put(new_mm);
		return r;
	}

	/*
	 * The stack is one VMA covering the whole region, with nothing mapped
	 * inside it. The pages the initial frame occupies are faulted in by
	 * user_memory_write below, and the rest of the stack is populated
	 * lazily by the fault handler, which is both cheaper and what makes a
	 * large stack cost nothing until it is used.
	 */
	r = mm_add_vma(new_mm, (virt_addr_t)USER_STACK_BASE,
		       (virt_addr_t)USER_STACK_TOP,
		       VM_READ | VM_WRITE | VM_USER, VM_ANON);
	if (r < 0) {
		PROC_LOG(KLOG_ERROR, "exec: stack VMA failed: %d", r);
		mm_put(new_mm);
		return r;
	}
	new_mm->stack_top = USER_STACK_TOP;

	r = build_user_stack(new_mm, argv, envp, &info, &sp);
	if (r < 0) {
		PROC_LOG(KLOG_ERROR, "exec: user stack failed: %d", r);
		mm_put(new_mm);
		return r;
	}

	/* Checked before the old address space goes, so that a thread pointer
	 * this kernel will not install fails with the process still exactly as
	 * it was -- the same property every other failure above preserves. */
	r = exec_thread_pointer_ok(new_mm);
	if (r < 0) {
		PROC_LOG(KLOG_ERROR,
			 "exec: refusing thread pointer %#lx for a %lu-byte "
			 "TLS block: %d", (unsigned long)new_mm->tls_ptr,
			 (unsigned long)new_mm->tls_size, r);
		mm_put(new_mm);
		return r;
	}

	/* The old address space is only dropped once the new one is complete,
	 * so a failure above leaves the process exactly as it was. */
	if (t->mm) {
		mm_put(t->mm);
		t->mm = NULL;
	}
	t->mm = new_mm;
	mm_get(t->mm);

	t->user_rip = info.entry;
	t->user_rsp = sp;
	t->user_rflags = USER_DEFAULT_RFLAGS;

	/*
	 * The task's thread pointer comes from the image that was just loaded,
	 * not from whatever the process had before: exec replaces the address
	 * space, and the new image's PT_TLS block is somewhere else. Leaving
	 * `fs_base` alone here would carry the *old* process's thread pointer
	 * into the new address space, which for an exec of a fresh image is a
	 * pointer into pages that no longer exist -- and for the very first
	 * exec, when there was no old process, it would leave it at the zero
	 * task_alloc() gave it.
	 *
	 * The value is elf.c's ALIGN_UP(p_vaddr + p_memsz, p_align), already
	 * checked by exec_thread_pointer_ok() above. It is *not* loaded into
	 * MSR_FS_BASE here: exec runs on the old CR3 with the new mm merely
	 * built, and the register is written by the ring-3 return path
	 * (`call task_load_fs_current` in ret_to_user) or by the process's own
	 * arch_prctl once crt1 runs. Writing it here would put a user-space
	 * address in a per-CPU register belonging to a process that is not
	 * about to run.
	 */
	t->fs_base = new_mm->tls_ptr;

	PROC_LOG(KLOG_INFO,
		 "exec: \"%s\" entry %#lx sp %#lx, thread pointer %#lx for a "
		 "%lu-byte TLS block",
		 t->comm, (unsigned long)t->user_rip,
		 (unsigned long)t->user_rsp, (unsigned long)t->fs_base,
		 (unsigned long)new_mm->tls_size);

	t->thread_fn = process_user_start;
	t->thread_arg = t;
	return 0;
}

static __noreturn void process_user_start(void *arg)
{
	struct task *t = arg;

	process_enter_user(t, t->user_rip, t->user_rsp);
}

__noreturn void process_enter_user(struct task *t, u64 entry, u64 sp)
{
	UNUSED(t);
	/*
	 * iretq rather than sysretq: this is the first entry into ring 3 for
	 * the process, and sysretq would require RFLAGS to have been built
	 * with the reserved bit already in the right place and the frame
	 * arranged for a return rather than an interrupt. The interrupt agent
	 * owns that stub.
	 */
	ret_to_user(entry, sp, USER_DEFAULT_RFLAGS, 0, 0);
	__builtin_unreachable();
}

/* ---------------------------------------------------------------- init ------ */

/*
 * The linked-in blob is an *initrd container*, not a bare ELF.
 *
 * Passing it straight to the ELF loader hands elf_validate() the container's
 * four magic bytes where it expects 0x7f "ELF", so the load fails with
 * -ENOEXEC. A bare ELF is returned unchanged, so a caller holding one does not
 * have to know which form it has.
 *
 * This was inline in process_create_init() only. sys_execve() passed the raw
 * container to exec_load_and_run() and could therefore never succeed -- not on
 * a corrupt image, not on an unknown path, on any input at all. The failure was
 * silent until the execve error logging landed, which is the better order: the
 * bug was invisible while the code around it was also silent.
 */
static const unsigned char *initrd_find_init_elf(const void *blob,
						unsigned long blob_size,
						unsigned long *out_size)
{
	const unsigned char *img = blob;
	unsigned long elf_size = blob_size;

	*out_size = 0;

	if (blob_size >= INITRD_HEADER_SIZE &&
	    initrd_le64((const uint8_t *)img) == INITRD_MAGIC) {
		unsigned long total = initrd_le64((const uint8_t *)img + 16);
		unsigned int entries;
		unsigned long pos = INITRD_HEADER_SIZE;
		unsigned long payload;
		unsigned long off = 0, size = 0;
		int found = 0;

		if (total > blob_size || total < INITRD_HEADER_SIZE) {
			PROC_LOG(KLOG_FATAL,
				 "initrd total_len %lu exceeds blob %lu",
				 total, blob_size);
			return NULL;
		}

		/*
		 * The header records an entry count, and it has to: every entry's
		 * offset is relative to the payload, while the payload follows the
		 * index, so without a count the payload base is uncomputable. Walking
		 * until a zero name_len -- the first guess at this -- reads the first
		 * bytes of an ELF header as a 20-byte index entry and treats them as
		 * one, which puts the payload base inside the file and yields a
		 * garbage ELF that happens to pass the magic check some of the time.
		 *
		 * Entry names are not padded or aligned; the bytes follow the fixed
		 * part immediately, so `pos` advances by exactly name_len.
		 */
		entries = initrd_le32((const uint8_t *)img + 12);
		for (unsigned int e = 0; e < entries && pos + INITRD_ENTRY_SIZE <= total; e++) {
			unsigned int name_len = initrd_le16((const uint8_t *)img + pos);
			unsigned long esz = initrd_le64((const uint8_t *)img + pos + 4);
			unsigned long eoff = initrd_le64((const uint8_t *)img + pos + 12);

			pos += INITRD_ENTRY_SIZE;
			if (name_len == 0 || pos + name_len > total)
				break;

			if (!found && name_len == 4 &&
			    !memcmp(img + pos, "init", 4)) {
				off = eoff;
				size = esz;
				found = 1;
			}
			pos += name_len;
		}
		payload = pos;

		if (!found) {
			PROC_LOG(KLOG_FATAL,
				 "initrd has no entry named \"init\" (%lu bytes)",
				 blob_size);
			return NULL;
		}
		if (off + size > total) {
			PROC_LOG(KLOG_FATAL,
				 "initrd \"init\" payload out of range: off %lu size %lu",
				 off, size);
			return NULL;
		}

		img = img + payload + off;
		elf_size = size;
	}

	*out_size = elf_size;
	return img;
}

struct task *process_create_init(void)
{
	static char arg0_init[] = "init";
	static char arg0_sh[] = "/bin/sh";
	char *argv[] = { arg0_init, NULL };
	char *envp[] = { NULL };
	struct task *t;

	UNUSED(arg0_sh);

	t = task_alloc("init", NULL);
	if (!t) {
		PROC_LOG(KLOG_FATAL, "cannot allocate the init task");
		return NULL;
	}

	if (setup_console_fds(t) < 0) {
		task_put(t);
		return NULL;
	}

	/*
	 * The image is linked into the kernel by the build. If it is missing
	 * the link fails rather than producing a kernel that boots to a panic.
	 */
	if (init_image_size == 0) {
		PROC_LOG(KLOG_FATAL, "no init image linked into the kernel");
		task_put(t);
		return NULL;
	}

	unsigned long elf_size = 0;
	const unsigned char *elf = initrd_find_init_elf(init_image,
							init_image_size,
							&elf_size);

	if (!elf) {
		task_put(t);
		return NULL;
	}

	if (exec_load_and_run(t, elf, elf_size, argv, envp) < 0) {
		PROC_LOG(KLOG_FATAL, "cannot load the init image");
		task_put(t);
		return NULL;
	}

	/* PID 1 by definition; task_alloc() handed out something else. */
	t->pid = 1;
	t->tid = 1;
	t->pgid = 1;
	t->sid = 1;
	t->ppid = 0;
	t->state = TASK_NEW;

	sched_add(t);
	PROC_LOG(KLOG_INFO, "init task created, entry %#lx", t->user_rip);
	return t;
}

/*
 * execve(2), minus the "which task" question.
 *
 * The only reason this is not the syscall body itself is that taking the task
 * as a parameter makes the failure paths inspectable without a live userspace
 * process: each of them returns an errno and nothing else, so a path taken only
 * by a failing execve produces no evidence at all until something is already
 * broken. Every one below writes a line before it returns, which is what makes
 * "init got -ENOEXEC" distinguishable from "init never called execve".
 *
 * Safety of the logging itself, since it runs on the failure path and a
 * diagnostic that takes the machine down destroys the diagnosis:
 *
 *   - klog_emit() allocates nothing. It formats into two fixed stack buffers
 *     (512 and 640 bytes) and truncates rather than overflowing, so it is safe
 *     where an allocator failure is the reason for the error being reported.
 *   - console_write() takes its own lock internally, and no lock is held at
 *     any point below: the spinlock scopes in copy_string_from_user() and
 *     exec_load_and_run() have all been released by the time any of these
 *     lines runs.
 *   - nothing here calls back into the process, exec or syscall layers, so
 *     there is no path from a log line back to execve.
 *   - the only string printed is `kpath`, and only after copy_string_from_user()
 *     has returned non-negative, which is the only outcome that guarantees it
 *     is NUL-terminated. copy_string_from_user() leaves the buffer untouched
 *     when it fails with -EFAULT, so printing it there would read uninitialised
 *     stack -- not a fault, but garbage in the one line that has to be right.
 */
static long execve_common(struct task *t, u64 path, u64 argv, u64 envp)
{
	/* Two staging buffers: argv strings and envp strings, so the two
	 * copies of a long argument cannot run into each other. */
	static char argv_buf[MAX_EXEC_ARGS][MAX_ARG_LEN];
	static char envp_buf[MAX_EXEC_ARGS][MAX_ARG_LEN];
	char kpath[MAX_ARG_LEN];
	char *kargv[MAX_EXEC_ARGS + 1];
	char *kenvp[MAX_EXEC_ARGS + 1];
	size_t used = 0;
	long n;

	if (!t || !t->mm) {
		PROC_LOG(KLOG_ERROR,
			 "execve: called by a task with no address space");
		return -ESRCH;
	}

	n = copy_string_from_user(kpath, path, sizeof(kpath));
	if (n < 0) {
		/* No path in the message: see the note above. */
		PROC_LOG(KLOG_ERROR,
			 "execve: pid %u path is not a readable user string: %ld",
			 t->pid, n);
		return (int)n;
	}

	/*
	 * There is no filesystem, so there is exactly one executable: the
	 * image linked into the kernel. Recognising it by path is arbitrary
	 * but it has to be *something*, and accepting the conventional init
	 * spellings means a libc's exec of its own init works.
	 */
	if (strcmp(kpath, "/init") != 0 && strcmp(kpath, "/bin/init") != 0 &&
	    strcmp(kpath, "/sbin/init") != 0) {
		PROC_LOG(KLOG_ERROR,
			 "execve: pid %u asked for \"%s\", which is not an image "
			 "this kernel has",
			 t->pid, kpath);
		return -ENOENT;
	}

	memset(kargv, 0, sizeof(kargv));
	memset(kenvp, 0, sizeof(kenvp));

	/* argv[0] is the path itself, as execve(2) specifies. */
	strlcpy(argv_buf[0], kpath, MAX_ARG_LEN);
	kargv[0] = argv_buf[0];
	n = count_user_strings(argv, &argv_buf[1][0], kargv + 1,
			       MAX_EXEC_ARGS - 1,
			       (MAX_EXEC_ARGS - 1) * MAX_ARG_LEN, &used);
	if (n < 0) {
		PROC_LOG(KLOG_ERROR,
			 "execve: pid %u \"%s\" argv is unreadable or too long: "
			 "%ld", t->pid, kpath, n);
		return n;
	}

	n = count_user_strings(envp, &envp_buf[0][0], kenvp, MAX_EXEC_ARGS,
			       MAX_EXEC_ARGS * MAX_ARG_LEN, &used);
	if (n < 0) {
		PROC_LOG(KLOG_ERROR,
			 "execve: pid %u \"%s\" envp is unreadable or too long: "
			 "%ld", t->pid, kpath, n);
		return n;
	}

	/* Unwrap the container here too. This used to pass init_image straight
	 * through, so elf_validate() saw the container's magic instead of
	 * 0x7f "ELF" and every execve returned -ENOEXEC -- on every input, not
	 * just a bad one. Nothing below this point can succeed without it. */
	unsigned long elf_size = 0;
	const unsigned char *elf = initrd_find_init_elf(init_image,
							init_image_size,
							&elf_size);

	if (!elf)
		return -ENOEXEC;

	n = exec_load_and_run(t, elf, elf_size, kargv, kenvp);
	if (n < 0) {
		/* exec_load_and_run() logs the stage that failed; this line adds
		 * the two things it has no way to know -- who asked, and for
		 * what -- so the pair reads as a cause rather than a symptom. */
		PROC_LOG(KLOG_ERROR,
			 "execve: pid %u \"%s\" failed to load: %ld",
			 t->pid, kpath, n);
	}
	return n;
}

long sys_execve(u64 path, u64 argv, u64 envp)
{
	return execve_common(current_task(), path, argv, envp);
}
