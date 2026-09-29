# Network Buffering Strategy

## Overview

Efficient packet buffering is critical for networking performance. The network stack uses `sk_buff` (socket buffer) structures to represent packets as they move through the stack, minimizing data copies.

## sk_buff Structure

```c
struct sk_buff {
    struct sk_buff  *next, *prev;   // linked list in queues
    struct netdev   *dev;           // network device this packet is associated with
    
    // Buffer pointers (all within 'data_buf'):
    uint8_t *head;     // start of allocated buffer
    uint8_t *data;     // start of actual packet data (moves as headers are added/stripped)
    uint8_t *tail;     // end of packet data
    uint8_t *end;      // end of allocated buffer
    
    uint32_t len;      // data length (tail - data)
    uint32_t truesize; // total memory used by this sk_buff + data
    
    // Layer headers (pointers into the data buffer):
    struct ethhdr  *mac_header;
    struct iphdr   *ip_header;
    struct tcphdr  *transport_header;
    
    // Metadata:
    uint64_t timestamp_ns;
    uint32_t priority;
    uint16_t protocol;    // ethertype
};
```

## Headroom and Tailroom

The data buffer has extra space reserved at both ends:

- **Headroom** (`data - head`): Space for protocol headers to be prepended without copying.
- **Tailroom** (`end - tail`): Space for protocol trailers or appending data.

When building a packet for transmission:

1. IP layer: sets `data` = `head + 64` (default headroom). Writes IP payload.
2. IP header prepend: `data -= sizeof(iphdr)`. Writes IP header. No copy.
3. Ethernet header prepend: `data -= sizeof(ethhdr)`. Writes Ethernet header. No copy.
4. Driver: passes `(data, len)` directly to NIC DMA.

Total copies for transmit: one (`copy_from_user` for the user payload). Zero for headers.

## sk_buff Allocation

```c
struct sk_buff *skb_alloc(size_t data_size, size_t headroom);
void skb_free(struct sk_buff *skb);
```

Backed by the kernel SLAB allocator. Separate SLAB caches for `struct sk_buff` (fixed-size header) and data buffers (variable-size, backed by buddy allocator for large packets or SLAB for small packets ≤ 2 KB).

## Receive Buffer

Each socket has a receive buffer (`sk_rcvbuf`): a linked list of `sk_buff` objects:

```c
struct socket {
    struct sk_buff_head  rcv_queue;  // received but not yet delivered to user
    uint32_t             rcv_buf_size; // current bytes in rcv_queue
    uint32_t             rcv_buf_max; // SO_RCVBUF limit (default 128 KB)
    wait_queue_t         rcv_wait;   // blocked readers
};
```

When an IP packet arrives for a TCP connection:

1. TCP processes the segment (ACK updates, sequencing).
2. In-order data is placed in the receive queue.
3. `recv()` in userspace: dequeues `sk_buff`s, copies data to user buffer, frees `sk_buff`s.

## Send Buffer

```c
struct socket {
    struct sk_buff_head  snd_queue;  // sent but not yet ACK'd (retransmit buffer)
    uint32_t             snd_buf_size;
    uint32_t             snd_buf_max; // SO_SNDBUF limit (default 128 KB)
    wait_queue_t         snd_wait;   // blocked writers (buffer full)
};
```

On `send()`:

1. `copy_from_user` into a new `sk_buff`.
2. Enqueue in `snd_queue` (under the send buffer limit).
3. Pass to TCP for segmentation and transmission.
4. TCP retains `sk_buff` until ACK'd (for retransmit); then frees.

## Zero-Copy Receive (Future)

For high-throughput workloads, zero-copy receive eliminates the `copy_to_user` step:

- The kernel maps the `sk_buff` data page directly into the process's virtual address space.
- The process reads directly from the mapped page.
- On release (via `recv(MSG_ZEROCOPY)` API), the kernel unmaps the page.
- This avoids one memory copy per received packet.

## Related Documents

- [device-handling.md](device-handling.md)
- [tcp-implementation.md](tcp-implementation.md)
- [socket-subsystem.md](socket-subsystem.md)
- [performance.md](performance.md)
