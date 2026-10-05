/*
 * process.h — processes, file descriptors, and the user memory interface.
 *
 * A process is a group of tasks that share an address space and a file
 * descriptor table. Both are refcounted and hung off struct task, so a
 * CLONE_VM thread is just a second task pointing at the same mm with an extra
 * reference — there is no separate "process" object whose fields can disagree
 * with the tasks' idea of the world.
 */
#ifndef PROCESS_H
#define PROCESS_H

#include <types.h>
#include <stdbool.h>
#include <io.h>
#include <list.h>
#include <task.h>
#include <vmm.h>
#include <uapi/syscall.h>
#include <uapi/errno.h>

/* --------------------------------------------------------------- files ------ */

struct task;

struct file_ops {
	ssize_t (*read)(struct file *f, void *buf, size_t count);
	ssize_t (*write)(struct file *f, const void *buf, size_t count);
	long (*ioctl)(struct file *f, unsigned long request, void *arg);
	int (*release)(struct file *f);
};

struct file {
	u64 f_pos;
	u32 f_flags;
	refcount_t refs;
	const struct file_ops *ops;
	void *private;        /* the backing object: console, pipe, inode */
	struct list_head all_node;   /* on the global open-file list */
	u32 ino;
};

struct file *file_alloc(const struct file_ops *ops, void *private, u32 flags);
void file_get(struct file *f);
void file_put(struct file *f);

/* The console-backed file, used for fds 0/1/2 of every process. */
struct file *console_file_create(void);

/* Descriptor table. */
int fd_install(struct task *t, struct file *f);
struct file *fd_lookup(struct task *t, int fd);
int fd_close(struct task *t, int fd);
void fd_table_close_all(struct task *t);

/* --------------------------------------------------------------- fds -------- */

/* ------------------------------------------------------------- process ------ */

/*
 * Build the first user process: a fresh address space, a kernel stack, a
 * console on fds 0/1/2, and the embedded init image loaded by elf.c.
 *
 * The image is linked into the kernel by the build; process.c only refers to
 * the two symbols below and never touches their contents directly.
 */
extern unsigned char init_image[];
extern unsigned long init_image_size;

/*
 * Initrd container, as produced by tools/initrd.py.
 *
 *   header  24 bytes: magic u64, version u32, entry_count u32, total_len u64
 *
 * `entry_count` is load-bearing, not decoration. Every entry's offset is
 * relative to the payload and the payload follows the index, so the payload
 * base cannot be found without knowing where the index stops. Deriving it by
 * walking until a zero name length parses the first bytes of an ELF header as
 * an index entry.
 *   index   repeated: name_len u16, mode u16, size u64, offset u64,
 *                     then name_len bytes of name -- no padding, no alignment
 *   payload the files, concatenated; every entry's `offset` is relative to
 *           the *start of this section*, which the header does not record.
 *           The only way to find it is to walk the index to its end, so a
 *           consumer cannot resolve an offset without doing so first.
 *
 * Little-endian throughout, matching every x86 ABI we run on.
 */
#define INITRD_MAGIC       0x4F53425255444E44ULL  /* "OSRBUNDND" */
#define INITRD_HEADER_SIZE 24u
#define INITRD_ENTRY_SIZE  20u

/* Unaligned little-endian loads. The container is byte-packed, so casting a
 * pointer to a struct would silently depend on the kernel's alignment. */
static inline uint16_t initrd_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t initrd_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t initrd_le64(const uint8_t *p)
{
	uint64_t v = 0;
	int i;

	for (i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

struct task *process_create_init(void);

/* Terminate the calling task. Never returns. */
__noreturn void sys_exit(int code);

/* Terminate every task that shares this task's address space. */
__noreturn void sys_exit_group(int code);

/* wait4(2). Returns the reaped pid, or a negative errno. */
long sys_wait4(s64 pid, u64 wstatus, u64 options, u64 rusage);

/* wait4 options. */
#define WNOHANG 1

/*
 * Record where the calling task was suspended so a forked child can resume at
 * the same user instruction. The syscall layer calls this with the values from
 * the saved register frame before invoking sys_fork(), because a fresh child
 * has no saved frame of its own and must borrow the parent's.
 */
void process_set_fork_context(u64 rip, u64 rsp, u64 rflags);

/* fork(2): a full page-table copy (see the trade-off note in process.c). */
long sys_fork(void);

/* execve(2) over the embedded init image. */
long sys_execve(u64 path, u64 argv, u64 envp);

/* ------------------------------------------------------------- user mem ----- */

/* True when [addr, addr+len) lies entirely in the lower half and does not
 * overflow. This is the gate every user pointer passes before it is
 * dereferenced; nothing in this kernel reads a user address without it. */
bool user_range_ok(struct address_space *mm, u64 addr, size_t len);

long copy_from_user(void *dst, u64 src, size_t n);
long copy_to_user(u64 dst, const void *src, size_t n);

/* Copy a NUL-terminated user string into a kernel buffer, bounded. */
long copy_string_from_user(char *dst, u64 src, size_t max);

/*
 * Make `len` bytes at `addr` present in `mm` and copy `src` there, allocating
 * and faulting pages as needed. This is the only way user memory is written by
 * the kernel: there is no window in which a partially valid user address is
 * dereferenced, because every page is translated before it is touched.
 */
long user_memory_write(struct address_space *mm, virt_addr_t addr,
		       const void *src, size_t len, uint32_t prot);

/* Map one anonymous zeroed page into a user address space. */
long user_map_zero_page(struct address_space *mm, virt_addr_t addr, uint32_t prot);

/* The address of the first byte past a user range, saturating at 0. */
u64 user_range_end(u64 addr, size_t len);

/* ---------------------------------------------------------------- ELF ------- */

#define ELF_MAX_PHDRS 64

struct elf_info {
	u64 entry;          /* e_entry + load_bias */
	u64 phdr_vaddr;     /* where the program headers live in the new mm */
	u32 phdr_count;
	u64 min_vaddr;      /* lowest and highest byte any PT_LOAD claims */
	u64 max_vaddr;
	u64 tls_ptr;        /* initial thread pointer: PT_TLS end, 0 if none */
	u64 tls_size;       /* PT_TLS p_memsz, 0 if the image has no PT_TLS */
	bool is_dyn;        /* ET_DYN: `entry` is relative to the load bias */
};

/*
 * Validate and map an ELF64 image into `mm`. Returns 0 or a negative errno.
 * Nothing is mapped unless the whole header and every program header entry
 * has already passed validation, so a malformed image cannot leave a
 * half-constructed address space behind.
 *
 * A PT_TLS segment is mapped like any other, and additionally recorded in both
 * `mm->tls_ptr` and `out->tls_ptr`: the process's initial thread pointer is the
 * *end* of the block, and it is the one number a libc needs before it can
 * execute an instruction compiled against `__thread`. See SYS_get_tls_base in
 * uapi/syscall.h for why that is asked for rather than derived, and elf.c for
 * why the end and not the start.
 */
int elf_load(struct address_space *mm, const void *image, size_t size,
	     uint64_t load_bias, struct elf_info *out);

/*
 * Load `image` into a brand-new address space and build the initial user stack
 * (argc/argv/envp/auxv and the strings they point at) for `t`. `name` is the
 * resolved program name, and is only used to make the load log say which image
 * was loaded rather than which task asked.
 *
 * On success `t->mm` is the new address space and the task's user context is
 * primed. It does NOT enter user mode, and it does NOT write CR3: entering is
 * the caller's business, and the two callers differ.
 *
 *   - process_create_init() hands the task to the scheduler, which publishes
 *     the PML4 in context_switch() and reaches ring 3 through task_trampoline.
 *   - execve() calls exec_enter_image(), which publishes the PML4 and iretq's
 *     into the new image. execve does not return to its caller on success.
 *
 * A caller that has replaced a running process's address space and then
 * returns to that process is not calling this correctly: CR3 still names the
 * old one and every user pointer the kernel resolves afterwards goes through
 * t->mm, which is now a different address space.
 */
int exec_load_and_run(struct task *t, const void *image, size_t size,
		      const char *name, char *const argv[], char *const envp[]);

/* Set up the user-context registers for the first entry into ring 3. */
__noreturn void process_enter_user(struct task *t, u64 entry, u64 sp);

/* ------------------------------------------------- external dependencies ----- */

/* Console agent. The declarations live in <tty.h>, which now exists; they were
 * repeated here while it did not, and the copy is exactly how tty_init() ended
 * up invisible to kmain and never called. */
#include <tty.h>

/* Interrupt/GDT agent. These now live in <gdt.h> and <interrupt.h>; the
 * declarations are repeated here only so process.c does not need the interrupt
 * header for three prototypes. */
extern void gdt_reload(uint32_t cpu);
extern void tss_set_kernel_stack(void *stack_top);
extern void ret_to_user(uint64_t rip, uint64_t rsp, uint64_t rflags,
			uint64_t rcx, uint64_t r11);

#endif /* PROCESS_H */
