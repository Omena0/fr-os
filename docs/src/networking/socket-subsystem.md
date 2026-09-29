# Socket Subsystem

## Overview

The socket subsystem manages the lifecycle of socket objects — from creation through binding, connection, and destruction. It provides the glue between the BSD socket API and the underlying protocol implementations.

## Socket Object

```c
struct socket {
    int             type;        // SOCK_STREAM, SOCK_DGRAM, SOCK_RAW
    int             state;       // SS_UNCONNECTED, SS_CONNECTING, SS_CONNECTED, SS_DISCONNECTING
    uint32_t        flags;       // SOCK_NONBLOCK, SOCK_CLOEXEC
    struct sock    *sk;          // protocol-specific socket (TCP sock, raw sock, etc.)
    struct socket_ops *ops;      // protocol-specific operations
    struct file    *file;        // back-pointer to the file object (for fd lookup)
    wait_queue_t    wait;        // wait queue for blocking operations
};
```

## Protocol Family Registration

Each address family registers with the socket subsystem:

```c
struct proto_family {
    int     family;         // AF_INET, AF_UNIX, AF_PACKET
    int   (*create)(struct socket *sock, int type, int protocol);
    struct list_head list;
};

void register_proto_family(struct proto_family *pf);
```

`sys_socket(domain, type, protocol)`:

1. Look up `proto_family` by `domain`.
2. Allocate a `struct socket`.
3. Call `pf->create(socket, type, protocol)` — initializes `socket->sk` and `socket->ops`.
4. Allocate a `struct file`, set `f_ops = &socket_file_ops`.
5. Install in the fd table. Return fd.

## TCP Socket (`struct tcp_sock`)

```c
struct tcp_sock {
    struct sock   base;         // common sock fields (src/dst IP, port, state)
    uint32_t      snd_una;      // oldest unacknowledged send sequence
    uint32_t      snd_nxt;      // next sequence to send
    uint32_t      rcv_nxt;      // next expected receive sequence
    uint32_t      rcv_wnd;      // receive window advertised to peer
    uint32_t      snd_wnd;      // send window (peer's advertised window)
    uint32_t      cwnd;         // congestion window
    uint32_t      ssthresh;     // slow start threshold
    uint32_t      rto;          // retransmit timeout (microseconds)
    uint32_t      rtt_avg;      // smoothed RTT estimate
    // ... more TCP state ...
};
```

## Connection Demultiplexing

Incoming TCP packets are demultiplexed to the correct `tcp_sock` by a 4-tuple hash:

```
hash(src_ip, src_port, dst_ip, dst_port) → struct tcp_sock *
```

The hash table has `TCP_HASH_SIZE = 4096` buckets. Collisions are handled by a linked list within each bucket.

## `accept` Queue

For listening sockets, completed connections are placed in an accept queue:

```c
struct listen_sock {
    struct socket_queue *accept_queue; // FIFO of fully connected sockets
    int                  backlog;      // max accept queue depth
    int                  qlen;         // current queue depth
};
```

`sys_accept` dequeues from `accept_queue`. If empty: blocks on `socket->wait`.

## Port Assignment

`sys_bind(AF_INET, ...)` with `sin_port == 0`: the kernel selects an ephemeral port from the range 49152–65535 (not currently in use for any socket with the same local IP).

Port 0–1023: requires `CAP_NET_BIND`.

## Socket Options via `getsockopt`

```c
int  sys_getsockopt(int fd, int level, int optname, void *val, socklen_t *len);
```

Reads per-socket configuration. Common options:

- `SO_ERROR`: retrieve and clear the pending error on the socket.
- `SO_TYPE`: retrieve the socket type.
- `TCP_INFO`: retrieve detailed TCP state (RTT, cwnd, etc.) for monitoring.

## Related Documents

- [socket-api.md](socket-api.md)
- [tcp-implementation.md](tcp-implementation.md)
- [buffering-strategy.md](buffering-strategy.md)
- [ipc/unix-sockets.md](../ipc/unix-sockets.md)
