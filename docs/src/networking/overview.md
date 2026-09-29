# Networking Overview

## Architecture

The network stack provides IPv4 connectivity with a BSD-compatible socket API. It is implemented as a set of kernel subsystems:

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

- **In-kernel TCP**: TCP/IP runs in the kernel (not a userspace network stack). This avoids the context switch overhead of user/kernel round trips per packet.
- **Low-copy buffering**: The `sk_buff` structure tracks a packet through the network stack with minimal data copying. Receive: DMA → sk_buff → socket receive buffer → `copy_to_user`. Transmit: `copy_from_user` → sk_buff → DMA.
- **NIC polling + interrupt hybrid**: At low load, NIC interrupts wake the network stack. At high load, the NIC switches to polling (NAPI-like) to avoid interrupt overhead.
- **No UDP stack initially**: UDP is low-priority. The OS provides raw IP sockets as a workaround for UDP applications. Full UDP is a future milestone.

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
