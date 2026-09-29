/*
 * sched.c — the MLFQ scheduler.
 *
 * Shape of the thing
 * ------------------
 * Per CPU there is one struct runqueue holding four things: a deadline list, an
 * RT priority array, eight MLFQ levels, and the idle task. Nothing here is
 * global except the migration list, which exists so that a wakeup on a remote
 * CPU never takes a remote lock.
 *
 * Selection is a strict cascade:
 *
 *   1. a runnable SCHED_DEADLINE task, earliest absolute deadline first
 *   2. a runnable SCHED_FIFO/SCHED_RR task, highest RT priority first
 *   3. the lowest-numbered non-empty MLFQ level
 *   4. the idle task
 *
 * Strict priority rather than a unified score is what makes the RT classes
 * mean what POSIX says they mean: an RT task is not "usually" first, it is
 * first, and no amount of normal work can delay it.
 *
 * Tick accounting
 * ---------------
 * Every tick charges one tick of exec_budget to the running task. Hitting zero
 * demotes one level and recharges the new level's full quantum. A voluntary
 * yield does *not* recharge: that asymmetry is the entire anti-gaming property
 * of the design. A task that yields just before its budget expires gets its
 * place back in the same level but has still spent the budget, so the next
 * tick demotes it anyway.
 *
 * Aging is the other half. Once per SCHED_AGING_INTERVAL ticks, every runnable
 * task in level L that has been waiting longer than L * MLFQ_AGE_TICKS is moved
 * up one level with a fresh quantum. The multiplier is what makes the guarantee
 * directional: the deeper a task is buried, the longer it must wait, so a deep
 * task always drains towards the top faster than new arrivals can pile onto the
 * level it is leaving.
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
#include <syscall.h>

/* klog.h's KLOG_SUBSYSTEM macro collides with its own extern declaration of the
 * same name, so the subsystem tag is passed explicitly. */
#define SCHED_LOG(level, ...)                                                \
	do {                                                               \
		if ((level) >= klog_runtime_level)                         \
			klog_emit((level), "sched", __VA_ARGS__);          \
	} while (0)

/*
 * Pointers, not the run queues themselves. A struct runqueue is about 1.9 KiB
 * (the RT priority array is 100 doubly-linked heads), so 256 of them would be
 * half a megabyte of BSS — a third of the kernel image budget, spent on CPUs
 * that will never run here. Each CPU allocates its own in sched_init(), which
 * is where the per-CPU isolation actually has to be established anyway.
 */
struct runqueue *sched_runqueues[MAX_CPUS];

/* list.h provides list_entry but not the front-of-list accessor the dequeue
 * paths need; defined here rather than in list.h because that header is not
 * this subsystem's to change. */
#define sched_first_entry(head, type, member) \
	((type *)((char *)((head)->next) - offsetof(type, member)))

/*
 * The migration list. A task placed here is on no CPU's run queue and is
 * claimed by whichever CPU next looks for work; that is how a wakeup that
 * landed on the wrong CPU gets to the right one without the waker taking the
 * destination's lock.
 */
static spinlock_t global_lock = SPINLOCK_INIT;
static struct list_head global_queue = LIST_HEAD_INIT(global_queue);
static volatile u64 global_count;

/* Per-CPU sleep lists, ordered by wake_tick. Short by design: a kernel this
 * size has tens of sleepers, not thousands, and a sorted insertion keeps the
 * expiry walk O(expired) instead of O(all). */
static spinlock_t sleep_lock[MAX_CPUS];
static struct list_head sleep_queue[MAX_CPUS];

/* Ticks since boot, per CPU. Kept per-CPU rather than read from a single
 * counter so two CPUs never write the same cache line on the tick path. */
static u64 cpu_ticks[MAX_CPUS];

/*
 * Whether this CPU has handed control to the scheduler. Before that point the
 * "current" task is a placeholder installed by sched_init(), and preemption
 * must not fire: the task being created is not finished being created, and
 * switching to it would strand its creator half-way through.
 */
static bool cpu_started[MAX_CPUS];

static inline u64 now_ticks(void)
{
	return cpu_ticks[this_cpu_id()];
}

u64 sched_now_ticks(void)
{
	return now_ticks();
}

u64 sched_now_ns(void)
{
	/* 1 kHz tick, so a tick is a millisecond. */
	return now_ticks() * 1000000ULL;
}

struct task *sched_idle_task(void)
{
	return (struct task *)this_cpu()->idle_task;
}

/* Forward: the idle task consults it before every hlt. */
static bool rq_has_work_locked(const struct runqueue *rq);

/* ------------------------------------------------------------ idle task ----- */

static u64 idle_ticks;

/*
 * The idle task. It is a real schedulable task rather than a special case in
 * schedule(), which means the "nothing is runnable" path is the same code as
 * every other path and cannot diverge from it.
 */
static __noreturn void idle_thread(void *arg)
{
	struct runqueue *rq = sched_runqueues[this_cpu_id()];

	UNUSED(arg);

	for (;;) {
		u64 flags = spinlock_irqsave(&rq->lock);
		bool work = rq_has_work_locked(rq);

		spinlock_unlock_irqrestore(&rq->lock, flags);

		if (work || global_count) {
			schedule();
			continue;
		}

		/*
		 * hlt rather than a spin: with no runnable task there is
		 * nothing to wait for except a timer interrupt, and halting
		 * stops the CPU from stealing the bus from whatever is about
		 * to make something runnable. The tick wakes the CPU and
		 * sched_tick() re-checks, so a wakeup cannot be missed by
		 * having gone to sleep over it.
		 */
		hlt();
		idle_ticks++;
		per_cpu(idle_ticks)++;
	}
}

static struct task *idle_task_create(void)
{
	struct task *t = task_create_kernel(idle_thread, NULL, "swapper");

	if (!t)
		panic("cannot create the idle task");

	/*
	 * PID 0. task_alloc() hands out from an atomic counter starting at 2,
	 * so the idle task's own allocation is discarded and the identity
	 * fixed up here: the idle task must not be waitable, killable, or
	 * findable through a pid lookup.
	 */
	t->pid = 0;
	t->tid = 0;
	t->ppid = 0;
	t->pgid = 0;
	t->sid = 0;
	t->policy = SCHED_IDLE;
	t->mlfq_level = MLFQ_LEVELS - 1;
	t->nice = 19;
	t->reaped = true;
	return t;
}

/* ------------------------------------------------------------- rt array ----- */

static inline void rt_bitmap_set(struct rt_prio_array *rt, u32 prio)
{
	rt->bitmap[prio / 64] |= 1ULL << (prio % 64);
}

static inline void rt_bitmap_clear(struct rt_prio_array *rt, u32 prio)
{
	rt->bitmap[prio / 64] &= ~(1ULL << (prio % 64));
}

/* Highest set bit, or -1. Two words because 100 priorities do not fit in one. */
static inline int rt_bitmap_highest(const struct rt_prio_array *rt)
{
	if (rt->bitmap[1])
		return 64 + (63 - __builtin_clzll(rt->bitmap[1]));
	if (rt->bitmap[0])
		return 63 - __builtin_clzll(rt->bitmap[0]);
	return -1;
}

/* ------------------------------------------------------------ enqueue -------- */

/*
 * All queue manipulation happens with the run queue lock held. `head` selects
 * the end: the head of the list is the task that has waited longest at this
 * level, and putting a freshly woken I/O-bound task there is the interactive
 * latency win the design is after.
 */
static void rq_enqueue_locked(struct runqueue *rq, struct task *t, bool head)
{
	t->rq_cpu = this_cpu_id();
	t->on_rq = true;
	t->rq_node.prev = &t->rq_node;
	t->rq_node.next = &t->rq_node;
	rq->nr_running++;

	switch (t->policy) {
	case SCHED_DEADLINE: {
		/* Keep the list sorted by absolute deadline. Insertion is a
		 * short walk because the list is the size of the RT workload,
		 * and a sorted list means dequeue is a single pop_front with
		 * no search at all. */
		struct list_head *pos = rq->deadline.next;

		while (pos != &rq->deadline) {
			struct task *o = list_entry(pos, struct task, rq_node);

			if (o->abs_deadline <= t->abs_deadline)
				break;
			pos = pos->next;
		}
		__list_add(&t->rq_node, pos->prev, pos);
		rq->deadline_count++;
		break;
	}
	case SCHED_FIFO:
	case SCHED_RR: {
		u32 prio = t->rt_priority;

		if (prio >= RT_PRIORITIES)
			prio = RT_PRIORITIES - 1;
		if (head)
			list_add(&t->rq_node, &rq->rt.queue[prio]);
		else
			list_add_tail(&t->rq_node, &rq->rt.queue[prio]);
		rt_bitmap_set(&rq->rt, prio);
		break;
	}
	default: {
		u8 level = t->mlfq_level;

		if (level >= MLFQ_LEVELS)
			level = MLFQ_LEVELS - 1;
		if (head)
			list_add(&t->rq_node, &rq->mlfq[level].head);
		else
			list_add_tail(&t->rq_node, &rq->mlfq[level].head);
		rq->mlfq[level].count++;
		rq->mlfq_bitmap |= 1u << level;
		break;
	}
	}
}

static void rq_dequeue_locked(struct runqueue *rq, struct task *t)
{
	switch (t->policy) {
	case SCHED_DEADLINE:
		if (rq->deadline_count)
			rq->deadline_count--;
		break;
	case SCHED_FIFO:
	case SCHED_RR: {
		u32 prio = t->rt_priority;

		if (prio >= RT_PRIORITIES)
			prio = RT_PRIORITIES - 1;
		if (list_empty(&rq->rt.queue[prio]))
			rt_bitmap_clear(&rq->rt, prio);
		break;
	}
	default: {
		u8 level = t->mlfq_level;

		if (level >= MLFQ_LEVELS)
			level = MLFQ_LEVELS - 1;
		if (rq->mlfq[level].count)
			rq->mlfq[level].count--;
		if (rq->mlfq[level].count == 0)
			rq->mlfq_bitmap &= ~(1u << level);
		break;
	}
	}

	list_del(&t->rq_node);
	t->on_rq = false;
	if (rq->nr_running)
		rq->nr_running--;
}

/*
 * Dequeue the highest-priority runnable task. Returns NULL when only the idle
 * task is left, which the caller turns into "run idle".
 */
static struct task *rq_pick_locked(struct runqueue *rq)
{
	if (rq->deadline_count) {
		struct task *t = sched_first_entry(&rq->deadline, struct task, rq_node);

		rq_dequeue_locked(rq, t);
		return t;
	}

	int prio = rt_bitmap_highest(&rq->rt);

	if (prio >= 0) {
		struct task *t = sched_first_entry(&rq->rt.queue[prio], struct task, rq_node);

		rq_dequeue_locked(rq, t);
		return t;
	}

	if (rq->mlfq_bitmap) {
		u32 level = (u32)__builtin_ctz(rq->mlfq_bitmap);
		struct task *t = sched_first_entry(&rq->mlfq[level].head, struct task, rq_node);

		rq_dequeue_locked(rq, t);
		return t;
	}

	return NULL;
}

/*
 * Non-destructive "is there anything to run". The idle task needs the answer
 * without the side effect rq_pick_locked() has of removing what it found.
 */
static bool rq_has_work_locked(const struct runqueue *rq)
{
	if (rq->deadline_count)
		return true;
	if (rq->mlfq_bitmap)
		return true;
	return rt_bitmap_highest(&rq->rt) >= 0;
}

/* Does the run queue hold anything that outranks `t`? */
static bool rq_has_higher_locked(const struct runqueue *rq, const struct task *t)
{
	if (t->policy == SCHED_DEADLINE) {
		/* Among deadline tasks, only a strictly earlier deadline wins. */
		struct list_head *pos = rq->deadline.next;

		while (pos != &rq->deadline) {
			const struct task *o = list_entry(pos, struct task,
							  rq_node);

			if (o->abs_deadline < t->abs_deadline)
				return true;
			pos = pos->next;
		}
		return false;
	}

	if (t->policy == SCHED_FIFO || t->policy == SCHED_RR) {
		if (rq->deadline_count)
			return true;
		return rt_bitmap_highest(&rq->rt) > (int)t->rt_priority;
	}

	/* Normal work: any deadline or RT task, or a higher MLFQ level. */
	if (rq->deadline_count || rt_bitmap_highest(&rq->rt) >= 0)
		return true;

	if (!rq->mlfq_bitmap)
		return false;
	u32 best = (u32)__builtin_ctz(rq->mlfq_bitmap);

	return best < t->mlfq_level;
}

/* --------------------------------------------------------- global queue ----- */

static void global_push(struct task *t)
{
	u64 flags = spinlock_irqsave(&global_lock);

	t->in_global = true;
	list_add_tail(&t->rq_node, &global_queue);
	global_count++;
	spinlock_unlock_irqrestore(&global_lock, flags);
}

/* Claim one task from the migration list, if there is one. */
static struct task *global_pop(void)
{
	struct task *t = NULL;
	u64 flags = spinlock_irqsave(&global_lock);

	if (!list_empty(&global_queue)) {
		t = sched_first_entry(&global_queue, struct task, rq_node);
		list_del(&t->rq_node);
		list_init(&t->rq_node);
		t->in_global = false;
		if (global_count)
			global_count--;
	}
	spinlock_unlock_irqrestore(&global_lock, flags);
	return t;
}

/* ------------------------------------------------------------- placement ---- */

u32 sched_select_cpu(const cpumask_t *mask, u32 preferred)
{
	if (!mask || cpumask_empty(mask))
		return 0;
	if (preferred < MAX_CPUS && cpumask_test(mask, preferred))
		return preferred;
	for (u32 i = 0; i < MAX_CPUS; i++)
		if (cpumask_test(mask, i))
			return i;
	return 0;
}

void sched_migrate(struct task *t, u32 cpu)
{
	if (cpu >= MAX_CPUS)
		cpu = 0;
	if (!cpumask_test(&t->cpumask, cpu))
		cpu = sched_select_cpu(&t->cpumask, cpu);
	t->rq_cpu = cpu;
	/* The actual move is performed by sched_remote_pull() on the target
	 * CPU; a task sitting on the global list is already "migrated" in the
	 * only sense that matters until then. */
	if (!t->on_rq)
		global_push(t);
}

int sched_set_affinity(struct task *t, const cpumask_t *mask)
{
	if (cpumask_empty(mask))
		return -EINVAL;

	cpumask_copy(&t->cpumask, mask);

	/*
	 * Collapse the common single-CPU case into a bitmask, so the placement
	 * test on the wakeup path is a compare rather than a word load and a
	 * bit test against a cache line that the task is already touching.
	 */
	u32 only = sched_select_cpu(mask, t->rq_cpu);
	u64 single = 0;

	if (cpumask_test(mask, only)) {
		bool multi = false;

		for (u32 i = 0; i < MAX_CPUS; i++) {
			if (cpumask_test(mask, i) && i != only) {
				multi = true;
				break;
			}
		}
		if (!multi)
			single = 1ULL << only;
	}
	t->affinity_mask = single;

	if (t->on_rq && !cpumask_test(mask, t->rq_cpu) &&
	    sched_runqueues[t->rq_cpu < MAX_CPUS ? t->rq_cpu : 0]) {
		/* Leaving a run queue from outside its CPU's lock would race
		 * with the tick walking the same list, so the move is deferred
		 * to the global list and picked up by whichever CPU next looks
		 * for work. */
		u64 flags = spinlock_irqsave(&sched_runqueues[t->rq_cpu]->lock);

		if (t->on_rq) {
			rq_dequeue_locked(sched_runqueues[t->rq_cpu], t);
			global_push(t);
		}
		spinlock_unlock_irqrestore(&sched_runqueues[t->rq_cpu]->lock, flags);
	}
	return 0;
}

void sched_add(struct task *t)
{
	struct runqueue *rq = sched_runqueues[this_cpu_id()];
	u64 flags = spinlock_irqsave(&rq->lock);

	t->state = TASK_RUNNABLE;
	t->detached = false;
	t->last_run = now_ticks();
	rq_enqueue_locked(rq, t, false);

	/* A task that outranks what is running preempts it immediately. */
	bool preempt = cpu_started[this_cpu_id()] && rq->current &&
		       rq_has_higher_locked(rq, rq->current);
	spinlock_unlock_irqrestore(&rq->lock, flags);

	if (preempt)
		schedule();
}

void sched_remove(struct task *t)
{
	if (t->in_global) {
		u64 flags = spinlock_irqsave(&global_lock);

		if (t->in_global) {
			list_del(&t->rq_node);
			list_init(&t->rq_node);
			t->in_global = false;
			if (global_count)
				global_count--;
		}
		spinlock_unlock_irqrestore(&global_lock, flags);
	}

	if (!t->on_rq)
		return;

	u32 cpu = t->rq_cpu < MAX_CPUS ? t->rq_cpu : 0;
	struct runqueue *rq = sched_runqueues[cpu];
	u64 flags;

	/* A CPU that has not run sched_init() has no queue to remove from. */
	if (!rq)
		return;
	flags = spinlock_irqsave(&rq->lock);
	if (t->on_rq)
		rq_dequeue_locked(rq, t);
	spinlock_unlock_irqrestore(&rq->lock, flags);
}

/*
 * Drain the migration list. Called at the top of schedule() so a task whose
 * wakeup landed on the wrong CPU becomes visible locally without any
 * cross-CPU locking on the wakeup path itself.
 */
static void sched_drain_global(struct runqueue *rq)
{
	struct task *t;

	while ((t = global_pop()) != NULL) {
		u64 flags = spinlock_irqsave(&rq->lock);

		rq_enqueue_locked(rq, t, false);
		spinlock_unlock_irqrestore(&rq->lock, flags);
	}
}

/* ---------------------------------------------------------------- waking ---- */

void sched_wake(struct task *t)
{
	if (!t || t->detached)
		return;

	if (t->state != TASK_BLOCKED && t->state != TASK_NEW)
		return;

	/*
	 * I/O boost. A task that just stopped blocking on I/O is, by
	 * construction, latency-sensitive: it has data waiting or a
	 * dependency resolved, and every tick it spends in a queue is a tick
	 * its requestor waits. It is placed at the *head* of its level rather
	 * than promoted, which gives the same latency win without letting a
	 * task that cycles through short reads climb the queue indefinitely.
	 * The one-level promotion with the io_boost latch is the doc's rule
	 * for exactly that concern, and it is applied first.
	 */
	if (t->mlfq_level > 0 && !t->io_boost && t->policy != SCHED_FIFO &&
	    t->policy != SCHED_RR && t->policy != SCHED_DEADLINE) {
		t->mlfq_level--;
		t->io_boost = 1;
		task_reset_budget(t);
	}

	struct runqueue *rq = sched_runqueues[this_cpu_id()];
	u64 flags = spinlock_irqsave(&rq->lock);

	if (!t->on_rq) {
		t->state = TASK_RUNNABLE;
		rq_enqueue_locked(rq, t, true);
	}
	spinlock_unlock_irqrestore(&rq->lock, flags);

	/* Preempt if the woken task belongs on this CPU and outranks us. */
	flags = spinlock_irqsave(&rq->lock);
	bool preempt = cpu_started[this_cpu_id()] && rq->current &&
		       rq_has_higher_locked(rq, rq->current);
	spinlock_unlock_irqrestore(&rq->lock, flags);

	if (preempt)
		schedule();
}

u32 sched_task_cpu(struct task *t)
{
	if (!t)
		return 0;
	if (t->on_rq)
		return t->rq_cpu;
	return this_cpu_id();
}

/* ------------------------------------------------------------- switching ---- */

/*
 * The one place a context switch happens. Everything else — the tick, a yield,
 * a block, a preemption — funnels through here so the FPU policy, the address
 * space switch, and the syscall stack update are applied exactly once.
 *
 * FPU policy: save and restore on every switch, unconditionally, rather than
 * tracking CR0.TS. The lazy alternative costs a #NM on the first FPU
 * instruction in every task and needs a per-task "has this task ever used the
 * FPU" bit that is wrong the moment a task is preempted between the use and
 * the save. 512 bytes on a switch that already writes CR3 and the TSS RSP0 is
 * not the bottleneck; being wrong about register state is.
 */
static __noreturn void context_switch_to(struct task *next)
{
	struct task *prev = current_task();
	struct runqueue *rq = sched_runqueues[this_cpu_id()];

	if (prev == next)
		panic("scheduler switched to the task it is already running");

	rq->current = next;
	next->state = TASK_RUNNING;
	next->last_run = now_ticks();
	per_cpu(current) = next;
	per_cpu(ctx_switches)++;
	rq->nr_switches++;

	/*
	 * The syscall entry path has no stack of its own to switch to: SYSCALL
	 * does not change RSP, so the entry code loads whatever this field
	 * says. Pointing it at the incoming task's stack is what makes a task
	 * preempted mid-syscall find its own frame when it comes back.
	 */
	syscall_set_kernel_stack((void *)((u64)next->kernel_stack +
					  TASK_KERNEL_STACK_SIZE));

	/*
	 * The same value goes into the TSS RSP0, which is where an interrupt
	 * arriving on this CPU lands. Without this, two tasks preempted at
	 * different moments would both have their interrupted frames on one
	 * shared interrupt stack, and the second would overwrite the first —
	 * a corruption that shows up as an unrelated task returning to a
	 * nonsense continuation.
	 */
	tss_set_kernel_stack((void *)((u64)next->kernel_stack +
				      TASK_KERNEL_STACK_SIZE));

	/* The address space changes only when the mm actually differs, so a
	 * CLONE_VM thread switch costs no TLB flush at all. */
	if (prev->mm != next->mm) {
		if (next->mm)
			write_cr3(next->mm->pgd);
		else
			vmm_switch_to_kernel_pgd();
	}

	if (prev->fpu_state)
		fpu_save(prev->fpu_state);
	if (next->fpu_state)
		fpu_restore(next->fpu_state);

	context_switch(&prev->context_rsp, next->context_rsp);
	__builtin_unreachable();
}

__noreturn void sched_switch_to_new(struct task *t)
{
	context_restore(t->context_rsp);
}

void schedule(void)
{
	struct runqueue *rq = sched_runqueues[this_cpu_id()];
	struct task *prev = current_task();
	struct task *next;
	u64 flags;

	/* Anything the migration list is holding is ours to adopt. */
	sched_drain_global(rq);

	/*
	 * Pick first, requeue the outgoing task second. The outgoing task is
	 * not on the queue while it runs, so it can never be the one picked;
	 * the only way it comes back onto a queue is here, and the only way it
	 * stays off is if nothing else is runnable and it simply keeps running.
	 */
	flags = spinlock_irqsave(&rq->lock);
	next = rq_pick_locked(rq);
	spinlock_unlock_irqrestore(&rq->lock, flags);

	if (!next) {
		/* Nothing else is runnable anywhere: keep running. Requeueing
		 * the current task here rather than leaving it off the queue
		 * means a later wakeup finds it and does not have to know that
		 * it was already running. */
		if (prev && prev->state == TASK_RUNNING && !prev->detached) {
			prev->state = TASK_RUNNABLE;
			prev->last_run = now_ticks();
			flags = spinlock_irqsave(&rq->lock);
			rq_enqueue_locked(rq, prev, false);
			spinlock_unlock_irqrestore(&rq->lock, flags);
		}
		return;
	}

	if (prev && prev->state == TASK_RUNNING && !prev->detached) {
		prev->state = TASK_RUNNABLE;
		/* The aging pass measures the wait from the moment a task left
		 * the CPU, so the timestamp is taken here rather than only in
		 * the tick: a task that ran for a long time and then blocked
		 * must not look starved the moment it wakes. */
		prev->last_run = now_ticks();
		flags = spinlock_irqsave(&rq->lock);
		rq_enqueue_locked(rq, prev, false);
		spinlock_unlock_irqrestore(&rq->lock, flags);
	}

	context_switch_to(next);
}

__noreturn void sched_stop_current(void)
{
	struct task *t = current_task();
	struct runqueue *rq = sched_runqueues[this_cpu_id()];

	/*
	 * The task is dead and its saved frame must never be resumed: whatever
	 * the C code below it was going to return into belongs to an object
	 * that is being freed. Switching away with no requeue is the only way
	 * to guarantee that.
	 */
	/*
	 * The state is deliberately left alone: a task that exited is a
	 * ZOMBIE, and wait4() is looking for exactly that. What stops it from
	 * being scheduled is that it is on no queue and `detached` is set, not
	 * the state.
	 */
	t->detached = true;

	u64 flags = spinlock_irqsave(&rq->lock);

	rq->current = rq->idle;
	rq->idle->state = TASK_RUNNING;
	spinlock_unlock_irqrestore(&rq->lock, flags);

	per_cpu(current) = rq->idle;

	if (rq->idle->fpu_state)
		fpu_restore(rq->idle->fpu_state);

	context_restore(rq->idle->context_rsp);
}

void sched_yield(void)
{
	struct task *t = current_task();

	if (!t)
		return;

	/*
	 * A yield requeues at the tail of the current level with the budget
	 * untouched. Deliberately no demotion and no budget reset: a yield is
	 * the task saying "someone else may go now", not "I have used my
	 * share". The tick accounting still charges it, so a task that
	 * yields forever still reaches its budget and is demoted.
	 */
	schedule();
}

void sched_block_current(void)
{
	struct task *t = current_task();

	if (!t)
		return;

	t->state = TASK_BLOCKED;
	task_set_state(t, TASK_BLOCKED);
	schedule();
}

/* ---------------------------------------------------------------- tick ------ */

/*
 * Aging. Walks every level from the bottom up so that a promotion never moves
 * a task to a level the pass has already examined, which would let one pass
 * promote a task several levels and skip the accounting for the ones in
 * between.
 */
static void sched_aging_pass(struct runqueue *rq, u64 now)
{
	u64 flags = spinlock_irqsave(&rq->lock);

	for (int level = MLFQ_LEVELS - 1; level > 0; level--) {
		if (!rq->mlfq[level].count)
			continue;

		u64 threshold = (u64)level * MLFQ_AGE_TICKS;
		struct list_head *pos = rq->mlfq[level].head.next;

		while (pos != &rq->mlfq[level].head) {
			struct task *t = list_entry(pos, struct task, rq_node);
			struct list_head *next = pos->next;

			if (now - t->last_run > threshold) {
				rq_dequeue_locked(rq, t);
				t->mlfq_level = (u8)(level - 1);
				t->io_boost = 0;
				task_reset_budget(t);
				t->last_run = now;
				rq_enqueue_locked(rq, t, false);
			}
			pos = next;
		}
	}
	spinlock_unlock_irqrestore(&rq->lock, flags);
}

/* Hand every expired sleeper on this CPU back to the run queue. */
static void sched_wake_sleepers(struct runqueue *rq, u64 now)
{
	for (;;) {
		struct task *t = NULL;
		u64 flags = spinlock_irqsave(&sleep_lock[this_cpu_id()]);

		if (!list_empty(&sleep_queue[this_cpu_id()])) {
			struct task *cand = sched_first_entry(&sleep_queue[this_cpu_id()], struct task, sleep_node);

			if (cand->wake_tick <= now) {
				list_del(&cand->sleep_node);
				list_init(&cand->sleep_node);
				cand->sleeping = false;
				t = cand;
			}
		}
		spinlock_unlock_irqrestore(&sleep_lock[this_cpu_id()], flags);

		if (!t)
			return;
		sched_wake(t);
	}
}

/*
 * The timer tick. Runs on the CPU whose LAPIC fired, with interrupts off
 * (the interrupt entry already did that) and with the run queue lock taken
 * only for the shortest possible window.
 */
void sched_tick(void)
{
	struct runqueue *rq = sched_runqueues[this_cpu_id()];
	struct task *t = current_task();
	u64 now = ++cpu_ticks[this_cpu_id()];
	bool need_switch = false;

	rq->clock = now;

	if (!t) {
		/* No current task: the CPU is between init and sched_start(). */
		sched_wake_sleepers(rq, now);
		return;
	}

	/*
	 * Charge the tick before deciding anything. The budget is the only
	 * mechanism that distinguishes a task that uses the CPU from one that
	 * blocks, so it has to be charged on every tick regardless of what the
	 * rest of the tick decides to do.
	 */
	t->total_ticks++;

	if (t->exec_budget)
		t->exec_budget--;

	if (t->policy == SCHED_FIFO || t->policy == SCHED_RR) {
		/* RT bandwidth: a task that has used its whole slice for the
		 * period is pushed back into the MLFQ at the top until the
		 * period rolls over. Without this, one RT task at priority 99
		 * can take the machine to zero useful work. */
		t->rt_consumed++;
		if (t->rt_consumed >= RT_BUDGET_TICKS)
			t->rt_throttled = true;
		if ((now - t->rt_period_start) >= RT_PERIOD_TICKS) {
			t->rt_period_start = now;
			t->rt_consumed = 0;
			t->rt_throttled = false;
		}
		if (t->rt_slice)
			t->rt_slice--;
		if ((t->policy == SCHED_RR && t->rt_slice == 0) ||
		    t->rt_throttled)
			need_switch = true;
	} else if (t->policy != SCHED_IDLE) {
		if (t->exec_budget == 0) {
			if (t->mlfq_level + 1 < MLFQ_LEVELS) {
				t->mlfq_level++;
				t->io_boost = 0;
			}
			/*
			 * Promotion to the top resets the budget; demotion
			 * recharges the new level's full quantum. Resetting on
			 * demotion as well would let a task time its slice to
			 * land just under the threshold and stay at level 0
			 * indefinitely, which is the gaming the accounting
			 * exists to stop.
			 */
			task_reset_budget(t);
			need_switch = true;
		} else if (t->policy == SCHED_IDLE) {
			/* The idle task gives up the CPU the moment anything
			 * becomes runnable. It has no work of its own to
			 * protect, so its only job is to notice. */
			u64 flags = spinlock_irqsave(&rq->lock);

			need_switch = rq_has_work_locked(rq);
			spinlock_unlock_irqrestore(&rq->lock, flags);
		} else {
			/* Strict priority within the MLFQ: if anything better is
			 * runnable, this task does not get the tick. */
			u64 flags = spinlock_irqsave(&rq->lock);

			need_switch = rq_has_higher_locked(rq, t);
			spinlock_unlock_irqrestore(&rq->lock, flags);
		}
	}

	sched_wake_sleepers(rq, now);

	if ((now % SCHED_AGING_INTERVAL) == 0)
		sched_aging_pass(rq, now);

	if (need_switch)
		schedule();
}

/* ------------------------------------------------------------- sleeping ----- */

int sched_sleep_ns(u64 ns)
{
	u64 ticks = (ns + 999999ULL) / 1000000ULL;
	u64 now = now_ticks();
	u64 flags = spinlock_irqsave(&sleep_lock[this_cpu_id()]);
	struct task *t = current_task();

	if (!t)
		return 0;

	if (ticks == 0)
		ticks = 1;

	/*
	 * Insertion keeps the list sorted by wake_tick so the expiry walk
	 * stops at the first sleeper that is not due, which makes waking O(due)
	 * rather than O(sleepers).
	 */
	struct list_head *pos = sleep_queue[this_cpu_id()].next;

	while (pos != &sleep_queue[this_cpu_id()]) {
		struct task *o = list_entry(pos, struct task, sleep_node);

		if (o->wake_tick > now + ticks)
			break;
		pos = pos->next;
	}
	__list_add(&t->sleep_node, pos->prev, pos);
	spinlock_unlock_irqrestore(&sleep_lock[this_cpu_id()], flags);

	t->sleeping = true;
	t->wake_tick = now + ticks;
	sched_block_current();
	return 0;
}

int sched_wait_for(struct task *t, u64 ns)
{
	if (!t)
		return -ESRCH;
	if (t->state == TASK_ZOMBIE || t->detached)
		return 0;
	if (ns)
		return sched_sleep_ns(ns);
	sched_block_current();
	return 0;
}

/* ------------------------------------------------------------- policies ----- */

int sched_set_policy(struct task *t, u32 policy, u32 rt_priority,
		     const struct sched_deadline_attr *dl)
{
	/*
	 * Capability check. SCHED_FIFO at priority 99 lets a holder freeze
	 * every other task on the machine, so it is a privileged operation
	 * even though POSIX lets an unprivileged process lower its own
	 * priority. Raising a task above the caller's own priority is what
	 * makes it privilege; lowering is not, and rejecting both would make
	 * the syscall useless for the cooperative use it was designed for.
	 */
	if (policy == SCHED_FIFO || policy == SCHED_RR ||
	    policy == SCHED_DEADLINE) {
		if (!(t->cred.caps & CAP_SYS_NICE))
			return -EPERM;
	}

	if (policy == SCHED_FIFO || policy == SCHED_RR) {
		if (rt_priority > RT_MAX_PRIORITY)
			return -EINVAL;
	}

	if (policy == SCHED_DEADLINE) {
		if (!dl || dl->period_ns == 0)
			return -EINVAL;
		/* runtime <= deadline <= period: the feasibility condition. */
		if (dl->runtime_ns > dl->deadline_ns ||
		    dl->deadline_ns > dl->period_ns)
			return -EINVAL;
	}

	struct runqueue *home = sched_runqueues[t->rq_cpu < MAX_CPUS ? t->rq_cpu : 0];

	if (!home)
		return -ENODEV;

	u64 flags = spinlock_irqsave(&home->lock);
	bool was_queued = t->on_rq;

	if (was_queued)
		rq_dequeue_locked(home, t);

	t->policy = policy;
	if (policy == SCHED_FIFO || policy == SCHED_RR)
		t->rt_priority = rt_priority;
	if (policy == SCHED_DEADLINE && dl) {
		t->rt_runtime = dl->runtime_ns;
		t->deadline_ns = dl->deadline_ns;
		t->period_ns = dl->period_ns;
		t->abs_deadline = now_ticks() * 1000000ULL + dl->deadline_ns;
		/* Admission control, evaluated against the target CPU. */
		u64 util = (dl->runtime_ns * 100ULL) / dl->period_ns;

		if (home->deadline_util + util > DEADLINE_MAX_UTIL_PCT) {
			spinlock_unlock_irqrestore(&home->lock, flags);
			return -EBUSY;
		}
		home->deadline_util += util;
	}
	if (policy == SCHED_NORMAL || policy == SCHED_BATCH) {
		/* nice maps onto the starting level: 0 and below start at the
		 * top, +19 starts at level 3, per the design. Demotion from
		 * there is unchanged, so a nice'd task still earns its way
		 * down. */
		s32 n = t->nice;

		t->mlfq_level = (u8)(n <= 0 ? 0 : (n > 3 ? 3 : n));
	}

	task_reset_budget(t);
	if (was_queued)
		rq_enqueue_locked(home, t, false);
	spinlock_unlock_irqrestore(&home->lock, flags);
	return 0;
}

/* ---------------------------------------------------------------- init ------ */

void sched_init(void)
{
	u32 cpu = this_cpu_id();
	struct runqueue *rq;

	rq = kmalloc_zeroed(sizeof(*rq));
	if (!rq)
		panic("cannot allocate a run queue");
	sched_runqueues[cpu] = rq;

	spinlock_init(&rq->lock);
	for (int i = 0; i < MLFQ_LEVELS; i++) {
		list_init(&rq->mlfq[i].head);
		rq->mlfq[i].count = 0;
	}
	for (int i = 0; i < RT_PRIORITIES; i++)
		list_init(&rq->rt.queue[i]);
	list_init(&rq->deadline);
	rq->mlfq_bitmap = 0;
	rq->deadline_count = 0;
	rq->nr_running = 0;
	rq->nr_switches = 0;
	rq->deadline_util = 0;

	list_init(&sleep_queue[cpu]);
	spinlock_init(&sleep_lock[cpu]);

	struct task *idle = idle_task_create();
	rq->idle = idle;
	per_cpu(idle_task) = idle;
	per_cpu(current) = idle;
	per_cpu(in_scheduler) = true;
	rq->current = idle;
	idle->state = TASK_RUNNING;

	/*
	 * Nothing is scheduled yet, but the syscall entry path needs a kernel
	 * stack to land on, and the idle task is what is "running", so point it
	 * at the idle task's own stack.
	 */
	syscall_set_kernel_stack((void *)((u64)idle->kernel_stack +
					  TASK_KERNEL_STACK_SIZE));

	SCHED_LOG(KLOG_INFO, "scheduler up on cpu %u, idle task pid 0", cpu);
}

__noreturn void sched_start(void)
{
	struct runqueue *rq = sched_runqueues[this_cpu_id()];

	cpu_started[this_cpu_id()] = true;

	/*
	 * Hand the CPU to the idle task's trampoline, on the idle task's own
	 * stack. From here on the boot stack is never a running context, so
	 * there is no frame anywhere that the scheduler could resume into
	 * code that no longer makes sense.
	 */
	per_cpu(current) = rq->idle;
	rq->current = rq->idle;
	rq->idle->state = TASK_RUNNING;
	syscall_set_kernel_stack((void *)((u64)rq->idle->kernel_stack +
					  TASK_KERNEL_STACK_SIZE));
	tss_set_kernel_stack((void *)((u64)rq->idle->kernel_stack +
				      TASK_KERNEL_STACK_SIZE));
	context_restore(rq->idle->context_rsp);
}
