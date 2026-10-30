/*
 * ata.c - ATA PIO block driver.
 *
 * Two channels, master and slave on each, 28-bit LBA addressing and 512-byte
 * sectors. Transfers are polled, not interrupt-driven: a boot-time filesystem
 * read has no other work to overlap, and polling keeps the driver usable before
 * the interrupt layer is ready. QEMU's IDE controller implements all of this.
 *
 * This is the first storage driver, chosen because it needs no PCI
 * enumeration. The block layer above it is driver-agnostic, so a virtio-blk
 * driver (docs/src/drivers/storage-driver.md) can be added beside it.
 */
#include <block.h>
#include <io.h>
#include <klog.h>
#include <kstring.h>

#define ATA_PRIMARY_IO    0x1F0
#define ATA_PRIMARY_CTRL  0x3F6
#define ATA_SECONDARY_IO  0x170
#define ATA_SECONDARY_CTRL 0x376

/* Register offsets from the I/O base. */
#define ATA_REG_DATA   0
#define ATA_REG_ERROR  1
#define ATA_REG_SECCOUNT 2
#define ATA_REG_LBA0   3
#define ATA_REG_LBA1   4
#define ATA_REG_LBA2   5
#define ATA_REG_DRIVE  6
#define ATA_REG_STATUS 7
#define ATA_REG_CMD    7
#define ATA_REG_ALTSTATUS 0

/* Status bits. */
#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DF   0x20
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

/* Commands. */
#define ATA_CMD_READ_PIO  0x20
#define ATA_CMD_WRITE_PIO 0x30
#define ATA_CMD_IDENTIFY  0xEC

#define ATA_TIMEOUT 1000000u

struct ata_drive {
    u16 io;
    u16 ctrl;
    u8 slave;
    bool present;
    bool lba48;
    u32 sectors;
};

static struct block_device ata_devices[BLOCK_MAX_DEVICES];
static struct ata_drive ata_drives[BLOCK_MAX_DEVICES];
static int ata_count;
static bool ata_ready;

static int ata_wait(struct ata_drive *d, u8 mask, u8 value)
{
    for (u32 i = 0; i < ATA_TIMEOUT; i++) {
        u8 status = inb(d->io + ATA_REG_STATUS);

        if ((status & mask) == value)
            return 0;
        io_wait();
    }
    return -1;
}

static void ata_delay400ns(struct ata_drive *d)
{
    (void)inb(d->ctrl + ATA_REG_ALTSTATUS);
    (void)inb(d->ctrl + ATA_REG_ALTSTATUS);
    (void)inb(d->ctrl + ATA_REG_ALTSTATUS);
    (void)inb(d->ctrl + ATA_REG_ALTSTATUS);
}

/* Select the drive and wait for it to stop being busy. */
static int ata_select(struct ata_drive *d, u32 lba)
{
    outb(d->io + ATA_REG_DRIVE,
         0xE0 | (d->slave ? 0x10 : 0) | ((lba >> 24) & 0x0F));
    ata_delay400ns(d);
    return ata_wait(d, ATA_SR_BSY, 0);
}

static int ata_identify(struct ata_drive *d)
{
    u16 id[256];

    outb(d->io + ATA_REG_DRIVE, 0xA0 | (d->slave ? 0x10 : 0));
    ata_delay400ns(d);

    outb(d->io + ATA_REG_SECCOUNT, 0);
    outb(d->io + ATA_REG_LBA0, 0);
    outb(d->io + ATA_REG_LBA1, 0);
    outb(d->io + ATA_REG_LBA2, 0);
    outb(d->io + ATA_REG_CMD, ATA_CMD_IDENTIFY);

    u8 status = inb(d->io + ATA_REG_STATUS);

    if (status == 0)
        return -1;              /* no drive on this slot */
    if (ata_wait(d, ATA_SR_BSY, 0) < 0)
        return -1;
    if (inb(d->io + ATA_REG_LBA1) || inb(d->io + ATA_REG_LBA2))
        return -1;              /* not an ATA device */

    if (ata_wait(d, ATA_SR_DRQ, ATA_SR_DRQ) < 0)
        return -1;

    insw(d->io + ATA_REG_DATA, id, 256);

    d->lba48 = (id[83] & 0x0400) != 0;
    u32 sectors = ((u32)id[61] << 16) | id[60];

    d->sectors = sectors;
    d->present = sectors > 0;
    return d->present ? 0 : -1;
}

static int ata_read(struct block_device *dev, u64 lba, u32 count, void *buf)
{
    struct ata_drive *d = dev->private;
    u8 *p = buf;

    if (!d || !d->present || lba + count > d->sectors)
        return -1;

    while (count--) {
        if (ata_select(d, (u32)lba) < 0)
            return -1;
        outb(d->io + ATA_REG_SECCOUNT, 1);
        outb(d->io + ATA_REG_LBA0, (u8)lba);
        outb(d->io + ATA_REG_LBA1, (u8)(lba >> 8));
        outb(d->io + ATA_REG_LBA2, (u8)(lba >> 16));
        outb(d->io + ATA_REG_CMD, ATA_CMD_READ_PIO);

        if (ata_wait(d, ATA_SR_BSY, 0) < 0)
            return -1;
        if (ata_wait(d, ATA_SR_DRQ, ATA_SR_DRQ) < 0)
            return -1;
        insw(d->io + ATA_REG_DATA, p, 256);
        p += BLOCK_SECTOR_SIZE;
        lba++;
    }
    return 0;
}

static int ata_write(struct block_device *dev, u64 lba, u32 count, const void *buf)
{
    struct ata_drive *d = dev->private;
    const u8 *p = buf;

    if (!d || !d->present || lba + count > d->sectors)
        return -1;

    while (count--) {
        if (ata_select(d, (u32)lba) < 0)
            return -1;
        outb(d->io + ATA_REG_SECCOUNT, 1);
        outb(d->io + ATA_REG_LBA0, (u8)lba);
        outb(d->io + ATA_REG_LBA1, (u8)(lba >> 8));
        outb(d->io + ATA_REG_LBA2, (u8)(lba >> 16));
        outb(d->io + ATA_REG_CMD, ATA_CMD_WRITE_PIO);

        if (ata_wait(d, ATA_SR_BSY, 0) < 0)
            return -1;
        if (ata_wait(d, ATA_SR_DRQ, ATA_SR_DRQ) < 0)
            return -1;
        outsw(d->io + ATA_REG_DATA, p, 256);
        /* Flush the write before returning, so a read after a write sees it. */
        outb(d->io + ATA_REG_CMD, 0xE7);
        if (ata_wait(d, ATA_SR_BSY, 0) < 0)
            return -1;
        p += BLOCK_SECTOR_SIZE;
        lba++;
    }
    return 0;
}

int block_init(void)
{
    static const struct {
        u16 io;
        u16 ctrl;
        const char *name;
    } channels[2] = {
        { ATA_PRIMARY_IO, ATA_PRIMARY_CTRL, "hda" },
        { ATA_SECONDARY_IO, ATA_SECONDARY_CTRL, "hdc" },
    };

    if (ata_ready)
        return ata_count;
    ata_ready = true;
    ata_count = 0;

    for (int ch = 0; ch < 2; ch++) {
        for (int unit = 0; unit < 2; unit++) {
            struct ata_drive *d = &ata_drives[ata_count];
            struct block_device *b = &ata_devices[ata_count];

            d->io = channels[ch].io;
            d->ctrl = channels[ch].ctrl;
            d->slave = (u8)unit;
            d->present = false;
            d->sectors = 0;
            if (ata_identify(d) < 0)
                continue;

            b->name = unit ? (ch == 0 ? "hdb" : "hdd")
                           : channels[ch].name;
            b->sectors = d->sectors;
            b->sector_size = BLOCK_SECTOR_SIZE;
            b->read = ata_read;
            b->write = ata_write;
            b->private = d;
            ata_count++;

            klog_emit(KLOG_INFO, "ata", "%s: %u sectors (%llu KiB)",
                      b->name, b->sectors,
                      (unsigned long long)b->sectors / 2);
        }
    }
    return ata_count;
}

int block_device_count(void)
{
    return ata_count;
}

struct block_device *block_device_at(int index)
{
    if (index < 0 || index >= ata_count)
        return NULL;
    return &ata_devices[index];
}

u64 block_device_bytes(struct block_device *dev)
{
    return dev ? (u64)dev->sectors * dev->sector_size : 0;
}

int block_read(struct block_device *dev, u64 lba, u32 count, void *buf)
{
    if (!dev || !dev->read)
        return -1;
    return dev->read(dev, lba, count, buf);
}

int block_write(struct block_device *dev, u64 lba, u32 count, const void *buf)
{
    if (!dev || !dev->write)
        return -1;
    return dev->write(dev, lba, count, buf);
}
