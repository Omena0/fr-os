# Networking Performance

## Targets

| Metric | Target | Notes |
|---|---|---|
| TCP throughput (loopback) | > 10 Gbps | Memory bandwidth limited |
| TCP throughput (virtio-net) | > 1 Gbps | QEMU virtio-net limit |
| TCP round-trip latency (loopback) | < 50 µs | |
| TCP round-trip latency (virtio-net) | < 500 µs | QEMU overhead |
| Connections per second | > 100,000 | SYN processing throughput |
| Maximum concurrent connections | > 1,000,000 | Memory: ~2 KB per tcp_sock |

## Hot Path Analysis

### Receive Path (One Packet)

1. NIC DMA completes → IRQ fires (or polling detects packet): ~0–5 µs.
2. `netdev_receive_skb`: IP checksum verification (~50 ns for hardware offload, ~200 ns software).
3. TCP demux (4-tuple hash lookup): ~50 ns.
4. TCP receive processing (ACK, sequencing, window update): ~200 ns.
5. Enqueue to socket receive buffer: ~50 ns.
6. Wake up blocked reader (futex wake): ~200 ns.
7. `recv` in user process: `copy_to_user` for 1 KB payload: ~100 ns.

Total (excluding IRQ latency): ~600 ns software path.

### Transmit Path (One Packet)

1. `send` syscall: `copy_from_user` into `sk_buff`: ~100 ns.
2. TCP segmentation, header construction: ~150 ns.
3. IP header construction + checksum: ~100 ns.
4. Ethernet header + route lookup + ARP: ~100 ns.
5. `start_xmit` → virtio TX queue: ~200 ns.
6. NIC transmits. TX complete IRQ: 1–50 µs (device-dependent).

### Key Bottlenecks

- `copy_from/to_user`: Dominant cost for large payloads. Future zero-copy (`sendfile`, `MSG_ZEROCOPY`) bypasses this.
- Interrupt overhead: At high packet rates (> 100K PPS), per-packet IRQs saturate the CPU. Mitigation: NAPI-style polling (adaptive, switches to polling at high rate).
- Context switches: Each wakeup involves a context switch. Mitigation: batch processing (coalesce multiple packets per wakeup).

## Interrupt Coalescing

The virtio-net driver supports interrupt coalescing:

- At low packet rates: interrupt per packet (minimal latency).
- At high packet rates: interrupt is suppressed while the driver is in poll mode. The driver polls the RX queue in a tight loop, processing up to 64 packets per poll iteration.
- After the burst: interrupts are re-enabled (return to interrupt-driven mode).

This is the NAPI (New API) pattern from Linux, adapted for this OS.

## `sendfile` (Planned)

For serving static files over TCP:

```c
sendfile(out_sock_fd, in_file_fd, offset, count);
```

The kernel transfers data directly from the page cache to the socket send buffer, bypassing `copy_from_user` entirely. One copy: page cache → socket buffer (or zero copies if scatter-gather DMA is available).

## TCP Checksum Offload

Many NICs (including virtio-net with the `VIRTIO_NET_F_CSUM` feature) support hardware TCP checksum computation:

- On transmit: the NIC computes the TCP checksum (kernel sets it to a partial pseudo-header checksum).
- On receive: the NIC verifies the checksum and sets a flag in the descriptor (kernel skips software verification).

When hardware offload is available, the software checksum cost is eliminated (~100 ns per packet).

## Related Documents

- [tcp-implementation.md](tcp-implementation.md)
- [buffering-strategy.md](buffering-strategy.md)
- [device-handling.md](device-handling.md)
- [architecture/performance-mandate.md](../architecture/performance-mandate.md)
