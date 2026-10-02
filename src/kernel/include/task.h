/*
 * task.h — execution contexts.
 *
 * A task is one schedulable thread of execution. The distinction from a
 * "process" is purely bookkeeping: a process is a task plus the address space
 * and file table that its thread group shares. Keeping them as one structure
 * with a refcount on `mm` is what lets CLONE_VM threads share an address space
 * without a second object to keep in sync.
 *
 * Context layout
 * --------------
 * The saved-register block is built by sched/task code and consumed by
 * context.S. The exact contract, which both sides must agree on:
 *
 *     high address
 *     +-------------------------+
 * r15| r15                     |
 *     +-------------------------+
 * r14| r14                     |
 *     +-------------------------+
 * r13| r13                     |
 *     +-------------------------+
 * r12| r12                     |
 *     +-------------------------+
 * rbx| rbx                     |
 *     +-------------------------+
 * rbp| rbp                     |
 *     +-------------------------+
 * rip| return address          |  <- consumed by `ret` in context_switch
 *     +-------------------------+
 *     | (callee frames grow down)|
 *     v
 *     low address
 *
 * The frame is therefore 56 bytes with the return address at offset 48, and
 * the address handed to context_switch() points at the r15 slot. The frame
 * base must be 16-byte aligned, so that the stack is 8 mod 16 after the `ret`,
 * which is what the SysV ABI requires at function entry.
 */
#ifndef TASK_H
#define TASK_H

/*
 * The include order below is deliberate. stdbool and io.h come first because
 * several kernel headers in this tree rely on their includers having provided
 * them, and the load-bearing one is the vmm.h repair at the bottom.
 */
#include <stdbool.h>
#include <io.h>
#include <types.h>
#include <boot.h>
#include <list.h>
#include <percpu.h>
#include <spinlock.h>

/*
 * vmm.h's struct vma carries a file offset. Defining the two POSIX scalar types
 * here puts them in the kernel's one canonical place rather than in each .c
 * file.
 */
typedef s64 ssize_t;
typedef s64 off_t;

#include <vmm.h>

/* ------------------------------------------------------- refcounting -------- */

/*
 * A reference count. The counter is the last word of the object it protects in
 * practice here, so the release path decrements and frees in one step.
 */
typedef struct {
	volatile s32 count;
} refcount_t;

#define REFCOUNT_INIT(n) { (n) }

static inline void refcount_set(refcount_t *r, s32 n)
{
	__atomic_store_n(&r->count, n, __ATOMIC_RELAXED);
}

static inline void refcount_inc(refcount_t *r)
{
	__atomic_add_fetch(&r->count, 1, __ATOMIC_RELAXED);
}

/* Decrement and report whether the object is now unreferenced. */
static inline bool refcount_dec_and_test(refcount_t *r)
{
	return __atomic_sub_fetch(&r->count, 1, __ATOMIC_ACQ_REL) == 0;
}

static inline s32 refcount_read(const refcount_t *r)
{
	return __atomic_load_n(&r->count, __ATOMIC_ACQUIRE);
}

/* ------------------------------------------------------------ states -------- */

typedef enum {
	TASK_NEW = 0,      /* built, not yet on any run queue */
	TASK_RUNNABLE,     /* on a run queue, waiting for the CPU */
	TASK_RUNNING,      /* the task on this CPU right now */
	TASK_BLOCKED,      /* sleeping on a timer, I/O, or a child */
	TASK_ZOMBIE,       /* exited, exit status not yet reaped */
	TASK_DEAD,         /* reaped or detached; the object is on its way out */
} task_state_t;

/* -------------------------------------------------------- credentials ------ */

/* Capability bits. Only the ones this subsystem checks are defined; the mask is
 * u64 so the security subsystem can extend it without an ABI change. */
#define CAP_SYS_NICE    (1ULL << 23)   /* sched_setscheduler, affinity */
#define CAP_SYS_ADMIN   (1ULL << 21)
#define CAP_DAC_OVERRIDE (1ULL << 1)

struct cred {
	u32 uid;
	u32 gid;
	u64 caps;
};

/* ------------------------------------------------------------- cpumask ------ */

/*
 * CPU affinity mask: one bit per CPU, four words to cover MAX_CPUS. A bitmap
 * rather than a single word because the bound is 256, and a bitmap rather than
 * an array of bools because a wakeup has to test membership on the scheduling
 * path where a load and a bit test are cheaper than a branch.
 */
typedef struct {
	u64 bits[(MAX_CPUS + 63) / 64];
} cpumask_t;

#define CPU_MASK_BITS  ((u32)(sizeof(cpumask_t) * 8))

static inline void cpumask_clear(cpumask_t *m)
{
	for (u32 i = 0; i < ARRAY_SIZE(m->bits); i++)
		m->bits[i] = 0;
}

static inline void cpumask_setall(cpumask_t *m)
{
	for (u32 i = 0; i < ARRAY_SIZE(m->bits); i++)
		m->bits[i] = ~0ULL;
}

static inline void cpumask_set(cpumask_t *m, u32 cpu)
{
	if (cpu < CPU_MASK_BITS)
		m->bits[cpu / 64] |= 1ULL << (cpu % 64);
}

static inline void cpumask_clear_one(cpumask_t *m, u32 cpu)
{
	if (cpu < CPU_MASK_BITS)
		m->bits[cpu / 64] &= ~(1ULL << (cpu % 64));
}

static inline bool cpumask_test(const cpumask_t *m, u32 cpu)
{
	if (cpu >= CPU_MASK_BITS)
		return false;
	return (m->bits[cpu / 64] & (1ULL << (cpu % 64))) != 0;
}

static inline bool cpumask_empty(const cpumask_t *m)
{
	for (u32 i = 0; i < ARRAY_SIZE(m->bits); i++)
		if (m->bits[i])
			return false;
	return true;
}

static inline void cpumask_copy(cpumask_t *dst, const cpumask_t *src)
{
	for (u32 i = 0; i < ARRAY_SIZE(src->bits); i++)
		dst->bits[i] = src->bits[i];
}

/* The saved-register block described at the top of this file. Declared here
 * because sched.c and task.c build it and context.S consumes it; the offsets
 * are the ABI between the two, so a change here is a change to context.S. */
struct context {
	u64 r15;
	u64 r14;
	u64 r13;
	u64 r12;
	u64 rbx;
	u64 rbp;
	u64 rip;      /* consumed by the `ret` at the end of context_switch */
};

STATIC_ASSERT(offsetof(struct context, rip) == 48,
	      "context frame layout must match context.S");

/* ------------------------------------------------------------ the task ------ */
/* Kernel stack size. Large enough for the deepest path in the kernel (a syscall
 * frame plus the VMM's fault path plus a bounce buffer), and every one of those
 * frames is bounded, so a bigger stack only delays the guard-page detection of
 * an overflow rather than preventing it. */
#define TASK_KERNEL_STACK_SIZE  (32u * 1024u)
#define TASK_COMM_LEN          16
#define TASK_MAX_FDS           256

/* Size of the x87/SSE save area. FXSAVE writes 512 bytes; the rest of the
 * allocation leaves room for a future XSAVE-format area without changing the
 * allocator call. */
#define TASK_FPU_STATE_SIZE    4160u

struct task {
	/* --- identity ------------------------------------------------------- */
	u32 pid;
	u32 tid;
	u32 ppid;
	u32 pgid;
	u32 sid;
	char comm[TASK_COMM_LEN];

	/* --- lifecycle ------------------------------------------------------ */
	task_state_t state;
	int exit_code;
	bool detached;        /* removed from every run queue for good */
	bool reaped;          /* parent has collected the exit status */
	struct address_space *mm;
	refcount_t refs;

	/* --- run queue membership ------------------------------------------- */
	struct list_head rq_node;
	u32 rq_cpu;           /* CPU whose run queue holds this task */
	bool on_rq;           /* sitting on a per-CPU run queue */
	bool in_global;       /* sitting on the global migration list */

	/* --- scheduling ------------------------------------------------------ */
	u32 policy;
	s32 nice;
	u32 rt_priority;
	u64 rt_slice;         /* ticks of RT slice left before a RR rotation */
	u8 mlfq_level;        /* 0 = highest */
	u8 io_boost;          /* one I/O boost already taken, prevents cascades */
	u8 rt_throttled;      /* RT bandwidth exhausted for this period */
	u64 vruntime;         /* nanoseconds of CPU, monotonic per level */
	u64 exec_budget;      /* ticks left in the current MLFQ slice */
	u64 deadline_ns;      /* SCHED_DEADLINE: relative deadline */
	u64 period_ns;        /* SCHED_DEADLINE: period */
	u64 rt_runtime;       /* SCHED_DEADLINE: CPU budget per period */
	u64 rt_consumed;      /* consumed out of rt_runtime this period */
	u64 rt_period_start;  /* tick the current period began at */
	u64 abs_deadline;     /* absolute tick deadline, EDF ordering key */
	u64 affinity_mask;    /* 1 << cpu when pinned, 0 when unpinned */
	cpumask_t cpumask;
	u64 last_run;         /* tick it last started running */
	u64 total_ticks;      /* lifetime CPU ticks */
	u64 wait_ticks;       /* ticks spent runnable-but-not-running (aging) */
	bool cpu_accounted;   /* charged to a per-CPU accounting bucket */

	/* --- execution context ---------------------------------------------- */
	void *kernel_stack;
	u64 context_rsp;      /* points at the r15 slot of struct context */
	u64 *fpu_state;       /* 64-byte aligned FXSAVE area */
	bool fpu_dirty;       /* x87/SSE state must be saved on switch-out */

	/*
	 * The task's FS base, and the only reason MSR_FS_BASE is not simply a
	 * per-CPU constant.
	 *
	 * MSR_FS_BASE is a per-CPU register, and a thread pointer is per-task
	 * state: arch_prctl writes the register for whoever is running now, and
	 * every %fs-relative access the task makes is relative to it. So the
	 * value has to travel with the task across a switch, exactly as CR3 and
	 * the FPU image do — see sched_switch_frame(), which is where that save
	 * and restore belongs and where the FS pair has to sit alongside the
	 * other two.
	 *
	 * Zero for a kernel thread and for a user task that has not installed a
	 * thread pointer, which is the state every process is in before its
	 * startup code runs. Nothing in the kernel addresses memory through
	 * %fs — per-CPU state is %gs, read through the hidden GS base — so a
	 * task's FS base pointing into user space is inert while the task is
	 * running in the kernel.
	 */
	u64 fs_base;

	/* --- kernel thread entry -------------------------------------------- */
	void (*thread_fn)(void *);
	void *thread_arg;

	/* --- files ------------------------------------------------------------ */
	struct file **fds;
	u32 fd_count;
	struct cred cred;

	/* --- process relationships ------------------------------------------- */
	struct list_head sibling;   /* on the parent's children list */
	struct list_head children;  /* head of this task's children */
	struct task *parent;
	void *mm_private;

	/* Where a user task starts. The kernel trampoline reads these and
	 * iretq's into ring 3; a kernel task ignores them. A task created by
	 * fork inherits the parent's saved user registers here, so it resumes
	 * at the same instruction with RAX = 0. */
	u64 user_rip;
	u64 user_rsp;
	u64 user_rflags;

	/* --- sleeping and waiting --------------------------------------------- */
	struct list_head sleep_node;  /* on a per-CPU sleep list */
	bool sleeping;
	u64 wake_tick;                /* tick the sleeper becomes runnable */
	struct task *waiter;          /* task blocked on this one changing state */
	struct list_head all_node;    /* on the global task list */
};

STATIC_ASSERT(sizeof(cpumask_t) == 32, "cpumask must cover MAX_CPUS in 4 words");

/* --------------------------------------------------------------- API -------- */

/* Allocate a zeroed task with a kernel stack, an FPU save area, and a fd
 * table. No address space: kernel threads pass mm == NULL. */
struct task *task_alloc(const char *name, struct address_space *mm);

/* A task whose first entry is a C function running on its own kernel stack. */
struct task *task_create_kernel(void (*fn)(void *), void *arg, const char *name);

/* Set the process-level identity fields from a parent (fork/exec path). */
void task_inherit(struct task *child, struct task *parent);

/* Give a task the full complement of MLFQ quantum for its level. */
void task_reset_budget(struct task *t);

/*
 * Move a task's thread pointer between the task and MSR_FS_BASE.
 *
 * Both halves belong on the per-task side of a context switch, beside the CR3
 * and FPU handling in sched_switch_frame(); the load is also what a transition
 * into ring 3 needs, which is why it is a function rather than a bare WRMSR
 * living in whichever entry stub happened to need it. `t == NULL` is a no-op,
 * so the entry path can call it unconditionally before the scheduler exists.
 */
void task_load_fs_base(struct task *t);
void task_save_fs_base(struct task *t);

/*
 * task_load_fs_current() — the same load, for the task on this CPU.
 *
 * This exists because ret_to_user() cannot supply a task pointer from
 * assembly: it has no way to hold current_task()'s return value across a
 * second call without spilling, and it has already consumed RDI/RSI/RDX as its
 * own arguments. The whole change the ring-3 return path needs is therefore
 * one instruction -- `call task_load_fs_current` before the iretq in
 * ret_to_user -- and this is the function it names.
 *
 * It is required for every forked child, not only for a process's first entry:
 * fork resumes the child at the parent's syscall return site and never re-runs
 * the C startup that would otherwise install the thread pointer.
 */
void task_load_fs_current(void);

/* The task running on this CPU, or NULL before the scheduler starts. */
struct task *current_task(void);

/* Lookups over the global task list. Both take the list lock internally. */
struct task *task_find_by_pid(u32 pid);
struct task *task_find_by_tid(u32 tid);

/* Iterate the global task list. The callback must not sleep. */
void task_for_each(int (*fn)(struct task *, void *), void *arg);

/*
 * The global task list and its lock. Exposed because parent/child links are
 * spliced on the same list: a child's membership in the global list and in its
 * parent's children list have to be ordered against each other, and two locks
 * would mean two orders to get right.
 */
extern struct list_head task_all_list;
extern spinlock_t task_all_lock;

/* Change state and record the wait time used by the aging pass. */
void task_set_state(struct task *t, task_state_t state);

/* Drop a reference; the last one frees the task's resources. */
void task_put(struct task *t);
void task_get(struct task *t);

/* Terminate the calling task. Never returns. */
__noreturn void task_exit_current(int code);

/* Release everything a dead task owns (fds, mm, kernel stack) and drop it from
 * every list. Safe to call on a task that never ran. */
void task_release_resources(struct task *t);

/* The per-CPU bootstrap kernel stack, used before any task exists. */
void *task_bootstrap_stack_top(void);

/* ------------------------------------------------------- context.S ----------- */

/*
 * void context_switch(u64 *save_rsp, u64 new_rsp)
 *
 * Saves rbp/rbx/r12-r15 below the current RSP and stores the resulting RSP at
 * *save_rsp; loads new_rsp and restores from it, ending in a `ret` that takes
 * its target from the word at new_rsp + 48.
 *
 * void context_restore(u64 rsp)
 *
 * The restore half on its own, for entering a task that has never been
 * descheduled. Does not save anything, so the caller must not return.
 */
void context_switch(u64 *save_rsp, u64 new_rsp);
__noreturn void context_restore(u64 rsp);

/*
 * FXSAVE / FXRSTOR. The kernel is built with -mno-sse -mno-80387, so these
 * cannot be written in C: the compiler would never emit the instructions and
 * would not preserve the state area. Note that neither instruction checks
 * CR0.TS, so no #NM can escape from here.
 */
void fpu_save(void *state);
void fpu_restore(const void *state);

/* RDTSC, lightly ordered. Callers that need a serialising read use
 * cpu_serialize() from io.h around it. */
u64 tsc_read(void);

/* The instruction pointer every task is born with, used as the saved `ret`
 * target so the first switch lands in C with a well-formed frame. */
__noreturn void task_trampoline(void);

#endif /* TASK_H */
