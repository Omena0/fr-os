# Socket Syscalls

## Syscall Table: Networking (Numbers 160–170)

| Number | Name | Signature | Description |
|---|---|---|---|
| 160 | `sys_socket` | `int sys_socket(int domain, int type, int protocol)` | Create a new socket |
| 161 | `sys_bind` | `int sys_bind(int fd, const struct sockaddr *addr, socklen_t addrlen)` | Bind socket to address |
| 162 | `sys_listen` | `int sys_listen(int fd, int backlog)` | Listen for incoming connections |
| 163 | `sys_accept` | `int sys_accept(int fd, struct sockaddr *addr, socklen_t *addrlen)` | Accept a connection |
| 164 | `sys_accept4` | `int sys_accept4(int fd, struct sockaddr *addr, socklen_t *addrlen, int flags)` | Accept with flags (`SOCK_NONBLOCK`, `SOCK_CLOEXEC`) |
| 165 | `sys_connect` | `int sys_connect(int fd, const struct sockaddr *addr, socklen_t addrlen)` | Connect to a remote address |
| 166 | `sys_send` | `ssize_t sys_send(int fd, const void *buf, size_t len, int flags)` | Send data on connected socket |
| 167 | `sys_recv` | `ssize_t sys_recv(int fd, void *buf, size_t len, int flags)` | Receive data |
| 168 | `sys_sendto` | `ssize_t sys_sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *dest, socklen_t destlen)` | Send to specific address (for `SOCK_DGRAM` / raw) |
| 169 | `sys_recvfrom` | `ssize_t sys_recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *src, socklen_t *srclen)` | Receive with source address |
| 170 | `sys_setsockopt` | `int sys_setsockopt(int fd, int level, int optname, const void *val, socklen_t len)` | Set socket option |
| 171 | `sys_getsockopt` | `int sys_getsockopt(int fd, int level, int optname, void *val, socklen_t *len)` | Get socket option |
| 172 | `sys_sendmsg` | `ssize_t sys_sendmsg(int fd, const struct msghdr *msg, int flags)` | Send with ancillary data / scatter-gather |
| 173 | `sys_recvmsg` | `ssize_t sys_recvmsg(int fd, struct msghdr *msg, int flags)` | Receive with ancillary data |
| 174 | `sys_shutdown` | `int sys_shutdown(int fd, int how)` | Shut down part of a full-duplex connection |
| 175 | `sys_getpeername` | `int sys_getpeername(int fd, struct sockaddr *addr, socklen_t *len)` | Get remote address |
| 176 | `sys_getsockname` | `int sys_getsockname(int fd, struct sockaddr *addr, socklen_t *len)` | Get local address |

## Address Structures

```c
// Generic (used in syscall signatures):
struct sockaddr { uint16_t sa_family; char sa_data[14]; };

// IPv4:
struct sockaddr_in {
    uint16_t sin_family;    // AF_INET
    uint16_t sin_port;      // network byte order
    uint32_t sin_addr;      // network byte order
    uint8_t  sin_zero[8];
};

// Unix domain:
struct sockaddr_un {
    uint16_t sun_family;    // AF_UNIX
    char     sun_path[108]; // socket filesystem path
};
```

## Security Checks

| Operation | Check |
|---|---|
| `bind` to port < 1024 | Requires `CAP_NET_BIND` |
| `socket(AF_PACKET, ...)` | Requires `CAP_NET_RAW` |
| `socket(AF_INET, SOCK_RAW, ...)` | Requires `CAP_NET_RAW` |
| `connect` to any address | No capability required for unprivileged ports |

## `sys_socket` Implementation

1. Look up the `proto_family` for `domain` in the registered family table.
2. If not found: return `-EAFNOSUPPORT`.
3. Check permissions (e.g., `CAP_NET_RAW` for `SOCK_RAW`).
4. Allocate `struct socket`.
5. Call `pf->create(sock, type, protocol)`.
6. Allocate `struct file`, set `f_ops = &socket_file_ops`.
7. Install in fd table. Return fd.

## `sys_accept4` Implementation

1. Get the listening socket's `struct socket`.
2. Check it is in `SOCK_LISTEN` state.
3. If accept queue is empty and `SOCK_NONBLOCK`: return `-EAGAIN`.
4. If accept queue is empty (blocking): sleep on `sock->wait`.
5. Dequeue from the accept queue: get a fully-connected `struct socket`.
6. Allocate a new `struct file` and fd for the connected socket.
7. If `addr != NULL`: `copy_to_user` the peer address.
8. Return the new fd.

## Related Documents

- [overview.md](overview.md)
- [networking/socket-api.md](../networking/socket-api.md)
- [networking/socket-subsystem.md](../networking/socket-subsystem.md)
- [ipc/unix-sockets.md](../ipc/unix-sockets.md)
