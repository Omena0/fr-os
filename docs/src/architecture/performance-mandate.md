# Performance Mandate

## Principle

Every subsystem must define three things explicitly:

1. **Fast path** — the optimized execution path for the common case. Must be documented, measured, and protected from regression.
2. **Degraded path** — the fallback behavior under contention, resource exhaustion, or partial failure. Must be bounded and safe.
3. **Concurrency strategy** — how the subsystem behaves under concurrent access. Lock granularity, lock-free structures, and scalability limits must be stated.

## System-Wide Targets

| Metric | Target |
|---|---|
| Syscall round-trip (simple, no I/O) | < 200 ns on KVM |
| Context switch latency | < 2 µs |
| Pipe throughput (same core) | > 2 GB/s |
| Page fault (anonymous, not swapped) | < 1 µs |
| Memory allocation (`malloc`, small object) | < 100 ns |
| Interrupt entry to handler | < 500 ns |
| Network packet RX to socket buffer | < 5 µs (virtio-net) |
| Scheduler overhead per tick | < 5 µs |

## Per-Subsystem Fast Path Summary

### Scheduler

- **Fast path**: MLFQ dequeue from per-core head — O(1), no global lock.
- **Degraded**: Load balancer engages when queue imbalance exceeds threshold — O(N cores), bounded.
- **Concurrency**: Per-core run queues with per-queue spinlock. Global queue only for migration.

### Memory Allocator

- **Fast path**: Per-CPU SLAB magazine — O(1), no lock.
- **Degraded**: Refill from global SLAB pool — lock held briefly. Buddy allocation if pool empty.
- **Concurrency**: Per-CPU magazine is lockless. SLAB pool uses fine-grained spinlock. Buddy allocator uses per-zone lock.

### Syscall Dispatch

- **Fast path**: SYSCALL instruction → direct function pointer dispatch — no table walk.
- **Degraded**: ABI compat translation layer — additional indirection, bounded overhead.
- **Concurrency**: Dispatch table is read-only after boot. No locks on the fast path.

### VFS / Filesystem

- **Fast path**: Dentry cache hit → inode cache hit → page cache hit — no disk I/O.
- **Degraded**: Cache miss → block device read → block until completion.
- **Concurrency**: RCU for dentry cache reads. Per-inode lock for writes.

### Pipes (IPC)

- **Fast path**: Ring-buffer write with space available — single CAS or spinlock, no copy if zero-copy path active.
- **Degraded**: Ring buffer full → writer blocks on futex.
- **Concurrency**: Ring-buffer head/tail with atomic operations. Single producer / single consumer: lock-free. Multiple producers: lightweight spinlock.

### Networking

- **Fast path**: Interrupt → DMA descriptor → socket buffer enqueue → `epoll` wakeup — minimal copying.
- **Degraded**: Backpressure from socket buffer full → packet drop with notification.
- **Concurrency**: Per-socket lock for receive path. Lock-free ring for NIC descriptor management.

### Interrupt Handling

- **Fast path**: IDT dispatch → per-CPU handler — no cross-CPU communication.
- **Degraded**: Spurious interrupt detection and masking — bounded overhead.
- **Concurrency**: Per-CPU interrupt stacks. No shared state on hot path.

## Anti-Patterns (Prohibited)

- Global kernel lock (no "big kernel lock" equivalent)
- Synchronous disk I/O in interrupt context
- Dynamic memory allocation in interrupt handlers
- Unbounded loops in scheduler hot path
- Polling loops in kernel threads without yield points

## Measurement

Performance-critical paths must be instrumented with the kernel's performance counter subsystem. Regression testing must include a benchmark suite that validates the targets listed above on the reference QEMU configuration (18 vCPUs, 4 GB RAM, KVM). See [debugging/performance-counters.md](../debugging/performance-counters.md).
