# Real-Time Scheduler (SCHED_FIFO / SCHED_RR)

## Status

The real-time scheduler class is **implemented**.

## Design

Real-time threads are scheduled above all MLFQ threads. Within the RT class:

- **Strict priority**: higher `rt_priority` (0–99) always runs first.
- **SCHED_FIFO**: runs until it blocks, yields, or is preempted by a higher-priority RT task.
- **SCHED_RR**: same as FIFO but with a time slice (`RT_RR_QUANTUM` = 10 ticks); when the slice expires, the thread rotates to the tail of its priority queue.
- **RT bandwidth reserve**: RT tasks together get at most `RT_BUDGET_TICKS` (950) out of `RT_PERIOD_TICKS` (1000) = 95% of CPU time. A task that exhausts its budget is throttled until the next period.

## Data Structures

Per-CPU run queue contains an RT priority array:

```c
struct rt_prio_array {
    u64 bitmap[2];              // two words cover 100 priorities
    struct list_head queue[RT_PRIORITIES];
};
```

- `bitmap` tracks non-empty priority slots; `rt_bitmap_highest()` finds the highest-priority runnable task in O(1) via `__builtin_ctzll`.
- Each priority has its own FIFO queue.

## Enqueue / Dequeue

```c
// Enqueue (rq_enqueue_locked)
list_add_tail(&t->rq_node, &rq->rt.queue[t->rt_priority]);
rt_bitmap_set(&rq->rt, t->rt_priority);

// Dequeue (rq_dequeue_locked)
list_del(&t->rq_node);
if (list_empty(&rq->rt.queue[prio]))
    rt_bitmap_clear(&rq->rt, prio);
```

## Preemption

A newly woken RT task preempts the current task if its priority is higher. This is checked in `sched_add()` and `sched_wake()` via `rq_has_higher_locked()`.

## Bandwidth Throttling

On each timer tick (`sched_tick()`), the running RT task's `rt_slice` (for RR) or budget (for bandwidth) is decremented. When exhausted:

- SCHED_RR: thread moved to tail of its priority queue.
- Bandwidth: `rt_throttled` set; task removed from run queue until next period.

## Related Documents

- [overview.md](overview.md)
- [mlfq.md](mlfq.md)
- [deadline-scheduling.md](deadline-scheduling.md)
- [multicore-overview.md](multicore-overview.md)