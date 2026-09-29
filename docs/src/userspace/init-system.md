# Init System

## Overview

The init system is PID 1. It is the first userspace process started by the kernel after booting. The kernel passes control to PID 1 after mounting the root filesystem. All other processes are descendants of PID 1.

## Responsibilities

1. **Mount filesystems**: `/proc`, `/sys`, `/dev`, `/tmp`.
2. **Bring up system services**: in dependency order.
3. **Maintain services**: restart crashed services, log failures.
4. **Handle orphaned children**: when a process's parent exits, the child is reparented to PID 1. PID 1 must `wait()` for them to prevent zombies.
5. **Manage runlevels / targets**: switch between boot, multi-user, shutdown states.
6. **Shutdown**: signal all services to stop, unmount filesystems, power off.

## Service Unit Format

Services are described in unit files (e.g., `/etc/init/httpd.unit`):

```ini
[Unit]
Name = httpd
Description = HTTP Server
After = network.target

[Service]
ExecStart = /usr/sbin/httpd -f /etc/httpd/httpd.conf
ExecStop  = /usr/sbin/httpd -k graceful-stop
Restart   = on-failure
RestartSec = 5s
User = http
Group = http

[Install]
WantedBy = multi-user.target
```

## Dependency Graph

Services declare `After = ` and `Requires = ` dependencies. The init system builds a dependency DAG and starts services in topological order, parallelizing services with no ordering constraint between them.

```
multi-user.target
  └─ network.target
       └─ dhcpd.service
            └─ udev.service
  └─ httpd.service
       └─ network.target
  └─ sshd.service
       └─ network.target
```

Services at the same level (no mutual dependency) are started in parallel.

## Service State Machine

```
┌─────────┐     start()     ┌──────────┐     process exits    ┌──────────┐
│ stopped │────────────────▶│ starting │────────────────────▶│  failed  │
└─────────┘                 └──────────┘                      └──────────┘
                                  │ process starts                 │
                                  ▼                                │ (restart)
                            ┌──────────┐     stop()          ┌──────────┐
                            │ running  │────────────────────▶│ stopping │
                            └──────────┘                      └──────────┘
```

## PID 1 Main Loop

```c
while (1) {
    // 1. Wait for events (non-blocking waitpid + epoll for unit file inotify):
    pid = waitpid(-1, &wstatus, WNOHANG);
    if (pid > 0) handle_child_exit(pid, wstatus);

    // 2. Poll for service control messages (Unix socket):
    epoll_wait(epfd, events, MAX_EVENTS, timeout_ms);
    for (each event) handle_control_message(event);

    // 3. Check restart timers for crashed services:
    check_restart_timers();
}
```

## Shutdown Sequence

1. Send `SIGTERM` to all running services (in reverse start order).
2. Wait up to 5 seconds per service for graceful shutdown.
3. Send `SIGKILL` to any remaining processes.
4. `waitpid` all children.
5. Unmount filesystems (in reverse mount order).
6. Call `sys_reboot(LINUX_REBOOT_CMD_POWER_OFF)`.

## Related Documents

- [overview.md](overview.md)
- [process-model.md](process-model.md)
- [syscalls/process-syscalls.md](../syscalls/process-syscalls.md)
- [ipc/signals.md](../ipc/signals.md)
