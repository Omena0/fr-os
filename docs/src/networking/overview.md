# Networking Overview

## Architecture

The network stack provides IPv4 connectivity[^rfc791] with a BSD-compatible socket API[^rfc1122]. It is implemented as a set of kernel subsystems:

```
Applications
    │  (BSD socket API: socket, bind, connect, send, recv, ...)
    ▼
Socket layer (AF_INET, AF_UNIX, AF_PACKET)
    │
    ▼
Transport layer (TCP, UDP, raw IP)
    │
    ▼
Network layer (IPv4: routing, fragmentation, checksum)
    │
    ▼
Device layer (sk_buff buffer management, NIC driver interface)
    │
    ▼
NIC driver (virtio-net / e1000)
```

## Subsystem Documents

| Document | Topic |
|---|---|
| [ipv4-stack.md](ipv4-stack.md) | IP layer: routing, fragmentation, checksum |
| [tcp-implementation.md](tcp-implementation.md) | TCP state machine, connection management, flow control |
| [socket-api.md](socket-api.md) | BSD socket interface: syscall semantics |
| [raw-sockets.md](raw-sockets.md) | SOCK_RAW, packet injection/capture |
| [device-handling.md](device-handling.md) | NIC abstraction, netdev interface |
| [buffering-strategy.md](buffering-strategy.md) | sk_buff structure, zero-copy techniques |
| [socket-subsystem.md](socket-subsystem.md) | Socket object lifecycle, addressing |
| [performance.md](performance.md) | Throughput targets, hot path analysis |

## Design Principles

- **In-kernel TCP**: TCP/IP runs in the kernel (not a userspace network stack). This avoids the context switch overhead of user/kernel round trips per packet. TCP implementation follows RFC 793[^rfc793] and RFC 5681[^rfc5681] for congestion control.
- **Low-copy buffering**: The `sk_buff` structure tracks a packet through the network stack with minimal data copying. Receive: DMA → sk_buff → socket receive buffer → `copy_to_user`. Transmit: `copy_from_user` → sk_buff → DMA.
- **NIC polling + interrupt hybrid**: At low load, NIC interrupts wake the network stack. At high load, the NIC switches to polling (NAPI-like) to avoid interrupt overhead.
- **No UDP stack initially**: UDP is low-priority[^rfc768]. The OS provides raw IP sockets as a workaround for UDP applications. Full UDP is a future milestone.

## QEMU Network Device

In QEMU, the network device is `virtio-net-pci`. The kernel driver:

1. Discovers the device via PCI enumeration.
2. Allocates virtio receive and transmit queues.
3. Registers as a netdev with the network stack.
4. Configures DHCP at init time (via a minimal DHCP client in the init system).

## Related Documents

- [ipv4-stack.md](ipv4-stack.md)
- [tcp-implementation.md](tcp-implementation.md)
- [socket-api.md](socket-api.md)
- [raw-sockets.md](raw-sockets.md)
- [device-handling.md](device-handling.md)
- [buffering-strategy.md](buffering-strategy.md)
- [socket-subsystem.md](socket-subsystem.md)
- [performance.md](performance.md)

## References

- [RFC 791 — Internet Protocol (IPv4)][rfc791]
- [RFC 793 — Transmission Control Protocol (TCP)][rfc793]
- [RFC 768 — User Datagram Protocol (UDP)][rfc768]
- [RFC 1122 — Requirements for Internet Hosts][rfc1122]
- [RFC 1123 — Requirements for Internet Hosts — Application and Support][rfc1123]
- [RFC 5681 — TCP Congestion Control][rfc5681]
- [Linux Kernel Networking Documentation][linux-net]
- [The Linux Kernel Module Programming Guide — Networking][lkmpg-net]

[rfc791]: https://www.rfc-editor.org/rfc/rfc791 "RFC 791: Internet Protocol"
[rfc793]: https://www.rfc-editor.org/rfc/rfc793 "RFC 793: Transmission Control Protocol"
[rfc768]: https://www.rfc-editor.org/rfc/rfc768 "RFC 768: User Datagram Protocol"
[rfc1122]: https://www.rfc-editor.org/rfc/rfc1122 "RFC 1122: Requirements for Internet Hosts"
[rfc1123]: https://www.rfc-editor.org/rfc/rfc1123 "RFC 1123: Requirements for Internet Hosts - Application and Support"
[rfc5681]: https://www.rfc-editor.org/rfc/rfc5681 "RFC 5681: TCP Congestion Control"
[linux-net]: https://www.kernel.org/doc/html/latest/networking/index.html "Linux Kernel Networking Documentation"
[lkmpg-net]: https://sysprog21.github.io/lkmpg/ "The Linux Kernel Module Programming Guide"
