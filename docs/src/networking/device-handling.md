# Network Device Handling

## Overview

The network device layer abstracts NIC hardware into a uniform interface used by the IP stack. Each NIC is represented as a `struct netdev`.

## Network Device Structure

```c
struct netdev {
    char        name[16];         // interface name ("eth0", "lo", ...)
    uint8_t     mac[6];           // MAC address
    uint32_t    ip;               // assigned IPv4 address (0 if unconfigured)
    uint32_t    netmask;          // subnet mask
    uint32_t    mtu;              // maximum transmission unit (default 1500)
    uint32_t    flags;            // IFF_UP, IFF_RUNNING, IFF_LOOPBACK, ...
    uint64_t    tx_bytes, rx_bytes, tx_packets, rx_packets; // stats
    struct netdev_ops *ops;       // driver operations
    void        *driver_data;     // NIC driver private data
    struct list_head tx_queue;    // pending transmit sk_buffs
    spinlock_t  tx_lock;
};

struct netdev_ops {
    int     (*start_xmit)(struct netdev *dev, struct sk_buff *skb); // transmit a packet
    int     (*set_rx_mode)(struct netdev *dev);  // update multicast filter
    void    (*get_stats)(struct netdev *dev);    // update stats counters
};
```

## Packet Reception Path

1. **NIC DMA**: The NIC DMA-transfers the received packet into a pre-allocated `sk_buff`'s data buffer.
2. **IRQ or poll**: NIC generates an interrupt (or is polled). The driver ISR/poll function reads the received packet.
3. **`netdev_receive_skb(dev, skb)`**: The driver hands the `sk_buff` to the network stack.
4. **Ethernet demux**: The Ethernet header is parsed. Supported ethertypes: `0x0800` (IPv4), `0x0806` (ARP). Unknown ethertypes: dropped.
5. **ARP processing**: ARP requests for the local IP are answered. ARP replies update the ARP cache.
6. **IPv4 delivery**: `ip_receive(skb)`. Validates IP checksum. Routes to TCP receive or raw socket delivery.

## Packet Transmission Path

1. TCP/IP builds an `sk_buff` and calls `ip_output(skb)`.
2. IP layer: fills IP header, computes checksum, looks up route, resolves next-hop MAC via ARP cache.
3. Ethernet layer: prepends Ethernet header (src MAC = `dev->mac`, dst MAC = next-hop MAC).
4. `dev->ops->start_xmit(dev, skb)`: driver puts the packet into the NIC's DMA TX queue.
5. NIC transmits. On completion: NIC generates a TX complete interrupt. Driver frees the `sk_buff`.

## ARP Cache

The ARP cache maps IPv4 addresses to MAC addresses:

```c
struct arp_entry {
    uint32_t ip;
    uint8_t  mac[6];
    uint64_t expiry_ns;      // cache entries expire after 5 minutes
    enum { ARP_INCOMPLETE, ARP_COMPLETE } state;
};
```

Cache size: 256 entries (sufficient for a local subnet). Entries expire after 5 minutes. On ARP miss: the packet is held while an ARP request is sent. If no response in 3 seconds: the packet is dropped.

## Loopback Interface

A loopback interface (`lo`) is always present:

- IP: `127.0.0.1`, mask `255.0.0.0`.
- Packets sent to `lo` are looped back directly (no DMA, no hardware).
- Used for local service communication and testing.

## virtio-net Driver

For QEMU, the `virtio-net` PCI device is used:

- Two virtio queues: RX queue and TX queue (one each; multi-queue is a future feature).
- RX: kernel pre-populates the RX queue with empty `sk_buff` data buffers. virtio-net fills them with received packets.
- TX: kernel puts `sk_buff` data buffers into the TX queue. virtio-net sends them.

## Related Documents

- [ipv4-stack.md](ipv4-stack.md)
- [buffering-strategy.md](buffering-strategy.md)
- [drivers/kernel-drivers.md](../drivers/kernel-drivers.md)
