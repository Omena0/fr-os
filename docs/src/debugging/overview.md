# Debugging and Observability Overview

## What This Section Covers

This section documents the facilities available for debugging and observing system behavior — both at the kernel level (kernel tracing, hardware counters, event logging) and at the userspace level (profiling, observability APIs, debug filesystems).

## Components

| Component | Description | Document |
|---|---|---|
| Kernel tracing | Lightweight tracepoints with ring-buffer output | [kernel-tracing.md](kernel-tracing.md) |
| Event logging | Structured subsystem events with priority levels | [event-logging.md](event-logging.md) |
| Hardware counters | PMU (Performance Monitoring Unit) via RDPMC | [performance-counters.md](performance-counters.md) |
| Profiling | Sampling-based CPU profiler | [profiling.md](profiling.md) |
| Debug filesystem | /proc and /debug pseudo-filesystems | [debug-filesystem.md](debug-filesystem.md) |
| Serial diagnostics | Structured output to serial port for early-boot debugging | [serial-diagnostics.md](serial-diagnostics.md) |
| Observability API | Syscall interface for userspace observability tools | [observability-api.md](observability-api.md) |

## Design Principles

1. **Zero overhead when not in use**: tracepoints are no-ops when no consumer is active. Hardware counters are not read unless explicitly requested.
2. **Non-intrusive**: debugging tools do not require stopping the system or rebooting. They can be attached and detached at runtime.
3. **Low-latency ring buffers**: kernel events are written to per-CPU ring buffers to avoid contention. Consumers drain asynchronously.
4. **Structured output**: events carry typed fields, not raw text strings, enabling programmatic analysis.

## Related Documents

- [kernel-tracing.md](kernel-tracing.md)
- [event-logging.md](event-logging.md)
- [performance-counters.md](performance-counters.md)
- [profiling.md](profiling.md)
- [debug-filesystem.md](debug-filesystem.md)
- [serial-diagnostics.md](serial-diagnostics.md)
- [observability-api.md](observability-api.md)
