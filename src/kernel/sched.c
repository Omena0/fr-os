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

/*
 * Take a task off the migration list if it is on it.
 *
 * A task has one `rq_node` and therefore one place it can be linked, so
 * `on_rq` and `in_global` record which place that is and the invariant is that
 * at most one of them is set: a task is on a run queue, or on the migration
 * list, or on neither — never on both.
 *
 * The invariant is not free, and the reason is worth stating because it is not
 * obvious from either list operation. `rq_enqueue_locked()` re-initialises
 * `rq_node` before linking it, so enqueueing a task that is *still* linked in
 * `global_queue` does not splice it out of that list. `global_queue` is left
 * holding a pointer to a node whose prev/next now belong to a different list,
 * and both lists are corrupt from that instant: a later `global_pop()` walks
 * out of the migration list and into a run queue. Symmetrically, `global_push()`
 * linking a node a run queue already owns corrupts the run queue the same way.
 *
 * So both paths into a list go through here or through `global_push()`, and
 * each refuses a node that is already linked. That makes the invariant
 * structural: there is no sequence of calls that gives one task two owners.
 *
 * Called with the destination run queue's lock held, and it must be called
 * before `rq_enqueue_locked()` links anything. `global_lock` is therefore
 * always taken run-queue-then-global, which is the order `sched_set_affinity()`
 * already uses; no path in this file takes them the other way round.
 */
static void global_unlink(struct task *t)
{
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

/*
 * Is there a runnable task waiting on the migration list?
 *
 * A predicate rather than a count of runnable entries, on purpose.
 * global_count stays the exact length of the list, which list_add and list_del
 * already maintain without any help, so its accounting cannot drift. A second
 * counter meaning "how many of those could run" would have to be updated by
 * every push, pop and unlink, and getting one of them wrong is a silent
 * under- or over-count rather than a crash — the kind of bug that shows up as a
 * CPU that is mysteriously busy.
 *
 * The cost is an O(n) walk of the migration list, where n is the number of
 * tasks migrated and not yet picked up. It runs at most once per hlt, and only
 * when the list is non-empty.
 *
 * It has to exist at all because sched_migrate() pushes any task that is on no
 * run queue, including one that is blocked. The idle loop asks this question to
 * decide whether to call schedule(), and schedule() cannot find work for a
 * blocked task, so asking on the list's length alone would leave the idle task
 * calling schedule(), finding nothing, and looping — a spin on the CPU that can
 * never reach the hlt below it.
 */
static bool global_has_runnable(void)
{
	bool found = false;
	u64 flags = spinlock_irqsave(&global_lock);

	for (struct list_head *pos = global_queue.next; pos != &global_queue;
	     pos = pos->next) {
		if (list_entry(pos, struct task, rq_node)->state == TASK_RUNNABLE) {
			found = true;
			break;
		}
	}

	spinlock_unlock_irqrestore(&global_lock, flags);
	return found;
}

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
static bool rq_has_work_locked(const struct runqueue *rq, const struct task *ignore);

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
		/*
		 * Nothing is runnable on this CPU, so this task — which is not on
		 * any run queue itself, see sched_init() — can go to hlt.
		 *
		 * `ignore` is passed anyway. It is a backstop, not the mechanism:
		 * no code path enqueues the idle task, so `rq_has_work_locked()`
		 * can never see it. That is deliberate — see the note on
		 * rq_pick_locked() — and it is what makes "the picker returned
		 * NULL" mean exactly "no task is runnable" rather than "only the
		 * idle task is left". If a future change does put the idle task on
		 * a level, this test starts reporting work the queue does not have,
		 * and the loop below spins on the CPU instead of ever reaching the
		 * hlt. Better that than the reverse: a missed hlt costs a spin,
		 * a spurious one costs the idle loop's reason to exist.
		 */
		bool work = rq_has_work_locked(rq, rq->idle);

		spinlock_unlock_irqrestore(&rq->lock, flags);

		if (work || global_has_runnable()) {
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
		 *
		 * IF must be set across the hlt. `hlt` halts until an
		 * interrupt is *pending and unmasked*; with IF clear the CPU
		 * wakes for nothing and the halt is permanent, which is what a
		 * halted CPU with a live PIT underneath it must never be.
		 *
		 * Measured, and this is the whole reason the tree's timer had
		 * never ticked. With `hlt()` alone here, the CPU sat at
		 * RIP=0xffffffff8000bf76 with RFLAGS=0x46 -- IF clear -- while
		 * `info pic` reported `pic0: irr=03 imr=fc isr=00`: IRQ0 pending,
		 * IRQ0 unmasked, nothing in service. An interrupt that is
		 * pending, unmasked and never delivered is only ever explained
		 * by IF, and forcing IF on at that exact point under gdb made
		 * `pit_irq` fire immediately on vector 32 with no other change.
		 * Nothing in the tree had ever executed `sti`: `kmain` clears IF
		 * before gdt_reload and never sets it again, every syscall
		 * entry clears it, and irq_restore() only ever *enables*, so
		 * it cannot restore a state that was never set.
		 *
		 * The re-enable is scoped to the halt rather than done once in
		 * kmain because every path into this loop arrives with IF clear
		 * -- from sched_block_current() under a syscall, or from
		 * schedule() under a spinlock -- and a single global `sti`
		 * would be undone by the next irq_save(). What matters is that
		 * the CPU is interruptible while it is idle, which is the only
		 * place this kernel can afford an interrupt today.
		 *
		 * `sti` and `hlt` are one asm block, and that is not a style
		 * choice. Written as C the compiler is free to sink the `sti` past
		 * the `hlt`, because `hlt` stops *delivery* and does not itself
		 * change IF, so on GCC's model the two are reorderable. It also
		 * sees a second, conditional `sti` reachable through
		 * `irq_restore(flags)` and folds the two together.
		 *
		 * It did exactly that. The compiled idle loop is:
		 *
		 *     hlt
		 *     test $0x2,%ah        <- bit 1 of AH, a general register
		 *     je   skip
		 *     sti
		 *   skip:
		 *
		 * so the halt happens with IF still clear, and the `sti` that
		 * would have rescued it is both downstream of the halt and gated
		 * on a register that has nothing to do with the interrupt flag.
		 * The machine stops for ever with a pending, unmasked interrupt
		 * sitting on the PIC -- measured: RFLAGS=0x246 with HLT=1,
		 * `pic0: irr=12 isr=01`, IRQ0 and IRQ4 both pending, neither
		 * delivered, on every sample.
		 *
		 * One `asm volatile` block is opaque to the scheduler, so the two
		 * instructions retire in the order written. There is no way to
		 * express "halt with interrupts enabled" more weakly than this and
		 * still have it mean that.
		 */
		__asm__ volatile("sti; hlt" ::: "memory");

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
	/*
	 * Detach from the migration list before linking into this one. rq_node is
	 * a single link and cannot be in two lists, and linking it without
	 * detaching first leaves global_queue pointing at a node this queue now
	 * owns. See global_unlink().
	 */
	global_unlink(t);

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

/*
 * Unlink a task from whichever queue holds it.
 *
 * The unlink comes first, deliberately. The RT branch below asks whether its
 * priority list is now empty in order to clear that priority's bitmap bit, and
 * the answer it needs is the state of the list *after* the task has left it —
 * asking before the unlink answers a question about a list that still contains
 * the task, so the last task at a priority never clears its bit, the bitmap
 * keeps saying that priority has work, and the next pick computes a task
 * pointer out of a sentinel node instead of out of a task.
 */
static void rq_dequeue_locked(struct runqueue *rq, struct task *t)
{
	list_del(&t->rq_node);
	t->on_rq = false;

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

	if (rq->nr_running)
		rq->nr_running--;
}

/*
 * First task on `head`, or NULL if the list is empty.
 *
 * A caller that got here through a non-zero count or a set bitmap bit must be
 * prepared for NULL: that is what "the summary and the list disagree" looks
 * like, and it has to be repaired by falling through to the next class. The
 * alternative — treating the head node as if it were a task — hands
 * sched_first_entry() a sentinel to subtract an offset from and produces a
 * pointer into the middle of struct runqueue, which the switch path would then
 * write through as if it were a struct task.
 */
static struct task *rq_first_locked(const struct list_head *head)
{
	if (list_empty(head))
		return NULL;
	return list_entry(head->next, struct task, rq_node);
}

/*
 * Dequeue the highest-priority runnable task, or NULL if there is none.
 *
 * NULL means "no run queue anywhere has a runnable task", and schedule() turns
 * that into "run idle". It does *not* mean "only the idle task is left": the
 * idle task is never enqueued at all. sched_init() installs it as rq->idle and
 * as the initial per-CPU `current`, and from there it runs on its own stack via
 * context_restore() without ever passing through a run queue. Keeping it off
 * the queues is what lets a NULL here be unambiguous, and it is also what keeps
 * the pick from ever handing back the task that is already running.
 *
 * Every task returned here came off a queue, and a task only reaches a queue
 * with state TASK_RUNNABLE — including via the migration list, where global_pop()
 * refuses anything else. So the return value is runnable by construction and the
 * caller does not re-check it.
 */
static struct task *rq_pick_locked(struct runqueue *rq)
{
	struct task *t;

	if (rq->deadline_count) {
		t = rq_first_locked(&rq->deadline);
		if (t) {
			rq_dequeue_locked(rq, t);
			return t;
		}
		/* The list is the authority; the count is only a fast path. */
		rq->deadline_count = 0;
	}

	int prio = rt_bitmap_highest(&rq->rt);

	if (prio >= 0) {
		t = rq_first_locked(&rq->rt.queue[prio]);
		if (t) {
			rq_dequeue_locked(rq, t);
			return t;
		}
		rt_bitmap_clear(&rq->rt, prio);
	}

	while (rq->mlfq_bitmap) {
		u32 level = (u32)__builtin_ctz(rq->mlfq_bitmap);

		t = rq_first_locked(&rq->mlfq[level].head);
		if (t) {
			rq_dequeue_locked(rq, t);
			return t;
		}
		rq->mlfq[level].count = 0;
		rq->mlfq_bitmap &= ~(1u << level);
	}

	return NULL;
}

/*
 * Non-destructive "is there anything to run". The idle task needs the answer
 * without the side effect rq_pick_locked() has of removing what it found.
 *
 * `ignore` is the task asking, and it is skipped rather than counted: the only
 * caller that passes one is the idle loop, and a run queue whose only content
 * is the idle task is a run queue with no work on it.
 */
static bool rq_has_work_locked(const struct runqueue *rq, const struct task *ignore)
{
	struct task *t;
	int prio;

	if (rq->deadline_count) {
		t = rq_first_locked(&rq->deadline);
		if (t && t != ignore)
			return true;
	}

	prio = rt_bitmap_highest(&rq->rt);
	if (prio >= 0) {
		t = rq_first_locked(&rq->rt.queue[prio]);
		if (t && t != ignore)
			return true;
	}

	u32 bitmap = rq->mlfq_bitmap;

	while (bitmap) {
		u32 level = (u32)__builtin_ctz(bitmap);
		struct list_head *pos = rq->mlfq[level].head.next;

		bitmap &= ~(1u << level);
		while (pos != &rq->mlfq[level].head) {
			if (list_entry(pos, struct task, rq_node) != ignore)
				return true;
			pos = pos->next;
		}
	}

	return false;
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

/*
 * Hand a task to the migration list.
 *
 * Refuses a node that is already linked, for the reason spelled out at
 * global_unlink(): a second list_add over a node another list still points at
 * corrupts that list, and nothing here can repair it afterwards because the
 * damage is already in the neighbour's pointers.
 *
 * `on_rq` is rejected too, so a caller that forgets to dequeue first fails to
 * move the task rather than quietly giving it two owners. Every caller in this
 * file dequeues before pushing; the check is the backstop, not the mechanism.
 * The remaining case this actually rejects is a second migration request for a
 * task whose first one has not been picked up yet.
 */
static void global_push(struct task *t)
{
	u64 flags = spinlock_irqsave(&global_lock);

	if (t->in_global || t->on_rq) {
		spinlock_unlock_irqrestore(&global_lock, flags);
		return;
	}
	t->in_global = true;
	list_add_tail(&t->rq_node, &global_queue);
	global_count++;
	spinlock_unlock_irqrestore(&global_lock, flags);
}

/*
 * Claim one *runnable* task from the migration list, or NULL.
 *
 * Runnable is the whole content of that word here. sched_migrate() pushes every
 * task that is on no run queue, which includes one that is blocked, stopped,
 * still TASK_NEW, or currently running on another CPU: a migration request is
 * about placement, not about state. Adopting one of those here would put a
 * task the picker can return onto a run queue, and the switch to it would run a
 * task that is asleep, or one whose context frame was never built — with no
 * fault and no diagnostic, just a task executing when it has no business
 * executing. A non-runnable task therefore stays on the migration list, and
 * sched_wake() removes it from there when it becomes runnable.
 *
 * Returning NULL on a non-empty list is not a lost-wakeup risk. The list is
 * FIFO and this takes the head, so NULL means the head is not runnable; it
 * becomes runnable only via sched_wake(), and sched_wake() removes it from the
 * migration list itself rather than waiting for a drain.
 */
static struct task *global_pop(void)
{
	struct task *t = NULL;
	u64 flags = spinlock_irqsave(&global_lock);

	if (!list_empty(&global_queue)) {
		t = sched_first_entry(&global_queue, struct task, rq_node);

		if (t->state == TASK_RUNNABLE) {
			list_del(&t->rq_node);
			list_init(&t->rq_node);
			t->in_global = false;
			if (global_count)
				global_count--;
		} else {
			t = NULL;
		}
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
	/*
	 * Not a move, and not a copy: the task goes on the migration list and
	 * whichever CPU next calls sched_drain_global() adopts it onto its own
	 * run queue. That is the whole point of the list — this caller never
	 * takes the destination's lock.
	 *
	 * A second request for a task already on the list is refused by
	 * global_push() rather than linking the node twice.
	 *
	 * t->state is deliberately untouched. See the note on global_pop():
	 * whether the task may run is decided when the list is consumed.
	 */
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
	/* Off the migration list first, so the run queue is its only owner. */
	global_unlink(t);

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
 *
 * global_pop() only ever hands back a runnable task, so nothing admitted here
 * can be picked and switched to while it is asleep, still TASK_NEW, or running
 * on another CPU. It returns NULL on the first task it cannot take, which is
 * what bounds this loop: the list is FIFO, so the task left behind is the one
 * at the head, and it leaves the list through sched_wake() when it becomes
 * runnable rather than being skipped over.
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
 * Everything a switch has to do to `next` before control leaves this CPU,
 * except the switch itself. Returns the frame to resume `next` with.
 *
 * This is deliberately *not* the function that switches. It is the whole of the
 * switch bookkeeping, and schedule() calls it as an argument to
 * context_switch(), so that in schedule()'s instruction stream the switch is
 * followed by exactly one thing: the jump back to the top of the loop. See the
 * resume contract above schedule() for why that is the whole point.
 *
* FPU policy: save and restore on every switch, rather than tracking CR0.TS.
 * The lazy alternative costs a #NM on the first FPU instruction in every task
 * and needs a per-task "has this task ever used the FPU" bit that is wrong the
 * moment a task is preempted between the use and the save. 512 bytes on a switch
 * that already writes CR3 and the TSS RSP0 is not the bottleneck; being wrong
 * about register state is. The NULL guard below is only about a task that was
 * never given an fpu_state, not about whether it has used the FPU — a task that
 * has used it and a task that has not are treated identically.
 */
static u64 sched_switch_frame(struct task *prev, struct task *next)
{
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
	 * CLONE_VM thread switch costs no TLB flush at all.
	 *
	 * This runs *here*, before the call to context_switch() in schedule(),
	 * and that ordering is the resume contract rather than an optimisation.
	 * A suspended task's return address is the instruction after that call, so
	 * anything the switch itself does has to have already happened by the time
	 * the task comes back: CR3 here, the FPU below, both ahead of the frame
	 * hand-off. Doing the write after the call instead would re-run the
	 * outgoing pair's CR3 reload on the way back, installing the kernel PGD
	 * over a process's own address space, and then fpu_save() into the wrong
	 * task's save area.
	 *
	 * Verified in build/kernel.elf: `mov %rax,%cr3` at 0xffffffff8000b057,
	 * `call context_switch` at 0xffffffff8000b08a, and one `jmp` after it. */
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

	return next->context_rsp;
}

__noreturn void sched_switch_to_new(struct task *t)
{
	context_restore(t->context_rsp);
}

/*
 * Pick something to run, and switch to it. Returns when the calling task is
 * scheduled again — which, because the switch suspends it inside this loop,
 * means it returns by resuming here rather than by returning to its caller.
 *
 * The resume contract
 * -------------------
 * context_switch() publishes the caller's frame and `ret`s into the incoming
 * one. The frame it publishes ends at whatever address the `call` pushed, so a
 * suspended task's resume address is *exactly* the instruction after the
 * `call context_switch` below. There is no other candidate, and no source-level
 * way to move it: a resumption is a `ret` to a return address that was chosen
 * before the task was suspended. So the only question is what that instruction
 * has to be.
 *
 * It has to satisfy two properties, and both are structural rather than
 * incidental:
 *
 *   1. It must not depend on CR3, because the resumed task is running with the
 *      address space that *switch* installed for it, not the one that was live
 *      when it suspended. Any instruction that recomputes or reloads CR3 on the
 *      way back is a bug waiting for the first user process.
 *   2. It must be idempotent. Re-entering the loop must not replay the switch
 *      bookkeeping, or a resumed task runs the switch again, with the callee's
 *      arguments, for a pair of tasks that have already been switched.
 *
 * Both are satisfied by making the resume point the jump back to `reschedule:`
 * at the top of the loop, with every derived value recomputed there and nothing
 * between the jump and the top. Nothing is left after the call, so the compiler
 * has no dead code to sink past the switch point.
 *
 * Measured, not asserted. In build/kernel.elf:
 *
 *     ffffffff8000b057:  0f 22 d8          mov  %rax,%cr3      <- before the call
 *     ffffffff8000b08a:  e8 4d 52 00 00    call context_switch
 *     ffffffff8000b08f:  e9 4c fd ff ff    jmp  ffffffff8000ade0   <- the resume pad
 *                                                                           (= reschedule:)
 *
 * Five bytes, one instruction, a relative displacement and nothing else: no
 * memory operand, no call, no stack traffic, no CR3. Whichever address space
 * the switch installed, that `jmp` is valid, and the kernel half it lands in is
 * mapped in every PGD (mm.c copies PML4 entries 256 and 511 out of the kernel
 * page directory), so resuming a user task cannot fault on its own return.
 *
 * The bug this replaces, measured the same way from the pre-fix object. The
 * call used to be followed by __builtin_unreachable(), which told GCC the call
 * never returns and is a licence to sink anything below the switch. It took one.
 * With context_switch_to() inlined into schedule(), the object had:
 *
 *     bbf:  call  fpu_save
 *     bd0:  call  fpu_restore
 *     be3:  call  context_switch
 *     be8:  call  vmm_switch_to_kernel_pgd   <- the resume address, a call
 *     bed:  jmp   bb3                        <- back into the fpu_save block
 *
 * The resume address was therefore a *call*, not a jump: the `next->mm == NULL`
 * arm of the address-space switch had been sunk past the hand-off, and the
 * resume path jumped into the middle of the FPU sequence. So the first time any
 * task was resumed it re-ran the address-space switch for the pair it had just
 * left — installing the kernel PGD over a process's own — then fpu_save()'d into
 * the *other* task's save area, then called context_switch() again to switch
 * back to that same other task. Two runnable tasks ping-ponged forever, making
 * no forward progress and no diagnostic, with the address space of whichever
 * task had one destroyed on every lap.
 */
void schedule(void)
{
	struct runqueue *rq;
	struct task *prev;
	struct task *next;
	u64 flags;

reschedule:
	/*
	 * Recomputed on every pass, including the pass taken on resumption:
	 * the only input that is still authoritative at this point is the
	 * per-CPU state, because the caller-saved half of this frame belongs to
	 * whichever task happened to run last.
	 */
	rq = sched_runqueues[this_cpu_id()];
	prev = current_task();

	if (!rq || !prev)
		panic("schedule() before the scheduler was initialised");

	/* Anything the migration list is holding is ours to adopt. */
	sched_drain_global(rq);

	/*
	 * Pick first, requeue the outgoing task second.
	 *
	 * A running task is off the queue, so it can never be the one picked:
	 * the only way it comes back onto a queue is here, and the only way it
	 * stays off is if nothing else is runnable and it simply keeps running.
	 *
	 * On the resumption pass it is *not* off — the task was requeued on its
	 * way out and that is the state it was suspended in — so it is taken
	 * back off here, before the pick. Skipping this would leave the resumed
	 * task both running and queued, and the pick below could hand it back to
	 * itself.
	 */
	flags = spinlock_irqsave(&rq->lock);
	if (prev->on_rq && !prev->in_global && prev->rq_cpu == this_cpu_id())
		rq_dequeue_locked(rq, prev);
	next = rq_pick_locked(rq);
	spinlock_unlock_irqrestore(&rq->lock, flags);

	/* `next` cannot be `prev`: `prev` is off the queue by now, and
	 * rq_pick_locked() only ever returns linked tasks. */
	BUG_ON(next == prev);

	if (!next) {
		/* Nothing else is runnable anywhere: keep running. Requeueing
		 * the current task here rather than leaving it off the queue
		 * means a later wakeup finds it and does not have to know that
		 * it was already running. */
		if (prev->state == TASK_RUNNING && !prev->detached) {
			prev->state = TASK_RUNNABLE;
			prev->last_run = now_ticks();
			flags = spinlock_irqsave(&rq->lock);
			rq_enqueue_locked(rq, prev, false);
			spinlock_unlock_irqrestore(&rq->lock, flags);
		}
		return;
	}

	if (prev->state == TASK_RUNNING && !prev->detached) {
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

	/*
	 * The switch. This is the last instruction of the loop body other than
	 * the jump below, and the frame published by context_switch() has that
	 * jump's address as its return address — which is the entire resume
	 * contract.
	 *
	 * `sched_switch_frame()` is an argument, so all of the bookkeeping runs
	 * before the call. Nothing else may be placed after the call: the
	 * `goto` has no operands and no memory traffic, so it is valid whatever
	 * CR3 is, and the loop above it recomputes everything it needs.
	 */
	context_switch(&prev->context_rsp, sched_switch_frame(prev, next));
	goto reschedule;
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

			need_switch = rq_has_work_locked(rq, NULL);
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
