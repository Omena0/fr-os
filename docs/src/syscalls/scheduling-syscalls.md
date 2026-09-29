# Scheduling Syscalls

## Syscall Table: Scheduler Control (Numbers 180–194)

| Number | Name | Signature | Description |
|---|---|---|---|
| 180 | `sys_sched_yield` | `int sys_sched_yield(void)` | Voluntarily give up the CPU |
| 181 | `sys_sched_setscheduler` | `int sys_sched_setscheduler(pid_t pid, int policy, const struct sched_param *param)` | Set scheduling policy and priority |
| 182 | `sys_sched_getscheduler` | `int sys_sched_getscheduler(pid_t pid)` | Get scheduling policy |
| 183 | `sys_sched_setparam` | `int sys_sched_setparam(pid_t pid, const struct sched_param *param)` | Set RT priority |
| 184 | `sys_sched_getparam` | `int sys_sched_getparam(pid_t pid, struct sched_param *param)` | Get RT priority |
| 185 | `sys_sched_setattr` | `int sys_sched_setattr(pid_t pid, struct sched_attr *attr, uint32_t flags)` | Set full scheduling attributes (including deadline) |
| 186 | `sys_sched_getattr` | `int sys_sched_getattr(pid_t pid, struct sched_attr *attr, uint32_t size, uint32_t flags)` | Get full scheduling attributes |
| 187 | `sys_sched_setaffinity` | `int sys_sched_setaffinity(pid_t pid, size_t cpusetsize, const cpumask_t *mask)` | Set CPU affinity mask |
| 188 | `sys_sched_getaffinity` | `int sys_sched_getaffinity(pid_t pid, size_t cpusetsize, cpumask_t *mask)` | Get CPU affinity mask |
| 189 | `sys_sched_get_priority_max` | `int sys_sched_get_priority_max(int policy)` | Max priority for policy |
| 190 | `sys_sched_get_priority_min` | `int sys_sched_get_priority_min(int policy)` | Min priority for policy |
| 191 | `sys_nice` | `int sys_nice(int inc)` | Adjust MLFQ nice value (-20 to +19) |
| 192 | `sys_getpriority` | `int sys_getpriority(int which, id_t who)` | Get nice value |
| 193 | `sys_setpriority` | `int sys_setpriority(int which, id_t who, int prio)` | Set nice value |
| 194 | `sys_getcpu` | `int sys_getcpu(uint32_t *cpu, uint32_t *node)` | Get current CPU and NUMA node |

## Scheduling Policies

| Policy | Description | Priority range |
|---|---|---|
| `SCHED_NORMAL` | MLFQ (default) | nice -20 to +19 |
| `SCHED_FIFO` | Real-time FIFO | 1–99 |
| `SCHED_RR` | Real-time round-robin | 1–99 |
| `SCHED_DEADLINE` | EDF (earliest deadline first) | N/A (uses runtime/deadline/period) |
| `SCHED_IDLE` | Runs only when no other thread is ready | N/A |
| `SCHED_BATCH` | Like SCHED_NORMAL but no preemption bias | nice -20 to +19 |

## `struct sched_attr`

```c
struct sched_attr {
    uint32_t size;             // size of this struct (for versioning)
    uint32_t sched_policy;     // SCHED_* constant
    uint64_t sched_flags;
    int32_t  sched_nice;       // for SCHED_NORMAL/SCHED_BATCH
    uint32_t sched_priority;   // for SCHED_FIFO/SCHED_RR
    // For SCHED_DEADLINE:
    uint64_t sched_runtime_ns;  // execution budget per period
    uint64_t sched_deadline_ns; // relative deadline
    uint64_t sched_period_ns;   // period
};
```

## Permission Requirements

| Operation | Permission |
|---|---|
| Set `SCHED_FIFO` or `SCHED_RR` | `CAP_SYS_NICE` |
| Set `SCHED_DEADLINE` | `CAP_SYS_NICE` |
| Set `nice` to a more negative value | `CAP_SYS_NICE` if lowering below current value for another process |
| `sched_setaffinity` for another process | `CAP_SYS_NICE` |

A process can always lower its own priority (higher nice value) without capabilities.

## `sys_sched_yield` Implementation

1. Remove the current thread from the CPU's run queue.
2. Re-add it to the tail of its current MLFQ level queue (not demoted — voluntary yield does not incur priority penalty).
3. Call `schedule()` to select the next thread.

## `sys_getcpu`

Returns the CPU ID and NUMA node of the CPU the calling thread is currently running on. Used by lock-free data structures to access per-CPU data without a kernel call in the fast path (via VDSO — but the VDSO version is advisory and may be stale by one scheduling event).

## Related Documents

- [overview.md](overview.md)
- [scheduling/mlfq.md](../scheduling/mlfq.md)
- [scheduling/realtime-scheduler.md](../scheduling/realtime-scheduler.md)
- [scheduling/deadline-scheduling.md](../scheduling/deadline-scheduling.md)
- [scheduling/cpu-affinity.md](../scheduling/cpu-affinity.md)
