/*
 * task.c — task objects: allocation, identity, refcounting, teardown.
 *
 * Everything here is about the lifetime of a struct task. Scheduling decisions
 * live in sched.c; the only scheduling calls made from this file are the ones
 * that are inseparable from a state change (a task has to leave the run queue
 * before its object can be freed).
 *
 * The global task list exists for identity lookups — pid_to_task() is on the
 * fork, wait4 and exit paths — and is not used for scheduling, which is
 * strictly per-CPU.
 */
#include <stdbool.h>
#include <io.h>
#include <types.h>
#include <string.h>
#include <list.h>
#include <spinlock.h>
#include <klog.h>
#include <panic.h>
#include <kstring.h>
#include <percpu.h>
#include <task.h>
#include <sched.h>
#include <process.h>
#include <syscall.h>

KLOG_SUBSYSTEM("task");

/* PIDs. PID 0 is the idle task and PID 1 is init, so the counter starts at 2
 * and neither of those can ever collide with a user-visible pid. Allocation is
 * a single fetch-add: pids are dense and never reused, which is what makes a
 * pid from a log line interpretable after the fact. */
#define FIRST_USER_PID 2

static volatile u32 next_pid = FIRST_USER_PID;
spinlock_t task_all_lock = SPINLOCK_INIT;
struct list_head task_all_list = LIST_HEAD_INIT(task_all_list);

static u32 alloc_pid(void)
{
    return __atomic_fetch_add(&next_pid, 1, __ATOMIC_RELAXED);
}

/* ------------------------------------------------------------------ alloc --- */

/*
 * Build the saved-register block a fresh task will resume into. It lives
 * at the top of the task's kernel stack, with `rip` pointing at
 * task_trampoline so the first switch looks like a return from
 * context_switch().
 */
static void *build_initial_frame(void *stack_top)
{
    /* 64 bytes: the frame base must be 16-byte aligned, and
     * sizeof(struct context) is 56, so landing on an exact
     * multiple would leave post-`ret` stack 8 mod 16 instead of the
     * ABI wants at function entry. The extra 8 bytes cost
     * nothing and keep the arithmetic obvious.
     */
    struct context *c = (struct context *)((u64)stack_top - 64);

    c->r15 = 0;
    c->r14 = 0;
    c->r13 = 0;
    c->r12 = 0;
    c->rbx = 0;
    c->rbp = 0;
    c->rip = (u64)task_trampoline;
    return c;
}

struct task *task_alloc(const char *name, struct address_space *mm)
{
    struct task *t = kmalloc_zeroed(sizeof(*t));

    if (!t)
        return NULL;

    t->pid = alloc_pid();
    t->tid = t->pid;
    t->ppid = 0;
    t->pgid = t->pid;
    t->sid = t->pid;
    t->state = TASK_NEW;
    t->mm = mm;
    refcount_set(&t->refs, 1);

    /*
     * A task born into a process shares that process's address space; the
     * reference is what keeps it alive while this task holds it.
     */
    if (mm)
        mm_get(mm);

    t->kernel_stack = kstack_alloc(TASK_KERNEL_STACK_SIZE);
    if (!t->kernel_stack) {
        kfree(t, sizeof(*t));
        return NULL;
    }

    /*
     * FXSAVE writes 512 bytes and the allocation is a power of two larger
     * so a future XSAVE-format area needs no allocator change. The extra
     * 64 bytes cover the alignment adjustment below: kmalloc's guarantee is
     * 8-byte alignment, and fxsave wants 16.
     */
    u64 *fpu = kmalloc(TASK_FPU_STATE_SIZE + 64);
    if (!fpu) {
        kstack_free(t->kernel_stack);
        kfree(t, sizeof(*t));
        return NULL;
    }
    t->fpu_state = (u64 *)ALIGN_UP((u64)fpu, 64);
    /*
     * Initialise from the CPU's own state rather than from zero. A zeroed
     * FXSAVE image restored into a task gives that task an x87 control
     * word of 0 — every exception masked, no precision control set — and
     * an MXCSR of 0, which denormalises are trapped on. Capturing the
     * reset values the CPU came up with is both free and correct.
     */
    fpu_save(t->fpu_state);

    t->fds = kmalloc_zeroed(sizeof(struct file *) * TASK_MAX_FDS);
    if (!t->fds) {
        kfree((void *)((u64)t->fpu_state & ~63ULL), TASK_FPU_STATE_SIZE + 64);
        kstack_free(t->kernel_stack);
        kfree(t, sizeof(*t));
        return NULL;
    }
    t->fd_count = TASK_MAX_FDS;

    t->policy = SCHED_NORMAL;
    t->nice = 0;
    t->mlfq_level = 0;
    t->exec_budget = MLFQ_QUANTUM_0;
    t->context_rsp = (u64)build_initial_frame((void *)((u64)t->kernel_stack +
                            TASK_KERNEL_STACK_SIZE));

    list_init(&t->rq_node);
    list_init(&t->sleep_node);
    list_init(&t->sibling);
    list_init(&t->children);
    list_init(&t->all_node);

    cpumask_setall(&t->cpumask);
    t->affinity_mask = 0;
    t->cred.uid = 0;
    t->cred.gid = 0;
    t->cred.caps = 0;

    if (name)
        strlcpy(t->comm, name, TASK_COMM_LEN);
    else
        strlcpy(t->comm, "task", TASK_COMM_LEN);

    u64 flags = spinlock_irqsave(&task_all_lock);
    list_add_tail(&t->all_node, &task_all_list);
    spinlock_unlock_irqrestore(&task_all_lock, flags);

    return t;
}

struct task *task_create_kernel(void (*fn)(void *), void *arg, const char *name)
{
    struct task *t = task_alloc(name, NULL);

    if (!t)
        return NULL;

    t->thread_fn = fn;
    t->thread_arg = arg;
    /* Kernel threads must never displace real work, so they start at the
     * bottom of the queue with a full budget there. */
    t->mlfq_level = MLFQ_LEVELS - 1;
    t->policy = SCHED_NORMAL;
    t->exec_budget = MLFQ_QUANTUM_7;
    return t;
}

void task_inherit(struct task *child, struct task *parent)
{
    child->ppid = parent->pid;
    child->pgid = parent->pgid;
    child->sid = parent->sid;
    child->nice = parent->nice;
    child->policy = parent->policy;
    child->cred = parent->cred;
    cpumask_copy(&child->cpumask, &parent->cpumask);
    child->affinity_mask = parent->affinity_mask;
    /*
     * A fork duplicates the address space, so the child gets its own copy of
     * the TLS block at the same address the parent's is at, and the same
     * thread pointer describes it. Copying the field rather than the
     * register is the point: the register holds whatever the *parent* last
     * installed, which is the same number here, but reading it would make
     * the child's state depend on whether the parent had run a syscall
     * since it was last switched to.
     */
    child->fs_base = parent->fs_base;
}

/*
 * The two halves of moving a thread pointer between the register and the task.
 *
 * MSR_FS_BASE is per-CPU and a thread pointer is per-task, so the scheduler
 * calls these either side of a switch, next to the CR3 and FPU handling it
 * already does there. Keeping them here rather than in sched.c is what lets
 * the ring-3 entry path in interrupt_entry.S use the same one, instead of
 * growing a second copy of the write.
 */
void task_load_fs_base(struct task *t)
{
    if (t)
        wrmsr(MSR_FS_BASE, t->fs_base);
}

void task_save_fs_base(struct task *t)
{
    if (t)
        t->fs_base = rdmsr(MSR_FS_BASE);
}

/*
 * Load the running task's thread pointer into MSR_FS_BASE. This is the one
 * entry point that takes no task, because it is the one the ring-3 return path
 * can actually use.
 *
 * ret_to_user() in interrupt_entry.S is reached from two places that need it:
 *
 *   - process_enter_user(), for a process's *first* entry. FS is still whatever
 *     the last process left there, so without this the process would run on a
 *     thread pointer that belongs to somebody else. It happens to be harmless
 *     for a process whose startup calls arch_prctl, and fatal for one that
 *     does not -- the failure surfaces as a read of another process's memory.
 *
 *   - fork_child_start(), for every forked child. This is the load-bearing
 *     case: the child resumes at its parent's syscall return site with RAX=0
 *     and never re-runs crt1, so nothing will ever call arch_prctl on its
 *     behalf. Without the load the child's first `__thread` access -- and libc
 *     reaches for errno on the first failing call -- is a read through the
 *     parent's thread pointer.
 *
 * NULL-safe by delegation: before the scheduler starts there is no current task
 * and this leaves MSR_FS_BASE alone.
 */
void task_load_fs_current(void)
{
    task_load_fs_base(current_task());
}

/*
 * A task promoted to level 0, a new task, or a task whose budget was reset by
 * an aging boost all get the full quantum of their level rather than whatever
 * was left of the previous one. Halving the remaining budget on demotion (as
 * opposed to resetting it) would let a task that has already burned most of a
 * slice skip the demotion penalty by timing its yield, which is exactly the
 * gaming the design is meant to prevent.
 */
void task_reset_budget(struct task *t)
{
    static const u64 quantum[MLFQ_LEVELS] = {
        MLFQ_QUANTUM_0, MLFQ_QUANTUM_1, MLFQ_QUANTUM_2, MLFQ_QUANTUM_3,
        MLFQ_QUANTUM_4, MLFQ_QUANTUM_5, MLFQ_QUANTUM_6, MLFQ_QUANTUM_7,
    };

    u8 level = t->mlfq_level;

    if (level >= MLFQ_LEVELS)
        level = MLFQ_LEVELS - 1;
    t->exec_budget = quantum[level];
    t->rt_slice = RT_RR_QUANTUM;
}

/* ---------------------------------------------------------------- lookup ---- */

struct task *current_task(void)
{
    return (struct task *)this_cpu()->current;
}

struct task *task_find_by_pid(u32 pid)
{
    struct task *found = NULL;
    u64 flags = spinlock_irqsave(&task_all_lock);
    struct list_head *pos;

    list_for_each(pos, &task_all_list) {
        struct task *t = list_entry(pos, struct task, all_node);

        if (t->pid == pid && !t->reaped) {
            found = t;
            break;
        }
    }
    spinlock_unlock_irqrestore(&task_all_lock, flags);
    return found;
}

struct task *task_find_by_tid(u32 tid)
{
    struct task *found = NULL;
    u64 flags = spinlock_irqsave(&task_all_lock);
    struct list_head *pos;

    list_for_each(pos, &task_all_list) {
        struct task *t = list_entry(pos, struct task, all_node);

        if (t->tid == tid && !t->reaped) {
            found = t;
            break;
        }
    }
    spinlock_unlock_irqrestore(&task_all_lock, flags);
    return found;
}

void task_for_each(int (*fn)(struct task *, void *), void *arg)
{
    u64 flags = spinlock_irqsave(&task_all_lock);
    struct list_head *pos, *tmp;

    list_for_each_safe(pos, tmp, &task_all_list)
        fn(list_entry(pos, struct task, all_node), arg);
    spinlock_unlock_irqrestore(&task_all_lock, flags);
}

/* Count live tasks sharing an address space. Used to decide whether the last
 * thread of a process is exiting and the page tables can go. */
static u32 count_mm_users(struct address_space *mm)
{
    u32 n = 0;
    u64 flags = spinlock_irqsave(&task_all_lock);
    struct list_head *pos;

    list_for_each(pos, &task_all_list) {
        struct task *t = list_entry(pos, struct task, all_node);

        if (t->mm == mm && !t->reaped && t->state != TASK_DEAD)
            n++;
    }
    spinlock_unlock_irqrestore(&task_all_lock, flags);
    return n;
}

/* ------------------------------------------------------------- lifecycle ---- */

void task_set_state(struct task *t, task_state_t state)
{
    t->state = state;
}

void task_get(struct task *t)
{
    refcount_inc(&t->refs);
}

/*
 * Release everything a dead task owns. Order matters: the address space is
 * dropped before the kernel stack, because the stack is mapped in the direct
 * map and the fault that would report the mistake is far away from where it is
 * made.
 */
void task_release_resources(struct task *t)
{
    if (t->fds) {
        fd_table_close_all(t);
        kfree(t->fds, sizeof(struct file *) * TASK_MAX_FDS);
        t->fds = NULL;
        t->fd_count = 0;
    }

    if (t->mm) {
        /*
         * The last thread out drops the page tables. A task that shares
         * an address space with CLONE_VM siblings leaves its reference
         * alone, which is the whole reason the refcount lives on mm.
         */
        if (count_mm_users(t->mm) == 0)
            mm_put(t->mm);
        t->mm = NULL;
    }

    if (t->fpu_state) {
        kfree((void *)((u64)t->fpu_state & ~63ULL),
              TASK_FPU_STATE_SIZE + 64);
        t->fpu_state = NULL;
    }

    /*
     * The kernel stack is deliberately NOT freed here. A task that has
     * exited is still running on its own stack — task_exit_current() calls
     * this function and then continues to sched_stop_current(), which
     * switches away. Freeing the stack before the switch would hand the
     * CPU a freed page. The parent reaps the zombie through task_put(),
     * whose refcount-zero path calls this function again and frees the
     * stack then, when it is safe.
     */
}

void task_put(struct task *t)
{
    if (!t)
        return;
    if (refcount_dec_and_test(&t->refs)) {
        u64 flags = spinlock_irqsave(&task_all_lock);

        list_del(&t->all_node);
        spinlock_unlock_irqrestore(&task_all_lock, flags);
        task_release_resources(t);
        /* Free the kernel stack last. task_release_resources() deliberately
         * does not free it: a task that has exited is still running on its
         * own stack until sched_stop_current() switches away, and freeing it
         * earlier would hand the CPU a freed page. Here the task is gone, so
         * it is safe. */
        if (t->kernel_stack) {
            kstack_free(t->kernel_stack);
            t->kernel_stack = NULL;
        }
        kfree(t, sizeof(*t));
    }
}

__noreturn void task_trampoline(void)
{
    struct task *t = current_task();
    void (*fn)(void *) = t->thread_fn;
    void *arg = t->thread_arg;

    /*
     * fn is loaded before the call and not re-read afterwards: the callee
     * may exit this task, and reading through `t` after that would be
     * reading a freed object.
     */
    if (fn)
        fn(arg);

    task_exit_current(0);
    __builtin_unreachable();
}

__noreturn void task_exit_current(int code)
{
    struct task *t = current_task();

    if (!t)
        panic("task_exit_current with no current task");

    klog_emit(KLOG_DEBUG, "task", "task_exit_current: pid=%u code=%d parent=%p",
          t->pid, code, (void *)t->parent);

    t->exit_code = code;

    /*
     * A zombie still has an exit status for its parent, so it is not
     * detached here — the parent reaps it. Until then the task holds no
     * run queue position and will not be scheduled.
     */
    t->state = TASK_ZOMBIE;
    t->detached = true;
    t->reaped = false;

    sched_remove(t);

    /*
     * Release the address space and descriptors now rather than at reap
     * time: a process that has exited should not keep its page tables
     * pinned for as long as its parent takes to notice.
     */
    task_release_resources(t);

    /* Whoever is in wait4() for this task has to be told. */
    if (t->parent && (t->parent->state == TASK_BLOCKED || t->parent->state == TASK_RUNNABLE))
        sched_wake(t->parent);
    else if (t->parent)
        klog_emit(KLOG_DEBUG, "task", "task %u exited, parent pid=%u state=%d not blocked",
                  t->pid, t->parent->pid, (int)t->parent->state);
    else
        klog_emit(KLOG_DEBUG, "task", "task %u exited, no parent", t->pid);

    klog(KLOG_DEBUG, "task %u exited with %d", t->pid, code);

    /*
     * Tell the syscall entry path that this task's frame, if it is sitting
     * in one, must not be returned to. Must happen before the switch away:
     * after it, the code that would set the flag belongs to a task this CPU
     * is no longer running.
     */
    klog_emit(KLOG_DEBUG, "task", "task %u about to abandon frame", t->pid);
    syscall_abandon_frame();

    klog_emit(KLOG_DEBUG, "task", "task %u about to stop current", t->pid);
    /* Hands the CPU to someone else and never comes back. */
    sched_stop_current();
}

void *task_bootstrap_stack_top(void)
{
    static void *boot_stack;

    if (!boot_stack)
        boot_stack = kstack_alloc(TASK_KERNEL_STACK_SIZE);
    return boot_stack ? (void *)((u64)boot_stack + TASK_KERNEL_STACK_SIZE)
              : NULL;
}
