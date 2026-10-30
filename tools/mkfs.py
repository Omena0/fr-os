#!/usr/bin/env python3
"""
mkfs.py - build a FrFS image from a directory tree.

FrFS is this kernel's on-disk filesystem: 4 KiB blocks, a superblock in block 0,
inode and block bitmaps, a fixed inode table and then data blocks. It is
deliberately small enough to read and write from the kernel without a page
cache, and it is the first filesystem a real VFS is built on. The ext4 subset
described in docs/src/filesystem is the eventual target; the block layer and
VFS interfaces here are shared with it.

The image is packed from whatever is in the source directory. Files added there
by hand are included and nothing is deleted, so a build can install its own
executables without disturbing them.

Layout:
    block 0            superblock
    block 1            inode bitmap
    block 2            block bitmap
    block 3..          inode table (inode_size 128)
    data_block_start.. file and directory data

Inode 1 is the root directory. Files larger than 12 blocks use one single
indirect block (1024 block pointers, so 4 MiB). Directories use direct blocks
only.
"""

import argparse
import os
import struct
import sys

BLOCK_SIZE = 4096
MAGIC = 0x53465246          # "FRFS" little-endian
VERSION = 1
INODE_SIZE = 128
INODE_COUNT = 1024
ROOT_INO = 1
MAX_FILE_BLOCKS = 12 + 1024

S_IFDIR = 0o040000
S_IFREG = 0o100000

DIRENT_TYPE_REG = 1
DIRENT_TYPE_DIR = 2


def align_up(n, a):
    return (n + a - 1) // a * a


class Node:
    def __init__(self, name, path, is_dir):
        self.name = name
        self.path = path
        self.is_dir = is_dir
        self.ino = 0
        self.size = 0
        self.blocks = []          # data block numbers, in order
        self.indirect = 0
        self.children = []        # Node, sorted by name
        self.dir_bytes = b""      # for directories


def scan(path, name):
    st = os.stat(path)
    is_dir = os.path.isdir(path)
    node = Node(name, path, is_dir)
    node.size = 0 if is_dir else st.st_size
    if is_dir:
        entries = sorted(os.listdir(path))
        for entry in entries:
            if child := scan(os.path.join(path, entry), entry):
                node.children.append(child)

    elif name == '.gitkeep': # Ignore .gitkeep files
        return None

    return node


def assign_inodes(node, next_ino):
    node.ino = next_ino
    next_ino += 1
    for child in node.children:
        next_ino = assign_inodes(child, next_ino)
    return next_ino


def dirent(ino, name, typ):
    raw = name.encode("utf-8")
    if len(raw) > 255:
        raise SystemExit(f"mkfs: name too long: {name}")
    rec_len = align_up(4 + 2 + 1 + 1 + len(raw), 4)
    return struct.pack("<IHBB", ino, rec_len, len(raw), typ) + raw


def build_dir_bytes(node):
    """Directory entries, with a record split across a block boundary padded."""
    raw_entries = [(node.ino, DIRENT_TYPE_DIR, b"."),
                   (node.ino, DIRENT_TYPE_DIR, b"..")]
    for child in node.children:
        typ = DIRENT_TYPE_DIR if child.is_dir else DIRENT_TYPE_REG
        raw_entries.append((child.ino, typ, child.name.encode("utf-8")))

    out = bytearray()
    for ino, typ, name in raw_entries:
        need = align_up(8 + len(name), 4)
        remaining = BLOCK_SIZE - (len(out) % BLOCK_SIZE)
        if need > remaining:
            # Extend the previous record to the end of this block.
            prev = out.rfind(struct.pack("<H", 0))  # not reliable; recompute
            # Instead: pad with an empty record that fills the block.
            out += struct.pack("<IHBB", 0, remaining, 0, 0)
        rec_len = align_up(8 + len(name), 4)
        out += struct.pack("<IHBB", ino, rec_len, len(name), typ) + name
        out += b"\0" * (rec_len - 8 - len(name))

    # The last record in the final block may absorb the trailing space.
    if out:
        last_block_start = (len(out) - 1) // BLOCK_SIZE * BLOCK_SIZE
        rec_off = out.rfind(struct.pack("<H", 0))  # unused
        # Find the last record start by walking the final block.
        pos = last_block_start
        last_start = pos
        while pos < len(out):
            ino, rec_len, name_len, typ = struct.unpack_from("<IHBB", out, pos)
            if rec_len == 0:
                break
            last_start = pos
            pos += rec_len
        ino, rec_len, name_len, typ = struct.unpack_from("<IHBB", out, last_start)
        fill = last_block_start + BLOCK_SIZE - last_start
        struct.pack_into("<H", out, last_start + 4, fill)
    return bytes(out)


def set_parent_entries(node, parent_ino):
    if not node.is_dir:
        return
    node.dir_bytes = build_dir_bytes(node)
    # fix ".." inode in every block where it appears (first record of block 0)
    if parent_ino:
        raw = bytearray(node.dir_bytes)
        first_rec = struct.unpack_from("<H", raw, 4)[0]
        raw[first_rec:first_rec + 4] = struct.pack("<I", parent_ino)
        node.dir_bytes = bytes(raw)
    for child in node.children:
        set_parent_entries(child, node.ino)


def main():
    ap = argparse.ArgumentParser(description="build a FrFS image")
    ap.add_argument("source", help="directory to pack")
    ap.add_argument("--out", required=True, help="image to write")
    ap.add_argument("--size-mb", type=int, default=64, help="image size (default 64)")
    args = ap.parse_args()

    if not os.path.isdir(args.source):
        raise SystemExit(f"mkfs: {args.source} is not a directory")

    root = scan(args.source, "")
    next_ino = assign_inodes(root, ROOT_INO)
    inode_count = INODE_COUNT
    if next_ino > inode_count:
        raise SystemExit(f"mkfs: {next_ino - 1} inodes needed, table holds {inode_count}")

    total_blocks = args.size_mb * 1024 * 1024 // BLOCK_SIZE
    inode_table_blocks = align_up(inode_count * INODE_SIZE, BLOCK_SIZE) // BLOCK_SIZE
    inode_bitmap_block = 1
    block_bitmap_block = 2
    inode_table_block = 3
    data_start = inode_table_block + inode_table_blocks

    set_parent_entries(root, ROOT_INO)

    nodes = []

    def collect(n):
        nodes.append(n)
        for c in n.children:
            collect(c)

    collect(root)

    next_block = data_start
    for n in nodes:
        if n.is_dir:
            count = max(1, align_up(len(n.dir_bytes), BLOCK_SIZE) // BLOCK_SIZE)
            n.blocks = list(range(next_block, next_block + count))
            next_block += count
        elif n.size:
            count = align_up(n.size, BLOCK_SIZE) // BLOCK_SIZE
            if count > MAX_FILE_BLOCKS:
                raise SystemExit(f"mkfs: {n.path} is too large ({n.size} bytes)")
            n.blocks = list(range(next_block, next_block + count))
            next_block += count
            if count > 12:
                n.indirect = next_block
                next_block += 1

    if next_block > total_blocks:
        raise SystemExit(
            f"mkfs: needs {next_block} blocks, image holds {total_blocks}; "
            f"raise --size-mb")

    image = bytearray(total_blocks * BLOCK_SIZE)

    def put_block(block, data):
        if len(data) > BLOCK_SIZE:
            raise SystemExit("mkfs: internal: block overflow")
        off = block * BLOCK_SIZE
        image[off:off + len(data)] = data

    # Superblock.
    sb = struct.pack(
        "<IIIIIIIIIII I Q",
        MAGIC, VERSION, BLOCK_SIZE, total_blocks, inode_count, INODE_SIZE,
        inode_table_block, inode_bitmap_block, block_bitmap_block,
        data_start, ROOT_INO, 0, 0)
    put_block(0, sb)

    # Bitmaps. All metadata blocks are used before data starts.
    inode_bitmap = bytearray(BLOCK_SIZE)
    for n in nodes:
        inode_bitmap[n.ino // 8] |= 1 << (n.ino % 8)
    put_block(inode_bitmap_block, inode_bitmap)

    block_bitmap = bytearray(BLOCK_SIZE)
    for b in range(data_start):
        block_bitmap[b // 8] |= 1 << (b % 8)
    for n in nodes:
        for b in n.blocks:
            block_bitmap[b // 8] |= 1 << (b % 8)
        if n.indirect:
            block_bitmap[n.indirect // 8] |= 1 << (n.indirect % 8)
    put_block(block_bitmap_block, block_bitmap)

    # Data and directory contents.
    for n in nodes:
        if n.is_dir:
            for i, b in enumerate(n.blocks):
                chunk = n.dir_bytes[i * BLOCK_SIZE:(i + 1) * BLOCK_SIZE]
                put_block(b, chunk)
        elif n.size:
            with open(n.path, "rb") as f:
                data = f.read()
            if len(data) != n.size:
                raise SystemExit(f"mkfs: {n.path} changed size while packing")
            for i, b in enumerate(n.blocks):
                put_block(b, data[i * BLOCK_SIZE:(i + 1) * BLOCK_SIZE])
            if n.indirect:
                ptrs = struct.pack("<%dI" % len(n.blocks[12:]),
                                   *n.blocks[12:])
                put_block(n.indirect, ptrs)

    # Inode table.
    for n in nodes:
        direct = list(n.blocks[:12])
        direct += [0] * (12 - len(direct))
        mode = (S_IFDIR if n.is_dir else S_IFREG) | (0o755 if n.is_dir else 0o644)
        size = len(n.dir_bytes) if n.is_dir else n.size
        blocks512 = (len(n.blocks) + (1 if n.indirect else 0)) * (BLOCK_SIZE // 512)
        inode = struct.pack(
            "<HHIIQQII12II10I",
            mode, 2 if n.is_dir else 1, 0, 0, size, blocks512,
            0, 0, *direct, n.indirect, *([0] * 10))
        off = inode_table_block * BLOCK_SIZE + (n.ino - 1) * INODE_SIZE
        image[off:off + INODE_SIZE] = inode

    with open(args.out, "wb") as f:
        f.write(image)

    used = next_block * BLOCK_SIZE
    print(f"mkfs: {args.out}: {total_blocks} blocks, {len(nodes)} inodes, "
          f"{used // 1024} KiB used of {len(image) // 1024} KiB")


if __name__ == "__main__":
    main()
