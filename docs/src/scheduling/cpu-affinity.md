# CPU Affinity (sched_setaffinity / sched_getaffinity)

## Status

CPU affinity is **implemented** in the kernel data structures and syscalls, but only CPU 0 is online so it has no practical effect.

## Implementation

### Data Structures

```c
// In task.h
cpumask_t cpumask;    // 4 words = 256 bits for MAX_CPUS
u64 affinity_mask;    // cached single-CPU mask (1 << cpu) or 0 if unpinned
```

### Syscalls

- `sched_setaffinity(pid, cpusetsize, mask)` — sets the task's CPU mask.
- `sched_getaffinity(pid, cpusetsize, mask)` — reads the task's CPU mask.

Both require `CAP_SYS_NICE` for other processes; a process can always set its own affinity.

### Kernel API

```c
int sched_set_affinity(struct task *t, const cpumask_t *mask);
u32 sched_select_cpu(const cpumask_t *mask, u32 preferred);
void sched_migrate(struct task *t, u32 cpu);
```

- `sched_set_affinity()`: validates mask, copies to `t->cpumask`, computes `affinity_mask` optimization (single bit if pinned to one CPU), and migrates if the task is currently running on a disallowed CPU.
- `sched_select_cpu()`: picks the first allowed CPU at or after `preferred`.
- `sched_migrate()`: places the task on the global migration list for the target CPU.

### Migration on Affinity Change

If a runnable task's affinity is changed to exclude its current CPU:

1. Task is removed from current CPU's run queue (under that CPU's lock).
2. Task is placed on the global migration list.
3. Destination CPU adopts it via `sched_drain_global()` when it next runs.

Since only CPU 0 runs, step 3 never executes for other CPUs.

## Current Limitation

With only CPU 0 online, all tasks effectively have affinity `{0}`. The affinity mask is stored and checked but no migration to other CPUs can occur because they don't exist.

## Related Documents

- [per-core-runqueues.md](per-core-runqueues.md)
- [multicore-overview.md](multicore-overview.md)
- [syscalls/scheduling-syscalls.md](../syscalls/scheduling-syscalls.md)