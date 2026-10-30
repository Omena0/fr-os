/*
 * block.h - block devices and the layer above them.
 *
 * A block device is a fixed number of 512-byte sectors with a synchronous
 * read/write. Drivers register one here; the filesystem layer mounts one by
 * looking at its first sector.
 */
#ifndef BLOCK_H
#define BLOCK_H

#include <types.h>

#define BLOCK_SECTOR_SIZE 512
#define BLOCK_MAX_DEVICES 4

struct block_device {
    const char *name;
    u32 sectors;
    u32 sector_size;
    int (*read)(struct block_device *dev, u64 lba, u32 count, void *buf);
    int (*write)(struct block_device *dev, u64 lba, u32 count, const void *buf);
    void *private;
};

/* Probe the ATA buses and register every drive found. Idempotent. */
int block_init(void);

int block_device_count(void);
struct block_device *block_device_at(int index);
u64 block_device_bytes(struct block_device *dev);

/* Read or write whole sectors. Both return 0 or a negative errno. */
int block_read(struct block_device *dev, u64 lba, u32 count, void *buf);
int block_write(struct block_device *dev, u64 lba, u32 count, const void *buf);

#endif
