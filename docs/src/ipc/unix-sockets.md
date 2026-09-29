# Unix Domain Sockets

## Overview

Unix domain sockets provide full-duplex, bidirectional communication between processes on the same machine via a filesystem path. Unlike pipes (which are unidirectional), Unix domain sockets support both `SOCK_STREAM` (byte stream) and `SOCK_DGRAM` (datagram) modes.

## Creation

```c
int fd = socket(AF_UNIX, SOCK_STREAM, 0);

struct sockaddr_un addr;
addr.sun_family = AF_UNIX;
strncpy(addr.sun_path, "/tmp/my.sock", sizeof(addr.sun_path) - 1);
bind(fd, (struct sockaddr *)&addr, sizeof(addr));
listen(fd, 10);
int client_fd = accept(fd, NULL, NULL);
```

Client:

```c
int fd = socket(AF_UNIX, SOCK_STREAM, 0);
connect(fd, (struct sockaddr *)&addr, sizeof(addr));
```

## Socket Types

| Type | Behavior |
|---|---|
| `SOCK_STREAM` | Reliable, ordered, full-duplex byte stream (like TCP but local) |
| `SOCK_DGRAM` | Unreliable, unordered datagrams; no connection required |
| `SOCK_SEQPACKET` | Reliable, ordered, message-preserving (like `SOCK_STREAM` but preserves message boundaries) |

## Filesystem Integration

`bind(AF_UNIX, path)` creates a special socket inode in the filesystem at `path`. The inode has type `S_IFSOCK`. Its permissions control who can connect (`connect` requires write permission on the socket inode).

`unlink(path)` removes the socket inode when the server is done.

## Internal Buffer

Each connected Unix socket pair shares two buffers (one per direction), implemented as ring buffers identical to pipe buffers:

```c
struct unix_sock_pair {
    struct ring_buf a_to_b;    // data from peer A to peer B
    struct ring_buf b_to_a;    // data from peer B to peer A
    wait_queue_t reader_wait_a, reader_wait_b;
    wait_queue_t writer_wait_a, writer_wait_b;
};
```

Buffer size: 64 KB per direction (same as pipe default). Configurable via `SO_SNDBUF`/`SO_RCVBUF`.

## File Descriptor Passing (SCM_RIGHTS)

Unix domain sockets support sending open file descriptors between processes via the `sendmsg`/`recvmsg` ancillary data mechanism:

```c
// Sender:
struct msghdr msg = { ... };
struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
cmsg->cmsg_level = SOL_SOCKET;
cmsg->cmsg_type = SCM_RIGHTS;
int *fdptr = (int *)CMSG_DATA(cmsg);
*fdptr = fd_to_send;
sendmsg(sock_fd, &msg, 0);

// Receiver:
recvmsg(sock_fd, &msg, 0);
int received_fd = *(int *)CMSG_DATA(CMSG_FIRSTHDR(&msg));
```

The kernel transfers the file descriptor by creating a new fd in the receiver's fd table pointing to the same `struct file` object as the sender's fd. This is used by display servers, privilege-separated services, and container runtimes.

## Performance

For local IPC:

- Unix domain sockets: ~1–5 µs round-trip (small messages).
- Pipes: ~1–3 µs (but unidirectional).
- Shared memory + futex: ~0.1–0.5 µs (fastest, but requires explicit synchronization).

Unix sockets are preferred for service-to-service IPC where bidirectionality and message framing are needed.

## Related Documents

- [pipes.md](pipes.md)
- [fd-abstraction.md](fd-abstraction.md)
- [networking/socket-api.md](../networking/socket-api.md)
- [ipc/overview.md](overview.md)
