# Raw Sockets

## Overview

Raw sockets (`SOCK_RAW`) allow a process to send and receive IP packets directly, bypassing the transport layer (TCP/UDP). They are used for:

- Custom protocols not implemented by the kernel.
- Packet injection and capture (network testing, monitoring).
- ICMP implementations (`ping`).
- Custom routing protocols.

Raw sockets require `CAP_NET_RAW`.

## Creating a Raw Socket

```c
// Raw ICMP socket:
int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);

// Raw socket for all IP protocols:
int fd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);  // requires IP_HDRINCL

// Raw Ethernet socket (Layer 2):
int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
```

## Receiving Packets

On `recv(fd, buf, len, 0)`:

- The kernel delivers a copy of every matching IP packet to the raw socket's receive buffer.
- Matching: if `protocol == IPPROTO_ICMP`, only ICMP packets are delivered; if `IPPROTO_RAW`, all IP packets.
- The received buffer contains the full IP header followed by the payload.
- No transport layer processing is done (the TCP/UDP state machine does not see these packets).

`AF_PACKET` sockets receive the full Ethernet frame (including the Ethernet header).

## Sending Packets

On `sendto(fd, buf, len, 0, dest_addr, addrlen)`:

- If `IP_HDRINCL` is not set: the kernel prepends an IP header (filling `src`, `ttl`, and `protocol` from socket options; the caller provides the payload).
- If `IP_HDRINCL` is set: the caller provides the full IP header (including checksum — the kernel may recalculate it).

## Packet Capture (AF_PACKET)

`AF_PACKET` with `SOCK_RAW` captures all Ethernet frames on a given interface:

```c
int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
struct sockaddr_ll addr = { .sll_family = AF_PACKET, .sll_ifindex = ifindex };
bind(fd, (struct sockaddr *)&addr, sizeof(addr));

uint8_t buf[65536];
ssize_t n = recv(fd, buf, sizeof(buf), 0);
// buf contains: Ethernet header + IP header + payload
```

## Socket Options for Raw Sockets

| Option | Description |
|---|---|
| `IP_HDRINCL` | Caller provides the full IP header (required for IP options) |
| `IP_TTL` | Set the TTL field in outgoing packets |
| `IP_TOS` | Set the ToS/DSCP byte |
| `SO_BINDTODEVICE` | Restrict to a specific network interface |

## Security

Raw sockets require `CAP_NET_RAW`. This capability is not granted to unprivileged processes by default. The following operations are gated:

- `socket(AF_INET, SOCK_RAW, ...)`: `CAP_NET_RAW`.
- `socket(AF_PACKET, SOCK_RAW, ...)`: `CAP_NET_RAW`.
- Sending spoofed source IP addresses: possible with `IP_HDRINCL` + `CAP_NET_RAW`.

Processes in a network namespace are limited to the interfaces in their namespace — they cannot capture traffic from the host's interfaces.

## Related Documents

- [ipv4-stack.md](ipv4-stack.md)
- [socket-api.md](socket-api.md)
- [socket-subsystem.md](socket-subsystem.md)
- [security/capabilities.md](../security/capabilities.md)
