# Driver Communication Protocol

## Overview

The kernel driver core and the userspace driver process communicate via shared memory ring buffers. This section specifies the ring buffer structure, serialization format, and ordering guarantees.

## Ring Buffer Structure

```c
#define RING_SIZE 256   // must be power of 2

struct ring_buf {
    uint32_t head;                   // write index (producer)
    uint32_t tail;                   // read index (consumer)
    uint32_t mask;                   // RING_SIZE - 1
    uint8_t  _pad[52];               // pad to cache line
    struct ring_entry entries[RING_SIZE];
};

struct ring_entry {
    uint32_t type;       // command/event type (device-specific)
    uint32_t flags;      // per-entry flags
    uint64_t id;         // request correlation ID
    uint64_t addr;       // data address (physical or shared memory offset)
    uint32_t len;        // data length
    uint32_t status;     // completion status (0 = success, errno = error)
    uint8_t  payload[32];// inline data or type-specific fields
};
```

## Producer/Consumer Protocol

**Enqueue (producer)**:

1. Compute `next_head = (head + 1) & mask`.
2. If `next_head == tail`: ring is full. Spin or wait for consumer to advance tail.
3. Write `entries[head]` (all fields).
4. Memory barrier (`__asm__ volatile("sfence")` on x86).
5. Increment `head` (atomic store with release semantics).

**Dequeue (consumer)**:

1. Read `head` (atomic load with acquire semantics).
2. If `head == tail`: ring is empty. Sleep on futex or poll.
3. Read `entries[tail]`.
4. Memory barrier (`__asm__ volatile("lfence")`).
5. Increment `tail` (atomic store with release semantics).

The memory barriers ensure that the entry contents are fully visible before the index update.

## Command Types

Each device type defines its own command/event types within `ring_entry.type`:

### Block Device Command Types

| Type | Direction | Description |
|---|---|---|
| `BLK_CMD_READ` | kernel → driver | Read N sectors from LBA |
| `BLK_CMD_WRITE` | kernel → driver | Write N sectors to LBA |
| `BLK_CMD_FLUSH` | kernel → driver | Flush device write cache |
| `BLK_RSP_COMPLETE` | driver → kernel | Request complete; `status` = result |

### Display Command Types

| Type | Direction | Description |
|---|---|---|
| `DISP_CMD_SETMODE` | kernel → driver | Set display mode (width, height, depth) |
| `DISP_CMD_FLIP` | kernel → driver | Flip framebuffer |
| `DISP_RSP_VSYNC` | driver → kernel | Vertical sync event |

## Doorbell

When the producer appends entries to the ring, it writes to the `doorbell` word in the shared region:

```c
atomic_store(&shm->doorbell, ++shm_local_seq, memory_order_release);
```

The consumer uses a futex wait on the `doorbell` to sleep until new entries appear:

```c
sys_futex(&shm->doorbell, FUTEX_WAIT, last_doorbell, timeout, NULL, 0);
```

For low-latency devices, the driver may busy-poll `doorbell` instead of sleeping.

## IRQ Notification (Driver → Kernel)

The userspace driver does not send IRQs to the kernel. Instead:

1. The hardware sends a real IRQ.
2. The kernel ISR increments `shm->irq_count`.
3. The userspace driver polls `irq_count` or waits via futex.

The driver then reads device completion data (from device MMIO registers or DMA buffers) and writes completion entries to `rsp_queue`.

## Related Documents

- [hybrid-architecture.md](hybrid-architecture.md)
- [kernel-drivers.md](kernel-drivers.md)
- [userspace-drivers.md](userspace-drivers.md)
- [ipc/shared-memory.md](../ipc/shared-memory.md)
