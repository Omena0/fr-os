/*
 * sched.h — the multi-level feedback queue scheduler.
 *
 * Structure, and why it is shaped this way:
 *
 *   - Every CPU owns a struct runqueue. There is no global "current": the
 *     running task lives in this_cpu()->current, which is a per-CPU cache line
 *     and therefore free of the coherence traffic a global would generate on
 *     every tick. Everything the scheduler mutates during a tick therefore
 *     touches only the local run queue's spinlock.
 *   - A single global queue exists solely for migration, so a wakeup on the
 *     wrong CPU never has to take a remote lock on the scheduling path.
 *   - Four classes, in strict priority order: SCHED_DEADLINE, then
 *     SCHED_FIFO/SCHED_RR, then the MLFQ, then SCHED_IDLE. The MLFQ is
 *     strictly preemptive across its own levels, so a task at level 0 never
 *     waits behind one at level 3.
 *
 * Parameters (docs/scheduling/mlfq*.md) live below as compile-time constants.
 * They are deliberately not runtime-tunable: a scheduler whose weights can be
 * changed underneath a running system is a scheduler whose behaviour cannot be
 * reproduced from a bug report.
 */
#ifndef SCHED_H
#define SCHED_H

#include <types.h>
#include <stdbool.h>
#include <io.h>
#include <list.h>
#include <spinlock.h>
#include <task.h>

/* ------------------------------------------------------------ constants ----- */

/* Number of feedback levels. Level 0 is the highest priority. */
#define MLFQ_LEVELS 8

/* Timer frequency. The MLFQ budgets below are expressed in ticks, and at
 * 1 kHz a tick is a millisecond, which is the unit every design document uses. */
#define SCHED_HZ 1000

/*
 * Per-level budget in ticks. Level N doubles the budget of level N-1: a task
 * that has already proved it is CPU-bound gets a longer uninterrupted run
 * before it is asked to re-enter the scheduler, because each re-entry costs
 * context switches for everyone else on the CPU.
 */
#define MLFQ_BASE_QUANTUM 5
#define MLFQ_QUANTUM_0    5
#define MLFQ_QUANTUM_1    10
#define MLFQ_QUANTUM_2    20
#define MLFQ_QUANTUM_3    40
#define MLFQ_QUANTUM_4    80
#define MLFQ_QUANTUM_5    120
#define MLFQ_QUANTUM_6    160
#define MLFQ_QUANTUM_7    200

/* Aging: a runnable task in level L that has waited longer than
 * L * MLFQ_AGE_TICKS is promoted one level. Level 0 is never aged because it
 * is already the top. At 1 kHz and 100 ticks per level, the worst case wait
 * for a task stuck at the bottom level is 700 ticks before it moves, and a
 * task that keeps getting promoted reaches the top within 8 ticks of that. */
#define MLFQ_AGE_TICKS 100

/* How often the aging pass walks the queues. Every 10 ticks is 10 ms, which is
 * an order of magnitude finer than the threshold it is checking, so the
 * promotion latency is bounded by the threshold rather than by the interval. */
#define SCHED_AGING_INTERVAL 10

/* Real-time priorities, matching the POSIX range. */
#define RT_PRIORITIES 100
#define RT_MAX_PRIORITY (RT_PRIORITIES - 1)

/* SCHED_RR slice inside one RT priority, in ticks. */
#define RT_RR_QUANTUM 10

/* Real-time bandwidth: RT tasks together get this fraction of each period. The
 * reserve is what keeps a runaway RT task from taking the machine to zero
 * useful work. */
#define RT_PERIOD_TICKS       1000
#define RT_BUDGET_TICKS       950

/* Admission control for SCHED_DEADLINE: refuse a new deadline task once the
 * summed utilisation on a CPU exceeds this, expressed in percent. */
#define DEADLINE_MAX_UTIL_PCT 95

/* Scheduling policies. Values match the POSIX/Linux numbers for the ones libc
 * can name, so a ported program that compares them agrees with the kernel. */
#define SCHED_NORMAL     0
#define SCHED_FIFO       1
#define SCHED_RR         2
#define SCHED_IDLE       5
#define SCHED_BATCH      3
#define SCHED_DEADLINE   6

/* ------------------------------------------------------------- queues ------- */

struct mlfq_queue {
    struct list_head head;
    u32 count;
};

/*
 * RT priority array. 100 priorities do not fit in one 64-bit word, so the
 * bitmap is two words and the highest set bit is found with a ctz on the high
 * half followed by a ctz on the low half. That is still O(1) and still one
 * branch, and it avoids a 128-bit shift on a CPU model that may not have it.
 */
struct rt_prio_array {
    u64 bitmap[2];
    struct list_head queue[RT_PRIORITIES];
};

struct runqueue {
    spinlock_t lock;
    struct mlfq_queue mlfq[MLFQ_LEVELS];
    u32 mlfq_bitmap;          /* one bit per non-empty level */
    struct rt_prio_array rt;
    struct list_head deadline; /* ordered by abs_deadline, earliest first */
    u32 deadline_count;
    struct task *current;
    struct task *idle;
    u64 nr_running;
    u64 nr_switches;
    u64 clock;                /* ticks on this CPU since boot */
    u64 deadline_util;        /* summed runtime/period, in percent */
};

/*
 * One run queue per CPU, allocated by that CPU in sched_init(). Indexed by
 * this_cpu_id(); NULL before sched_init() has run on that CPU.
 */
extern struct runqueue *sched_runqueues[MAX_CPUS];

/* ------------------------------------------------------------- time --------- */

u64 sched_now_ticks(void);
u64 sched_now_ns(void);

/* ------------------------------------------------------------- API ---------- */

/* Build the idle tasks and the per-CPU run queue state. */
void sched_init(void);

/* Called from the timer IRQ on every tick, on the CPU whose LAPIC fired. */
void sched_tick(void);

/* Voluntary yield: requeue at the tail of the current level, no demotion. */
void sched_yield(void);

/* Block the calling task until something wakes it. */
void sched_block_current(void);

/* Make a blocked task runnable again, with the I/O boost applied. */
void sched_wake(struct task *t);

/* Put a fresh or reparented task on a run queue. */
void sched_add(struct task *t);

/* Remove a task from whatever queue holds it. */
void sched_remove(struct task *t);

/* Pick the highest-priority runnable task and switch to it.
 *
 * Does not return in the usual sense: the calling task is suspended inside
 * this function's loop and, when it is next scheduled, resumes by jumping back
 * to the top of that loop rather than by returning to its own caller. Every
 * value the loop needs is recomputed at the top, so a second pass through it is
 * a plain re-pick and not a replay of the switch. See the resume contract above
 * schedule() in sched.c. */
void schedule(void);

/* Never returns: hands the CPU to the first task and stays in the scheduler. */
__noreturn void sched_start(void);

/* Retire the calling task and switch away from it. The task's saved frame is
 * abandoned, so control never returns to it. */
__noreturn void sched_stop_current(void);

/*
 * Move `t` to `cpu`'s run queue.
 *
 * The migration list is for *placement*, not for state. This places any task
 * that is on no run queue — blocked, stopped, TASK_NEW, or currently running
 * elsewhere — so it cannot be described as moving "a runnable task": a task
 * that is asleep and then has its affinity changed is exactly the case this
 * exists for, and it must not be made runnable by being moved.
 *
 * The runnable test lives where the list is consumed: global_pop() refuses a
 * task that is not TASK_RUNNABLE and leaves it on the migration list, so a
 * blocked task here becomes runnable on the next runnable call to sched_wake(),
 * not at migration time. sched_migrate() deliberately does not touch t->state.
 */
void sched_migrate(struct task *t, u32 cpu);

/* Restrict a task to `mask`; migrates it if it is no longer allowed to run
 * where it currently sits. */
int sched_set_affinity(struct task *t, const cpumask_t *mask);

/* First CPU in `mask` at or after `preferred`. */
u32 sched_select_cpu(const cpumask_t *mask, u32 preferred);

/* Parameters for SCHED_DEADLINE. runtime <= deadline <= period is the
 * feasibility condition for a periodic task; a request that violates it is
 * rejected rather than admitted, because admitting it would silently break the
 * EDF guarantee it was asked for. */
struct sched_deadline_attr {
    u64 runtime_ns;
    u64 deadline_ns;
    u64 period_ns;
};

/*
 * Change scheduling policy. Requires CAP_SYS_NICE in the task's credentials:
 * handing a task SCHED_FIFO at priority 99 is a denial-of-service primitive
 * against everything else on the machine, so the check is not optional.
 *
 * `dl` is only consulted for SCHED_DEADLINE and may be NULL otherwise.
 */
int sched_set_policy(struct task *t, u32 policy, u32 rt_priority,
             const struct sched_deadline_attr *dl);

/* Sleep for `ns`, using the scheduler's timer list. Returns early (0) if a
 * signal-equivalent wake happened; there are no signals yet, so it always
 * returns 0 after the full interval. */
int sched_sleep_ns(u64 ns);

/* Block until `t` is woken, sleeping at most `ns` (0 = forever). */
int sched_wait_for(struct task *t, u64 ns);

/* The CPU this task currently sits on, for /proc and getcpu(2). */
u32 sched_task_cpu(struct task *t);

/* Switch to a task that is being brought up for the first time. */
__noreturn void sched_switch_to_new(struct task *t);

/* The idle task for this CPU. */
struct task *sched_idle_task(void);

#endif /* SCHED_H */
