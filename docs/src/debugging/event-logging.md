# Event Logging

## Overview

The kernel maintains a structured event log — distinct from the tracepoint ring buffers. The event log is for higher-level diagnostic messages: driver initialization, subsystem errors, security events. It is the kernel equivalent of syslog.

## Log Levels

| Level | Value | Meaning |
|---|---|---|
| `LOG_EMERG` | 0 | System is unusable (kernel panic imminent) |
| `LOG_ALERT` | 1 | Action must be taken immediately |
| `LOG_CRIT` | 2 | Critical condition (driver failure, OOM) |
| `LOG_ERR` | 3 | Error condition |
| `LOG_WARN` | 4 | Warning (deprecated API, degraded mode) |
| `LOG_NOTICE` | 5 | Normal but significant event |
| `LOG_INFO` | 6 | Informational (device probed, service started) |
| `LOG_DEBUG` | 7 | Debug-level information |

## Kernel Logging API

```c
// In-kernel API:
klog(LOG_INFO, "net", "virtio-net: device %d MAC %02x:%02x:%02x:%02x:%02x:%02x",
     dev->id, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
```

The `klog` macro captures:
- Timestamp (`RDTSC` → converted to wall clock).
- CPU number.
- Subsystem tag (up to 16 bytes): `"sched"`, `"mm"`, `"net"`, `"fs"`, `"drv"`, `"sec"`, etc.
- Log level.
- The formatted message (up to 256 bytes).

## Internal Ring Buffer

Log entries are stored in a kernel-managed circular buffer:

```c
#define KLOG_BUF_SIZE (1 << 20) // 1 MB

struct klog_entry {
    uint64_t timestamp_ns;
    uint32_t cpu;
    uint8_t  level;
    char     subsystem[16];
    char     message[256];
};

static struct klog_entry klog_buf[KLOG_BUF_SIZE / sizeof(struct klog_entry)];
static uint32_t klog_head, klog_tail;
static spinlock_t klog_lock;
```

Writing: protected by `klog_lock` (only held for the duration of the ring buffer write).

## Reading from Userspace

The log is accessible via:
1. `/debug/klog`: a character device that provides a sequential read of all logged messages, newline-separated, in the format: `[timestamp_ns] [cpu] [level] [subsystem]: message`.
2. A dedicated `sys_klog_read` syscall for streaming reads (non-blocking, returns up to N entries).

## Serial Echo

Log entries at level `LOG_WARN` and above are also echoed to the serial port (COM1) for early-boot and crash diagnostics. This is unconditional and not configurable at runtime (it persists through kernel panics).

## Log Format

```
[  12345678.001] [CPU 3] [INFO ] [net ]: virtio-net: device 0 MAC 52:54:00:12:34:56
[  12345678.002] [CPU 0] [WARN ] [mm  ]: page reclaim: 89% of RAM in use
[  12345678.100] [CPU 1] [ERR  ] [drv ]: nvme: timeout waiting for completion queue
```

## Related Documents

- [overview.md](overview.md)
- [kernel-tracing.md](kernel-tracing.md)
- [serial-diagnostics.md](serial-diagnostics.md)
- [kernel/logging.md](../kernel/logging.md)
