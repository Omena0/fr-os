# Debug Filesystem

## Overview

The OS exposes kernel state to userspace through two pseudo-filesystems:
- `/proc`: process and system information (POSIX-inspired).
- `/debug`: raw kernel debugging interfaces (not for general applications).

Neither filesystem stores data on disk. Their files are generated on-demand when read.

## `/proc` Layout

```
/proc/
  self/           → symlink to the current process's PID directory
  <pid>/
    cmdline       — command line arguments (NUL-separated)
    status        — process status (state, memory usage, credentials)
    maps          — virtual memory map (one VMA per line)
    fd/           — one symlink per open file descriptor
    stat          — single-line process statistics (scheduling)
    mem           — process virtual memory (readable by ptrace only)
    environ       — environment variables (NUL-separated)
    threads/      — directory listing per-thread info
  cpuinfo         — per-CPU information (model, frequency, flags)
  meminfo         — system memory statistics
  uptime          — uptime in seconds
  version         — kernel version string
  filesystems     — registered filesystem types
  mounts          — currently mounted filesystems
  interrupts      — interrupt counts per CPU
  stat            — system-wide CPU statistics (idle, user, sys, iowait)
  kallsyms        — kernel symbol table (address → name, requires CAP_SYS_ADMIN)
  modules         — loaded kernel modules
```

## `/proc/<pid>/maps`

Each line describes one VMA:
```
start-end  perms  offset  dev  inode  pathname
7f8a000000-7f8a100000  r-xp  0000000000  08:01  12345  /lib/libc.so.1
7f8a100000-7f8a101000  rw-p  00100000    08:01  12345  /lib/libc.so.1
7fff800000-7fffc00000  rwxp  0000000000  00:00  0      [stack]
ffffffffff600000-ffffffffff601000  r-xp  0000000000  00:00  0      [vdso]
```

## `/debug` Layout

```
/debug/
  tracing/
    enable            — write 1/0 to enable/disable all tracepoints
    tracepoints/      — one directory per tracepoint
      sched_switch/
        enable        — enable this specific tracepoint
        format        — event format description
    trace_pipe        — blocking read to stream events from ring buffers
    trace             — snapshot of current ring buffer contents
    per_cpu/
      cpu0/           — ring buffer for CPU 0
      cpu1/
  pmu/
    cpu0/
      fixed0          — fixed counter 0 (instructions retired)
      fixed1          — fixed counter 1 (core cycles)
      prog0           — programmable counter 0
  klog              — kernel event log (structured messages)
  panic_log         — last panic backtrace (if any)
  slab_info         — SLAB allocator statistics per cache
  buddy_info        — buddy allocator free list sizes
```

## Mounting

Both are mounted during early boot by the init system:
```sh
mount -t proc  proc  /proc
mount -t debug debug /debug
```

## Security

- Most `/proc/<pid>/` entries: readable by the owner or root. `mem` requires `ptrace` permission.
- `/proc/kallsyms`: kernel symbols are readable only by processes with `CAP_SYS_ADMIN`.
- `/debug/`: requires `CAP_SYS_ADMIN` for write access to `enable` files.
- `/debug/panic_log`: world-readable (no secret data, just backtrace).

## Related Documents

- [overview.md](overview.md)
- [kernel-tracing.md](kernel-tracing.md)
- [observability-api.md](observability-api.md)
- [security/capabilities.md](../security/capabilities.md)
