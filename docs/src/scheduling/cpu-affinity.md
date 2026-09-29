# CPU Affinity

## Overview

CPU affinity allows a thread or process to be restricted to a specific subset of CPUs. The kernel respects affinity during scheduling decisions, load balancing, and wakeup placement.

## Affinity Mask

Affinity is represented as a bitmask: one bit per CPU. Bit N is set if the thread may run on CPU N.

```c
typedef struct {
    uint64_t bits[4];   // supports up to 256 CPUs
} cpumask_t;
```

On a system with 18 CPUs (as in the QEMU reference config), only bits 0–17 are valid.

## Default Affinity

By default, a new thread inherits its parent's affinity mask. The default for PID 1 (init) is all CPUs online at boot.

## Setting and Getting Affinity

### Syscall Interface

```c
// Set affinity for thread tid
int sched_setaffinity(pid_t tid, size_t cpusetsize, const cpu_set_t *mask);

// Get current affinity
int sched_getaffinity(pid_t tid, size_t cpusetsize, cpu_set_t *mask);
```

Requires:

- Setting one's own affinity: no special privilege needed (but cannot expand beyond the current mask).
- Setting another thread's affinity: requires same UID or `CAP_SYS_NICE`.

### Inheritance

`fork()` and `clone()` inherit the parent's affinity mask. `exec()` preserves the affinity mask across the exec boundary.

## Kernel Enforcement

Affinity is enforced at three points:

1. **Initial placement**: `fork()`/`clone()` selects an initial CPU from the affinity mask.
2. **Load balancing**: The load balancer never migrates a thread to a CPU not in its affinity mask.
3. **Wakeup**: When a sleeping thread wakes, it is placed on a CPU in its affinity mask (preferring the home CPU if it is in the mask).

## Use Cases

### Isolating Real-Time Threads

Pin a real-time thread to a dedicated CPU to eliminate scheduling jitter from other threads:

```c
cpu_set_t mask;
CPU_ZERO(&mask);
CPU_SET(17, &mask);   // CPU 17 exclusively for this RT thread
sched_setaffinity(rt_tid, sizeof(mask), &mask);
```

### NUMA-Aware Allocation

Pin threads that access specific memory regions to CPUs close to those NUMA nodes. Combined with NUMA-aware memory allocation (see [memory/numa-policies.md](../memory/numa-policies.md)), this minimizes remote memory access latency.

### Userspace Driver Isolation

Userspace drivers may be pinned to specific CPUs to prevent them from interfering with application threads.

## Affinity and Load Balancing Interaction

If a thread's affinity mask contains only one CPU, it is effectively pinned and the load balancer will never migrate it. If the affinity mask is a subset of all CPUs, the load balancer respects the mask but still balances within the allowed CPUs.

A thread pinned to a single overloaded CPU cannot benefit from load balancing. This is a trade-off: cache locality vs. load distribution. The operator must make this decision explicitly by setting the affinity mask.

## Related Documents

- [multicore-overview.md](multicore-overview.md)
- [load-balancing.md](load-balancing.md)
- [realtime-scheduler.md](realtime-scheduler.md)
- [syscalls/scheduling-syscalls.md](../syscalls/scheduling-syscalls.md)
