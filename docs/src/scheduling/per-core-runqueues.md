# Per-CPU Run Queues

## Data Structures

```c
struct runqueue {
    spinlock_t lock;
    struct mlfq_queue mlfq[MLFQ_LEVELS];
    u32 mlfq_bitmap;           // one bit per non-empty MLFQ level
    struct rt_prio_array rt;   // RT priority array with 100 priorities
    struct list_head deadline; // EDF-ordered deadline tasks
    u32 deadline_count;
    struct task *current;      // currently running task on this CPU
    struct task *idle;         // idle task for this CPU
    u64 nr_running;
    u64 nr_switches;
    u64 clock;                 // ticks on this CPU since boot
    u32 deadline_util;         // summed deadline utilization (percent)
};
```

- One `struct runqueue` per CPU, allocated in `sched_init()` on each CPU.
- Indexed by `this_cpu_id()` via `sched_runqueues[MAX_CPUS]`.
- All scheduling operations (enqueue, dequeue, pick) take the local run queue's spinlock.

## Enqueue / Dequeue

See [mlfq-priority-queues.md](mlfq-priority-queues.md) for MLFQ and [realtime-scheduler.md](realtime-scheduler.md) for RT queue operations.

## Cross-CPU Scheduling

Cross-CPU task placement uses a **global migration list** (`global_queue`):

```c
void sched_migrate(struct task *t, u32 cpu)
{
    t->rq_cpu = cpu;
    if (!t->on_rq)
        global_push(t);  // places on global_queue
}
```

The destination CPU calls `sched_drain_global()` to adopt tasks from the global list onto its local run queue.

## Affinity

Tasks have a `cpumask_t cpumask` field. `sched_set_affinity()` updates the mask and migrates the task if it's no longer allowed on its current CPU.

## Related Documents

- [overview.md](overview.md)
- [mlfq-priority-queues.md](mlfq-priority-queues.md)
- [multicore-overview.md](multicore-overview.md)
- [load-balancing.md](load-balancing.md)
- [cpu-affinity.md](cpu-affinity.md)