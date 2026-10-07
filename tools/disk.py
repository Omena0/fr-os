"""
 * disk.py — assemble the bootable disk image.
 *
 * The image layout is fixed and unpartitioned, matching src/boot/boot_layout.h:
 *
 *   LBA 0        stage1 (the MBR, exactly 512 bytes)
 *   LBA 1..63    stage2 (up to 32256 bytes)
 *   LBA 64+      the kernel ELF64 image
 *   LBA 4096+    the initrd: a userspace bundle appended after the kernel
 *
 * A partition table would buy nothing here: stage1 reads stage2 by LBA, and
 * stage2 reads the kernel ELF by LBA. Leaving the image unpartitioned keeps the
 * boot path free of any filesystem dependency, so the kernel is the only thing
 * that has to understand disk structure — and when the ext4 driver lands it can
 * be mounted from a partition added at that point, without touching the
 * bootloaders.
 *
 * The kernel is written at a fixed LBA and the bootloaders were compiled with
 * that same constant, so the two can never disagree about where the image is.
"""

import argparse
import struct
import sys

SECTOR = 512

# Mirrors src/boot/boot_layout.h. If these change, boot_layout.h must change
# with them; the build fails loudly if the constants do not fit.
STAGE1_LBA = 0
STAGE2_LBA = 1
STAGE2_MAX_SECTORS = 63
KERNEL_LBA = 64
KERNEL_MAX_SECTORS = 4096
INITRD_LBA = 4096

# Where the userspace bundle begins inside the initrd, and its size. The kernel
# reads a small header at the start of the initrd to find it.
INITRD_MAGIC = 0x4F53425255444E44  # "OSRBUNDND"
INITRD_HEADER_FMT = "<QIIQ"  # magic, version, entry_count, total_len
INITRD_HEADER_SIZE = struct.calcsize(INITRD_HEADER_FMT)


def pad_to_sectors(data: bytes, what: str, limit: int = 0) -> int:
    """Return the sector count for `data`, or exit if it needs more than the
    reserved space."""
    sectors = (len(data) + SECTOR - 1) // SECTOR
    if limit and sectors > limit:
        print(f"error: {what} needs {sectors} sectors, limit is {limit}",
              file=sys.stderr)
        raise SystemExit(1)
    return sectors


def read(path: str) -> bytes:
    with open(path, "rb") as fh:
        return fh.read()


def main() -> int:
    ap = argparse.ArgumentParser(description="assemble the OS disk image")
    ap.add_argument("--stage1", required=True)
    ap.add_argument("--stage2", required=True)
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--initrd", help="prebuilt initrd blob")
    ap.add_argument("--out", required=True)
    ap.add_argument("--size-mb", type=int, default=64)
    args = ap.parse_args()

    stage1 = read(args.stage1)
    stage2 = read(args.stage2)
    kernel = read(args.kernel)

    if len(stage1) != SECTOR:
        print(f"error: stage1 must be exactly {SECTOR} bytes, got {len(stage1)}",
              file=sys.stderr)
        return 1
    if stage1[510:512] != b"\x55\xaa":
        print("error: stage1 is missing the MBR signature at offset 510",
              file=sys.stderr)
        return 1

    stage2_sectors = pad_to_sectors(stage2, "stage2", STAGE2_MAX_SECTORS)

    kernel_sectors = pad_to_sectors(kernel, "kernel", KERNEL_MAX_SECTORS)

    if KERNEL_LBA + kernel_sectors > INITRD_LBA:
        print(f"error: kernel ends at LBA {KERNEL_LBA + kernel_sectors}, "
              f"overlapping initrd at LBA {INITRD_LBA}", file=sys.stderr)
        return 1

    if args.initrd:
        initrd = read(args.initrd)
    else:
        initrd = b""

    initrd_sectors = pad_to_sectors(initrd, "initrd") if initrd else 0

    total_sectors = args.size_mb * 1024 * 1024 // SECTOR
    if INITRD_LBA + initrd_sectors > total_sectors:
        print(f"error: initrd ends at LBA {INITRD_LBA + initrd_sectors}, "
              f"image is {total_sectors} sectors", file=sys.stderr)
        return 1

    # Build the image as a bytearray of the full size and carve it up. Sizing
    # the whole image up front means no file-offset arithmetic can run past the
    # end, and a short write cannot produce a silently truncated disk.
    image = bytearray(total_sectors * SECTOR)

    def place(lba: int, payload: bytes) -> None:
        start = lba * SECTOR
        image[start:start + len(payload)] = payload

    place(STAGE1_LBA, stage1)
    place(STAGE2_LBA, stage2)
    place(KERNEL_LBA, kernel)
    if initrd:
        place(INITRD_LBA, initrd)

    with open(args.out, "wb") as fh:
        fh.write(image)

    print(f"disk: {args.out} ({args.size_mb} MiB)")
    print(f"  stage1  LBA {STAGE1_LBA:<6} {len(stage1):>8} bytes")
    print(f"  stage2  LBA {STAGE2_LBA:<6} {len(stage2):>8} bytes "
          f"({stage2_sectors} sectors)")
    print(f"  kernel  LBA {KERNEL_LBA:<6} {len(kernel):>8} bytes "
          f"({kernel_sectors} sectors)")
    if initrd:
        print(f"  initrd  LBA {INITRD_LBA:<6} {len(initrd):>8} bytes "
              f"({initrd_sectors} sectors)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
