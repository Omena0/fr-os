# Observability API

## Overview

The observability API provides syscalls and file interfaces for userspace tools to access kernel diagnostic data: tracepoint event streams, performance counter readings, and system statistics.

## Syscall Interface

| Syscall | Description |
|---|---|
| `sys_trace_open(uint32_t *tracepoint_ids, int n)` | Open a tracing session for the listed tracepoints. Returns a tracing fd. |
| `sys_trace_read(int fd, void *buf, size_t len)` | Read accumulated trace events (blocks if ring buffer is empty). |
| `sys_trace_close(int fd)` | End the tracing session. |
| `sys_perf_event_open(struct perf_event_attr *attr, pid_t pid, int cpu, int group_fd, uint64_t flags)` | Open a performance monitoring event. Returns a perf fd. |
| `sys_klog_read(struct klog_entry *buf, int max_entries, uint64_t *cursor)` | Read kernel log entries starting from `cursor`. Returns count read; updates `cursor`. |

## mmap-Based Ring Buffer Access

For high-throughput tracing, events are delivered via a memory-mapped ring buffer (no read syscall needed):

```c
// After sys_trace_open:
struct trace_mmap_header *hdr = mmap(NULL, TRACE_MMAP_SIZE,
    PROT_READ, MAP_SHARED, trace_fd, 0);
// hdr->data_head: producer index (updated by kernel)
// hdr->data_tail: consumer index (updated by userspace)
// Events follow the header at offset TRACE_MMAP_HEADER_SIZE

// Consumer loop:
while (1) {
    uint64_t head = atomic_load(&hdr->data_head);
    uint64_t tail = hdr->data_tail;
    if (head == tail) { /* spin or sleep */ continue; }
    struct trace_event_header *ev = (void *)((uint8_t *)hdr
        + TRACE_MMAP_HEADER_SIZE
        + (tail % TRACE_RING_SIZE));
    process_event(ev);
    hdr->data_tail = tail + ev->len;
}
```

## System Statistics Files

Beyond tracing, `/proc` provides polled statistics:

| File | Contents |
|---|---|
| `/proc/stat` | Per-CPU time in user/sys/idle/iowait (jiffies) |
| `/proc/meminfo` | Total/free/available/cached/swapped memory |
| `/proc/interrupts` | Per-CPU interrupt counts per IRQ |
| `/proc/<pid>/stat` | Per-process CPU time, memory, scheduler state |

These are text files formatted for human readability and tool parsing.

## Userspace Observability Tools

| Tool | Function |
|---|---|
| `top` | Live per-process CPU and memory usage (polls `/proc`) |
| `vmstat` | Memory, swap, and CPU summary |
| `iostat` | Disk I/O statistics |
| `netstat` | Network connections and interface statistics |
| `profild` | Sampling CPU profiler (uses PMU interrupts + tracing) |
| `tracecap` | Capture tracepoint events to a binary file for offline analysis |

## Design Notes

- The mmap ring buffer protocol is compatible with the Linux `perf_event` ABI to simplify porting of existing tools.
- All observability syscalls are subject to normal capability checks: `CAP_SYS_ADMIN` for kernel-wide tracing; per-process tracing requires `CAP_PTRACE` or process ownership.
- No "always-on" kernel overhead: counters and tracepoints are enabled only when a session is open.

## Related Documents

- [overview.md](overview.md)
- [kernel-tracing.md](kernel-tracing.md)
- [performance-counters.md](performance-counters.md)
- [profiling.md](profiling.md)
- [debug-filesystem.md](debug-filesystem.md)
