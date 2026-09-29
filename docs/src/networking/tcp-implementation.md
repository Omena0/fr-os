# TCP Implementation

## Overview

TCP provides reliable, ordered, full-duplex byte stream communication over IP. The OS implements a complete TCP state machine with connection management, flow control, and congestion control.

## TCP State Machine

```
CLOSED ──listen()──► LISTEN ──SYN──► SYN_RCVD ──SYN+ACK+ACK──► ESTABLISHED
CLOSED ──connect()─► SYN_SENT ──SYN+ACK──► ESTABLISHED
ESTABLISHED ──close()──► FIN_WAIT_1 ──FIN+ACK──► FIN_WAIT_2 ──FIN──► TIME_WAIT ──2MSL──► CLOSED
ESTABLISHED ──FIN──► CLOSE_WAIT ──close()──► LAST_ACK ──ACK──► CLOSED
```

Per-connection state is stored in `struct tcp_sock` (one per active connection).

## Connection Management

### Three-Way Handshake (Connect)

1. Client sends SYN (sequence number = ISN, random).
2. Server responds with SYN+ACK (server ISN, ACK = client ISN + 1).
3. Client sends ACK (ACK = server ISN + 1).
4. Connection enters ESTABLISHED.

ISN (Initial Sequence Number) is generated using `csprng_next32()` to prevent ISN prediction attacks (TCP sequence number injection).

### Listen Backlog

`listen(fd, backlog)` sets the maximum number of pending (not yet `accept`'d) connections. The kernel maintains a queue of SYN_RCVD connections. If the queue is full when a new SYN arrives: the SYN is silently dropped (client will retry after timeout).

### Four-Way Termination

When either side calls `close()`:

1. Sends FIN.
2. Remote sends ACK.
3. Remote sends FIN (when it is also done sending).
4. Local sends ACK.
5. TIME_WAIT state: wait 2×MSL (4 seconds) to ensure the final ACK is not lost.

## Sliding Window Flow Control

Both sides advertise a receive window (`rwnd`): the amount of buffer space available for incoming data. The sender must not transmit more than `min(rwnd, cwnd)` bytes beyond the last acknowledged sequence number.

If `rwnd` reaches 0 (receiver's buffer is full), the sender stops and sends probe packets periodically to check if the window has opened.

## Congestion Control (Reno)

Four phases:

| Phase | Trigger | Behavior |
|---|---|---|
| Slow start | After connection setup or timeout | `cwnd` doubles each RTT (exponential growth) |
| Congestion avoidance | `cwnd >= ssthresh` | `cwnd` grows by 1 MSS per RTT (linear growth) |
| Fast retransmit | 3 duplicate ACKs | Retransmit immediately without waiting for timeout |
| Fast recovery | After fast retransmit | Halve `cwnd` + `ssthresh`, re-enter congestion avoidance |

On timeout: `ssthresh = cwnd/2`, `cwnd = 1 MSS`, re-enter slow start.

## Retransmission

Every unacknowledged segment has a retransmit timer (RTO = 1–60 seconds, computed from RTT measurements via the Karn algorithm). On RTO expiry: retransmit the segment and double the RTO (exponential backoff, up to 60 s max).

## Nagle's Algorithm

Small segments are held and coalesced until:

- A full MSS of data is ready, or
- All previously sent data is acknowledged.

This reduces the number of tiny packets on the network. Disabled by `TCP_NODELAY` socket option (for low-latency applications like terminals and gaming).

## Related Documents

- [ipv4-stack.md](ipv4-stack.md)
- [socket-api.md](socket-api.md)
- [socket-subsystem.md](socket-subsystem.md)
- [buffering-strategy.md](buffering-strategy.md)
- [performance.md](performance.md)
