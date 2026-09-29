# MLFQ Priority Queues

## Data Structure

Each CPU's run queue contains 8 per-level MLFQ queues plus one RT priority array. The structure:

```c
#define MLFQ_LEVELS 8
#define RT_PRIORITIES 100

struct mlfq_queue {
    struct list_head head;   // doubly linked list of struct task
    uint32_t count;          // number of threads at this level
};

struct rt_prio_array {
    uint64_t bitmap;          // bitmask of non-empty priority slots (O(1) find)
    struct list_head queue[RT_PRIORITIES];
};

struct runqueue {
    spinlock_t          lock;
    struct mlfq_queue   mlfq[MLFQ_LEVELS];
    struct rt_prio_array rt;
    struct task         *current;  // running thread
    struct task         *idle;     // idle thread (always runnable)
    uint64_t            nr_running;
    uint64_t            nr_switches;
    uint64_t            clock_ns;  // monotonic nanosecond clock for this CPU
};
```

One `struct runqueue` exists per CPU, stored in per-CPU memory (no cache line sharing between CPUs).

## Enqueue / Dequeue

### Enqueue (adding a thread to the run queue)

```c
void rq_enqueue(struct runqueue *rq, struct task *t) {
    if (t->sched_class == SCHED_RT) {
        // RT: add to priority queue at rt_priority slot
        list_add_tail(&t->rq_node, &rq->rt.queue[t->rt_priority]);
        bit_set(&rq->rt.bitmap, t->rt_priority);
    } else {
        // MLFQ: add to tail of the thread's current level queue
        list_add_tail(&t->rq_node, &rq->mlfq[t->mlfq_level].head);
        rq->mlfq[t->mlfq_level].count++;
    }
    rq->nr_running++;
}
```

### Dequeue (selecting the next thread to run)

```c
struct task *rq_dequeue_next(struct runqueue *rq) {
    // 1. Check RT class first
    if (rq->rt.bitmap != 0) {
        int prio = __builtin_ctzll(rq->rt.bitmap);  // find highest RT priority
        struct task *t = list_first_entry(&rq->rt.queue[prio], struct task, rq_node);
        list_del(&t->rq_node);
        if (list_empty(&rq->rt.queue[prio]))
            bit_clear(&rq->rt.bitmap, prio);
        rq->nr_running--;
        return t;
    }
    // 2. Find highest non-empty MLFQ level
    for (int level = 0; level < MLFQ_LEVELS; level++) {
        if (rq->mlfq[level].count > 0) {
            struct task *t = list_first_entry(&rq->mlfq[level].head, struct task, rq_node);
            list_del(&t->rq_node);
            rq->mlfq[level].count--;
            rq->nr_running--;
            return t;
        }
    }
    // 3. No runnable thread — return idle
    return rq->idle;
}
```

Complexity:

- RT dequeue: O(1) — single `ctzll` plus list pop.
- MLFQ dequeue: O(MLFQ_LEVELS) worst case = O(8) = effectively O(1).

## Per-CPU Isolation

Each CPU owns exactly one `struct runqueue`. There is no global run queue. Threads can only be on the run queue of one CPU at a time.

When a thread migrates between CPUs (via load balancing or affinity change), it is:

1. Removed from the source CPU's run queue (under source RQ lock).
2. Added to the destination CPU's run queue (under destination RQ lock).

Both locks are never held simultaneously — the two-phase "lock, grab, unlock, lock, place, unlock" protocol avoids deadlock.

## MLFQ Level Bitmask (Optimization)

A 64-bit bitmask tracks which MLFQ levels are non-empty (similar to the RT bitmap). This allows O(1) dequeue even for the MLFQ class:

```c
uint8_t mlfq_nonempty_bitmap;   // 1 bit per level, bit 0 = level 0
```

`__builtin_ctz(mlfq_nonempty_bitmap)` gives the highest non-empty level in a single instruction.

## Related Documents

- [mlfq.md](mlfq.md)
- [per-core-runqueues.md](per-core-runqueues.md)
- [load-balancing.md](load-balancing.md)
