# Per-Core Run Queues

## Structure

Each CPU has exactly one `struct runqueue`, allocated in per-CPU memory at boot time. The run queue is the sole source of scheduling decisions for that CPU.

## Per-CPU Memory

Per-CPU variables are stored in a segment-register-addressed region. Each CPU uses its `GS` segment base register to point at its own `struct percpu` block:

```c
struct percpu {
    uint32_t         cpu_id;
    struct runqueue  rq;             // the CPU's run queue
    struct task      *current;       // currently running thread
    struct task      *idle;          // this CPU's idle thread
    struct slab_mag  *slab_mags[N_SLAB_SIZES]; // SLAB magazines
    uint64_t         tick_count;     // number of scheduler ticks
    uint64_t         last_balance_ns;// timestamp of last load balance
    // ... other per-CPU fields
};
```

Access: `this_cpu_read(field)` / `this_cpu_write(field, val)` — inline assembly using `GS`-relative addressing, no lock needed.

## Locking

The run queue spinlock (`rq.lock`) must be held to:

- Add or remove a thread from any queue in the run queue.
- Change the `nr_running` count.
- Inspect or modify `rq.current`.

The lock is a ticket spinlock to ensure FIFO ordering among multiple CPUs attempting to access the same run queue (e.g., during load balancing migration).

Preemption is disabled while holding the run queue lock (spinlocks always disable preemption).

## Thread State and Run Queue Membership

A thread is in exactly one of these states at any time:

| State | Run queue membership |
|---|---|
| `TASK_RUNNING` | On this CPU's run queue (or currently running) |
| `TASK_INTERRUPTIBLE` | Off all run queues; on a wait queue |
| `TASK_UNINTERRUPTIBLE` | Off all run queues; on a wait queue (not woken by signals) |
| `TASK_ZOMBIE` | Off all run queues; waiting for parent to `wait()` |
| `TASK_STOPPED` | Off all run queues; paused by SIGSTOP |

The current running thread (`rq.current`) is not on the run queue FIFO — it is running, not waiting to be scheduled.

## Idle Thread

Each CPU has a dedicated idle thread that runs when no other thread is runnable. The idle thread:

- Executes `hlt` to halt the CPU until the next interrupt (saves power).
- Wakes on any interrupt (timer tick, IPI, device interrupt).
- Is never migrated to another CPU.
- Is always at the lowest priority and is never placed on a MLFQ queue — it is a special fallback.

## Run Queue Statistics

For load balancing and observability:

```c
struct runqueue {
    uint64_t nr_running;       // threads currently on this queue (not idle)
    uint64_t nr_switches;      // total context switches since boot
    uint64_t load_avg;         // exponential moving average of nr_running
    uint64_t clock_ns;         // monotonic ns clock (updated each tick)
};
```

`load_avg` is computed using an exponential moving average with a 500 ms half-life, updated on each scheduler tick:

```
load_avg = load_avg * 0.9 + nr_running * 0.1
```

This smooths out transient spikes and gives the load balancer stable data.

## Related Documents

- [multicore-overview.md](multicore-overview.md)
- [load-balancing.md](load-balancing.md)
- [mlfq-priority-queues.md](mlfq-priority-queues.md)
- [work-stealing.md](work-stealing.md)
