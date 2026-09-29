"""
 * initrd.py — build the userspace initrd.
 *
 * The format is a flat container, not a filesystem. The kernel copies the blob
 * into memory and walks the index; there is no superblock, no directory tree
 * and no allocation, because at this stage the only consumer is the loader,
 * which wants the whole of each program's image and nothing else.
 *
 * Making this a real filesystem would buy nothing today and cost a filesystem
 * driver in the kernel before there is a block layer to hang it off. When the
 * ramfs lands it can mount this by reading the same index.
 *
 * Layout:
 *
 *   struct initrd_header   16 bytes
 *   struct initrd_entry[]  variable: name_len(u16), mode(u16),
 *                          size(u64), offset(u64), then name_len bytes of name
 *   payload                the concatenated file contents
"""

import argparse
import struct
import sys

INITRD_MAGIC = 0x4F53425255444E44  # "OSRBUNDND"
INITRD_VERSION = 1
INITRD_HEADER_FMT = "<QIIQ"  # magic, version, flags, total_len
INITRD_HEADER_SIZE = struct.calcsize(INITRD_HEADER_FMT)
ENTRY_FMT = "<HHQQ"  # name_len, mode, size, offset


def build(entries: list[tuple[str, str, int]]) -> bytes:
    payload = bytearray()
    index = bytearray()

    for name, path, mode in entries:
        with open(path, "rb") as fh:
            data = fh.read()
        if not data:
            print(f"error: {path} is empty", file=sys.stderr)
            raise SystemExit(1)

        name_bytes = name.encode()
        if len(name_bytes) > 0xFFFF:
            print(f"error: name too long: {name}", file=sys.stderr)
            raise SystemExit(1)

        offset = len(payload)
        payload.extend(data)
        index.extend(struct.pack(ENTRY_FMT, len(name_bytes), mode, len(data), offset))
        index.extend(name_bytes)

    total = INITRD_HEADER_SIZE + len(index) + len(payload)
    header = struct.pack(INITRD_HEADER_FMT, INITRD_MAGIC, INITRD_VERSION, 0, total)
    return header + bytes(index) + bytes(payload)


def main() -> int:
    ap = argparse.ArgumentParser(description="build the userspace initrd")
    ap.add_argument("--out", required=True)
    ap.add_argument("--program", action="append", default=[], required=True,
                    metavar="NAME[:MODE]=PATH")
    args = ap.parse_args()

    entries = []
    for spec in args.program:
        if "=" not in spec:
            print(f"error: --program expects NAME[:MODE]=PATH, got {spec!r}",
                  file=sys.stderr)
            return 1
        left, path = spec.split("=", 1)
        if ":" in left:
            name, mode_s = left.split(":", 1)
            mode = int(mode_s, 8)
        else:
            name, mode = left, 0o755
        entries.append((name, path, mode))

    blob = build(entries)
    with open(args.out, "wb") as fh:
        fh.write(blob)

    print(f"  INITRD {args.out} ({len(blob)} bytes, {len(entries)} program(s))")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
