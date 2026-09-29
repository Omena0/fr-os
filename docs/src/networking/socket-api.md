# Socket API

## Overview

The BSD socket API provides a uniform interface for network and local IPC communication. All socket types (`AF_INET`, `AF_UNIX`, `AF_PACKET`) are accessible through the same set of syscalls.

## Core Syscalls

### `socket(domain, type, protocol)`

Creates a new socket.

| Domain | Type | Protocol | Description |
|---|---|---|---|
| `AF_INET` | `SOCK_STREAM` | `IPPROTO_TCP` | IPv4 TCP |
| `AF_INET` | `SOCK_RAW` | `IPPROTO_ICMP` | Raw ICMP |
| `AF_UNIX` | `SOCK_STREAM` | 0 | Unix domain stream |
| `AF_UNIX` | `SOCK_DGRAM` | 0 | Unix domain datagram |
| `AF_PACKET` | `SOCK_RAW` | `htons(ETH_P_ALL)` | Raw Ethernet (requires `CAP_NET_RAW`) |

Returns a file descriptor.

### `bind(fd, addr, addrlen)`

Associates a socket with a local address:

- `AF_INET`: binds to `struct sockaddr_in { sin_family, sin_port, sin_addr }`.
- `AF_UNIX`: binds to `struct sockaddr_un { sun_family, sun_path[] }` — creates a socket inode in the filesystem.

Binding to port < 1024 requires `CAP_NET_BIND`.

### `listen(fd, backlog)` / `accept(fd, addr, addrlen)`

`listen`: transitions the socket to passive listening mode.
`accept`: dequeues the next completed connection from the accept queue. Blocks if empty. Returns a new fd for the connection.

`accept4(fd, addr, addrlen, flags)` is the preferred form, supporting `SOCK_NONBLOCK | SOCK_CLOEXEC` flags.

### `connect(fd, addr, addrlen)`

For TCP: initiates the three-way handshake. Blocks until the connection is established (or fails). Non-blocking connect: returns immediately with `EINPROGRESS`; completion is signaled via `POLLOUT` on the fd.

### `send(fd, buf, len, flags)` / `recv(fd, buf, len, flags)`

Stream-oriented data transfer. `send` copies data from user buffer into the socket send buffer (kernel). Returns immediately once data is in the kernel buffer (does not wait for ACK). `recv` copies available data from the socket receive buffer to user buffer.

`MSG_DONTWAIT` flag: non-blocking send/recv (returns `EAGAIN` if would block).
`MSG_WAITALL` flag on recv: blocks until exactly `len` bytes are received.

### `sendmsg(fd, msghdr, flags)` / `recvmsg(fd, msghdr, flags)`

Extended variants supporting:

- Scatter-gather I/O (`iov` array in `msghdr`).
- Ancillary data (Unix socket credential passing, fd passing via `SCM_RIGHTS`).
- Datagram destination address (for `SOCK_DGRAM`).

### `shutdown(fd, how)`

- `SHUT_RD`: stop receiving.
- `SHUT_WR`: send FIN (signal end of data).
- `SHUT_RDWR`: both.

### `setsockopt(fd, level, optname, val, len)`

Configure socket options:

| `optname` | Effect |
|---|---|
| `SO_REUSEADDR` | Allow binding to an address in TIME_WAIT |
| `SO_KEEPALIVE` | Enable TCP keepalive probes |
| `SO_SNDBUF` / `SO_RCVBUF` | Set send/receive buffer sizes |
| `TCP_NODELAY` | Disable Nagle's algorithm |
| `TCP_KEEPIDLE` | Idle time before first keepalive probe |
| `SO_LINGER` | Wait for data to be sent on close |

### `getpeername(fd, addr, addrlen)` / `getsockname(fd, addr, addrlen)`

Retrieve the remote/local address of a connected socket.

## Non-Blocking I/O

Set a socket to non-blocking mode:

```c
int flags = fcntl(fd, F_GETFL);
fcntl(fd, F_SETFL, flags | O_NONBLOCK);
```

Or at creation: `socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP)`.

## Related Documents

- [tcp-implementation.md](tcp-implementation.md)
- [socket-subsystem.md](socket-subsystem.md)
- [ipc/unix-sockets.md](../ipc/unix-sockets.md)
- [ipc/fd-abstraction.md](../ipc/fd-abstraction.md)
