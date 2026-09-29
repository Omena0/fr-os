# Storage Driver

## Overview

The storage driver provides block device access (read/write of fixed-size sectors) to the filesystem layer. In QEMU, the primary storage device is `virtio-blk`, which is exposed as a PCI device.

## virtio-blk Protocol

virtio-blk is a virtual block device specification. The kernel communicates with it via virtio split queues:

### Virtio Queue Structure

```c
struct virtq_desc {
    uint64_t addr;    // physical address of buffer
    uint32_t len;     // buffer length
    uint16_t flags;   // VIRTQ_DESC_F_NEXT, VIRTQ_DESC_F_WRITE
    uint16_t next;    // index of next descriptor in chain (if NEXT flag set)
};

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;         // producer index
    uint16_t ring[];      // descriptor indices
};

struct virtq_used {
    uint16_t flags;
    uint16_t idx;         // consumer index
    struct { uint32_t id; uint32_t len; } ring[];
};
```

### I/O Request Format

```c
struct virtio_blk_req {
    uint32_t type;    // VIRTIO_BLK_T_IN (read) or VIRTIO_BLK_T_OUT (write)
    uint32_t _pad;
    uint64_t sector;  // LBA sector number
    // followed by: data buffer (one or more DMA descriptors)
    // followed by: 1-byte status buffer (written by device: 0=OK, 1=error)
};
```

### Submission Path

1. Allocate a descriptor chain in the virtq_desc table:
   - Descriptor 0: `struct virtio_blk_req` header.
   - Descriptor 1–N: data buffers (DMA addresses of pages to read/write).
   - Descriptor N+1: 1-byte status buffer (device-writable).
2. Write the first descriptor index into `virtq_avail.ring[avail.idx]`. Increment `avail.idx`.
3. Write to the virtio notify register (MMIO doorbell) to kick the device.

### Completion Path

When the device completes an I/O:

1. The device writes the completion entry to `virtq_used` and sends an IRQ (MSI or INTx).
2. The kernel ISR reads `virtq_used`, identifies the completed descriptor chain by ID.
3. The status byte is checked (0 = success). The corresponding block request is completed.
4. The `rsp_queue` entry is written to notify the userspace driver process.

## Block I/O Queue

The kernel block layer maintains a per-device I/O queue:

```c
struct bio {
    uint64_t     sector;       // starting LBA sector
    uint32_t     nsectors;     // number of 512-byte sectors
    uint8_t      op;           // BIO_OP_READ or BIO_OP_WRITE
    struct page *pages[];      // physical pages of data
    void       (*complete)(struct bio *, int status);
    void        *private;
};
```

The userspace storage driver receives `BLK_CMD_READ`/`BLK_CMD_WRITE` commands, translates them into virtio requests, submits to the device, and calls the completion callback on the kernel side.

## Sector and Block Sizes

- Sector size: 512 bytes (virtio-blk default).
- Kernel block size: 4096 bytes (8 sectors). All I/O is aligned to 4 KB blocks.
- Max transfer: 1024 sectors (512 KB) per request.

## Related Documents

- [overview.md](overview.md)
- [kernel-drivers.md](kernel-drivers.md)
- [hybrid-architecture.md](hybrid-architecture.md)
- [filesystem/ext4-journaling.md](../filesystem/ext4-journaling.md)
