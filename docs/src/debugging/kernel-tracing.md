# Kernel Tracing

## Overview

Kernel tracing provides lightweight instrumentation at key kernel events. When a consumer is active, events are recorded to a per-CPU ring buffer and can be read by userspace tools via the observability API.

## Tracepoints

A tracepoint is a static instrumentation site in the kernel:

```c
DEFINE_TRACEPOINT(sched_switch,
    TP_ARGS(struct thread *prev, struct thread *next),
    TP_FIELDS(
        uint32_t prev_tid;
        uint32_t next_tid;
        uint8_t  prev_state;
    ),
    TP_FAST_ASSIGN(
        __entry->prev_tid   = prev->tid;
        __entry->next_tid   = next->tid;
        __entry->prev_state = prev->state;
    )
);
```

At each instrumentation site, the tracepoint is called:
```c
TRACE(sched_switch, prev, next);
```

When no consumer is active, this expands to a single comparison (tracepoint enabled flag) + predicted-not-taken branch — effectively zero overhead.

When a consumer attaches, the flag is set and events are recorded.

## Per-CPU Ring Buffers

Each CPU has a dedicated ring buffer for tracepoint events:
```c
struct trace_ring_buffer {
    uint8_t   *buf;       // physically contiguous buffer
    uint32_t   size;      // power of 2
    uint32_t   write_pos; // producer position (always monotonically increasing)
    uint32_t   read_pos;  // consumer position (userspace-owned)
    uint32_t   dropped;   // events dropped due to buffer full
};
```

Writing is lock-free: the producer atomically increments `write_pos` and writes the event. The consumer reads events and advances `read_pos` via the observability API.

## Event Format

Each event in the ring buffer has a header followed by the event payload:

```c
struct trace_event_header {
    uint32_t type;       // tracepoint ID
    uint32_t len;        // total length including this header
    uint64_t timestamp;  // RDTSC timestamp
    uint32_t cpu;        // CPU that generated this event
};
```

## Available Tracepoints

| ID | Name | Fields |
|---|---|---|
| 1 | `sched_switch` | `prev_tid`, `next_tid`, `prev_state` |
| 2 | `sched_wakeup` | `tid`, `target_cpu` |
| 3 | `irq_entry` | `irq_num` |
| 4 | `irq_exit` | `irq_num` |
| 5 | `syscall_entry` | `syscall_num`, `arg0`, `arg1` |
| 6 | `syscall_exit` | `syscall_num`, `return_val` |
| 7 | `page_fault` | `addr`, `error_code` |
| 8 | `mm_alloc` | `addr`, `order` (buddy) |
| 9 | `mm_free` | `addr`, `order` |
| 10 | `vfs_open` | `inode`, `dentry_hash`, `flags` |
| 11 | `vfs_read` | `inode`, `offset`, `len` |
| 12 | `vfs_write` | `inode`, `offset`, `len` |
| 13 | `net_tx` | `skb_len`, `dev_id` |
| 14 | `net_rx` | `skb_len`, `dev_id` |

## Related Documents

- [overview.md](overview.md)
- [performance-counters.md](performance-counters.md)
- [observability-api.md](observability-api.md)
- [event-logging.md](event-logging.md)
