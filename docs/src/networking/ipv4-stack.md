# IPv4 Stack

## Overview

The IPv4 layer handles packet routing, fragmentation/reassembly, and IP checksum computation. It is positioned between the transport layer (TCP, raw) and the device layer (NIC drivers).

## IP Header

```c
struct iphdr {
    uint8_t  ihl : 4;       // header length in 32-bit words (min 5 = 20 bytes)
    uint8_t  version : 4;   // 4 for IPv4
    uint8_t  tos;           // type of service (DSCP/ECN)
    uint16_t tot_len;       // total packet length (header + data)
    uint16_t id;            // identification (for fragmentation)
    uint16_t frag_off;      // flags (3 bits) + fragment offset (13 bits)
    uint8_t  ttl;           // time-to-live
    uint8_t  protocol;      // IPPROTO_TCP=6, IPPROTO_ICMP=1, IPPROTO_RAW=255
    uint16_t check;         // header checksum
    uint32_t saddr;         // source IP (network byte order)
    uint32_t daddr;         // destination IP (network byte order)
    // optional: options (if ihl > 5)
};
```

## Checksum

IP header checksum: 16-bit one's complement sum of all 16-bit words in the header, with the checksum field itself set to 0 during computation.

```c
uint16_t ip_checksum(const void *data, size_t len) {
    uint32_t sum = 0;
    const uint16_t *p = (const uint16_t *)data;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}
```

## Routing

The kernel maintains a routing table:

```c
struct route_entry {
    uint32_t dst;       // destination network (network byte order)
    uint32_t mask;      // subnet mask
    uint32_t gateway;   // next-hop gateway (0 if directly connected)
    struct netdev *dev; // outgoing network interface
    uint32_t metric;    // route preference (lower = higher priority)
};
```

Longest-prefix match is used to select the best route. On a single-interface QEMU setup, the routing table has two entries:

- Local subnet: directly connected via `eth0`.
- Default route (0.0.0.0/0): via the QEMU gateway (default: `10.0.2.2`).

Route lookup: linear scan (simple for small tables). For larger tables: trie-based lookup (future).

## Fragmentation

If a packet exceeds the outgoing interface MTU (default 1500 bytes for Ethernet):

1. The IP layer fragments the packet into MTU-sized pieces.
2. All fragments share the same `id` field.
3. The `frag_off` field indicates the offset of the fragment within the original datagram.
4. All fragments except the last have the `MF` (More Fragments) flag set.

Reassembly:

1. Received fragments are stored in a reassembly queue indexed by `(src_ip, id)`.
2. When all fragments arrive (determined by the last fragment's offset + size = total length): reassemble into a single IP packet.
3. Deliver to the transport layer.
4. Reassembly timeout: 30 seconds. Expired queues are discarded and an ICMP "Time Exceeded" message is sent.

## ICMP

Minimal ICMP support:

- Echo request/reply (ping).
- "Destination Unreachable" (sent when a port is not open: ICMP type 3, code 3 for UDP/raw; TCP sends RST instead).
- "Time Exceeded" (sent on TTL expiry).

No ICMP redirect support (treated as informational only).

## Related Documents

- [tcp-implementation.md](tcp-implementation.md)
- [raw-sockets.md](raw-sockets.md)
- [device-handling.md](device-handling.md)
- [buffering-strategy.md](buffering-strategy.md)
