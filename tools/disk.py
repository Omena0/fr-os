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
INITRD_HEADER_FMT = "<QIIQ"  # magic, version, flags, total_len


def pad_to_sectors(data: bytes, what: str) -> int:
    """Return the sector count for `data`, or exit if it needs more than the
    reserved space."""
    sectors = (len(data) + SECTOR - 1) // SECTOR
    return sectors


def read(path: str) -> bytes:
    with open(path, "rb") as fh:
        return fh.read()


def build_initrd(entries: list[tuple[str, str]]) -> bytes:
    """Bundle userspace programs into an initrd.

    The format is deliberately trivial: a header followed by fixed-size records
    of (name, offset, size, mode). The kernel's initrd reader walks it linearly
    and copies each entry into the ramdisk filesystem. Using a real tar here
    would force a tar parser into the kernel for no benefit — the kernel mounts
    this into a ramfs, not a disk.
    """
    body = bytearray()
    index = bytearray()

    for name, path in entries:
        payload = read(path)
        offset = len(body)
        body.extend(payload)
        # Record: name_len(u16), mode(u16), size(u64), offset(u64)
        name_bytes = name.encode()
        index.extend(struct.pack("<HHQQ", len(name_bytes), 0o755, len(payload), offset))
        index.extend(name_bytes)

    total = 16 + len(index) + len(body)
    header = struct.pack(INITRD_HEADER_FMT, INITRD_MAGIC, 1, 0, total)
    return header + bytes(index) + bytes(body)


def main() -> int:
    ap = argparse.ArgumentParser(description="assemble the OS disk image")
    ap.add_argument("--stage1", required=True)
    ap.add_argument("--stage2", required=True)
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--initrd", help="prebuilt initrd blob")
    ap.add_argument("--out", required=True)
    ap.add_argument("--init", action="append", default=[],
                    metavar="NAME=PATH",
                    help="userspace program to include in the initrd")
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

    stage2_sectors = pad_to_sectors(stage2, "stage2")
    if stage2_sectors > STAGE2_MAX_SECTORS:
        print(f"error: stage2 is {stage2_sectors} sectors, "
              f"limit is {STAGE2_MAX_SECTORS} ({STAGE2_MAX_SECTORS * SECTOR} bytes)",
              file=sys.stderr)
        return 1

    kernel_sectors = pad_to_sectors(kernel, "kernel")
    if kernel_sectors > KERNEL_MAX_SECTORS:
        print(f"error: kernel is {kernel_sectors} sectors, "
              f"limit is {KERNEL_MAX_SECTORS} ({KERNEL_MAX_SECTORS * SECTOR} bytes)",
              file=sys.stderr)
        return 1

    entries = []
    for spec in args.init:
        if "=" not in spec:
            print(f"error: --init expects NAME=PATH, got {spec!r}", file=sys.stderr)
            return 1
        name, path = spec.split("=", 1)
        entries.append((name, path))

    if args.initrd:
        initrd = read(args.initrd)
    elif entries:
        initrd = build_initrd(entries)
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
              f"({initrd_sectors} sectors, {len(entries)} entries)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
