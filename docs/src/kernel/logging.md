# Kernel Logging

## Overview

The kernel logging system (`klog`) provides structured, level-filtered log output from kernel code. It supports multiple output sinks and is safe to call from most kernel contexts (not from NMI handlers or early boot before initialization).

## Log Levels

| Level | Value | Usage |
|---|---|---|
| `KLOG_DEBUG` | 0 | Verbose debug output, disabled in release builds |
| `KLOG_INFO` | 1 | Normal informational messages |
| `KLOG_WARN` | 2 | Unexpected but recoverable conditions |
| `KLOG_ERROR` | 3 | Errors that affect functionality |
| `KLOG_PANIC` | 4 | Used by the panic system; bypasses filters |

The minimum log level is configurable at boot time via the kernel command line (`loglevel=N`).

## API

```c
// Primary logging macro
#define klog(level, fmt, ...) \
    _klog(level, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

// Convenience macros
#define klog_debug(fmt, ...)  klog(KLOG_DEBUG, fmt, ##__VA_ARGS__)
#define klog_info(fmt, ...)   klog(KLOG_INFO,  fmt, ##__VA_ARGS__)
#define klog_warn(fmt, ...)   klog(KLOG_WARN,  fmt, ##__VA_ARGS__)
#define klog_error(fmt, ...)  klog(KLOG_ERROR, fmt, ##__VA_ARGS__)
```

## Log Record Format

Each log record consists of:

```
[<timestamp_ns>] [<cpu>] <level> <file>:<line>: <message>
```

Example:

```
[    0.123456] [0] INFO  mm/buddy.c:87: buddy allocator: 2048 MB free
[    0.234567] [0] INFO  fs/ext4.c:44: ext4: mounted root filesystem
[    0.345678] [2] WARN  sched/mlfq.c:201: CPU 2: run queue starvation detected
```

Timestamps are nanoseconds since boot (from the TSC, calibrated against HPET).

## Output Sinks

| Sink | When active | Notes |
|---|---|---|
| Serial (COM1) | Always (after `klog_serial_init`) | 115200 8N1, direct port I/O |
| Ring buffer | Always | In-memory circular buffer, exposed to userspace via `/dev/klog` |
| Framebuffer | After display init | Scrolling text console on screen |

### Ring Buffer

The in-kernel ring buffer is a lock-free single-producer multiple-consumer design:

- Size: 1 MB (configurable at build time).
- Producers: any kernel code calling `klog`.
- Consumers: the `/dev/klog` device (userspace log daemon reads from here).

When the buffer is full, the oldest records are overwritten (wrap-around). Consumers must track their read position.

## Kernel Log Buffer (`/dev/klog`)

A character device at `/dev/klog` exposes the ring buffer to userspace:

- `read()`: Returns available log records from the consumer's current position.
- `poll()`/`epoll`: Becomes readable when new records are available.
- A userspace log daemon (part of the init system) reads from `/dev/klog` and writes to `/var/log/kernel.log`.

## Rate Limiting

To prevent log flooding from buggy code:

```c
// Emit at most once per second for a given call site
klog_ratelimited(KLOG_WARN, "unexpected condition at %s", name);
```

Rate limiting is per call site using a per-site `rate_limit_t` struct with a token bucket.

## Early Boot Logging

Before `klog_init()` is called, the kernel uses a minimal serial output function (`early_printk`) that writes directly to COM1 without buffering or level filtering. Messages are prefixed with `[early]`.

## Related Documents

- [panic-system.md](panic-system.md)
- [early-boot.md](early-boot.md)
- [bootloader/serial-output.md](../bootloader/serial-output.md)
- [debugging/serial-diagnostics.md](../debugging/serial-diagnostics.md)
