# Work Stealing

## Overview

Work stealing is the mechanism by which idle CPUs proactively pull threads from the run queues of busy CPUs. It is an alternative to centralized load balancing — instead of a single balancer redistributing work, each idle CPU finds its own work.

## When Work Stealing Occurs

Work stealing is triggered when a CPU's run queue becomes empty and the idle thread is about to be scheduled:

```
schedule() called on CPU N:
  if rq[N].nr_running == 0:
    if work_steal(N) == false:
      run idle thread
```

Work stealing runs before entering the idle thread. If successful, the CPU immediately schedules the stolen thread instead of idling.

## Victim Selection

The work-stealing algorithm selects a victim CPU to steal from:

1. **Random probe**: Select a random CPU from the affinity-compatible set. Check its `nr_running` via a lockless read. If `nr_running > 1` (it has at least one thread to spare): proceed.
2. **Verification**: Lock the victim's run queue. Re-check `nr_running > 1` (double-check under lock).
3. **Steal**: Take the thread at the **lowest MLFQ level** from the victim's queue (steal background work, not interactive threads). Check affinity compatibility.
4. **Place**: Unlock victim queue. Place stolen thread on local queue. Run it.

If the random probe fails (victim has 0 or 1 threads), retry up to `STEAL_MAX_TRIES` times (default: 4) with different random victims before giving up and entering idle.

## Lockless Load Check

Before locking a victim's run queue, the stealing CPU checks `victim_rq->nr_running` without holding the lock (using `READ_ONCE`). This is a lockless read used only as a filter to avoid acquiring locks on empty queues. The actual decision is made under the lock.

## Thread Selection Strategy

The stealing CPU prefers to steal threads from the lowest MLFQ level (background threads):

- Stealing a low-priority thread minimizes impact on the victim CPU's interactivity.
- Low-MLFQ threads have less cached state benefit from staying on their original CPU.
- RT threads are never stolen (they are assumed to be intentionally placed).

## Affinity Check During Steal

Before committing to steal a thread, the stealing CPU verifies that the thread's CPU affinity mask includes the stealing CPU. If not, the thread is skipped and the stealer looks for another candidate on the same victim.

## Relationship to Periodic Load Balancing

Work stealing and periodic load balancing are complementary:

- **Work stealing**: Reactive, immediate, triggered by idleness.
- **Periodic load balancing**: Proactive, scheduled, handles sustained imbalance.

Work stealing handles transient idle periods (e.g., one CPU finishes its batch quickly while others still have work). Load balancing handles sustained skew (e.g., all new threads being spawned on one CPU).

## Performance

Fast path (successful steal):

- 1 lockless read (random probe)
- 1 lock acquire + list pop + lock release (victim)
- 1 lock acquire + list push + lock release (local)
- Total: ~200 ns on a cache-warm QEMU KVM system

Fallback (steal fails, idle):

- Up to `STEAL_MAX_TRIES` lockless reads
- Execute `hlt` — woken by next interrupt

## Related Documents

- [load-balancing.md](load-balancing.md)
- [per-core-runqueues.md](per-core-runqueues.md)
- [multicore-overview.md](multicore-overview.md)
- [cpu-affinity.md](cpu-affinity.md)
