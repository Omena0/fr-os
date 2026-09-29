# ext4 Extents

## Overview

ext4 uses an **extent tree** to map file logical blocks to physical disk blocks. This replaces the legacy indirect block scheme used by ext2/3, which suffered from performance and fragmentation issues for large files.

An extent is a contiguous range of physical blocks:

```c
struct ext4_extent {
    uint32_t ee_block;     // first logical block covered by this extent
    uint16_t ee_len;       // number of blocks (max 32768)
    uint16_t ee_start_hi;  // high 16 bits of physical block number
    uint32_t ee_start_lo;  // low 32 bits of physical block number
};
```

A single extent covers up to 32768 × 4 KB = 128 MB of contiguous data.

## Extent Tree Structure

The extent tree root is stored in the inode's `i_block[0..14]` field (60 bytes):

```
Inode i_block:
  ┌─── ext4_extent_header ───┐
  │ eh_magic, eh_entries,    │
  │ eh_max, eh_depth, ...    │
  └───────────────────────────┘
  [eh_entries × ext4_extent or ext4_extent_idx]
```

- If `eh_depth == 0`: the inode directly contains extent leaves (actual file→block mappings). This is the common case for small and medium files.
- If `eh_depth > 0`: the inode contains index nodes (`ext4_extent_idx`) pointing to child tree nodes (loaded from disk blocks).

Maximum depth: 4 levels. Supports files up to 2^48 blocks × 4 KB = 1 EB (exabyte).

## Extent Operations

### Lookup

To find the physical block for logical block `lbn`:

1. Start at the extent tree root (in inode `i_block`).
2. If `depth == 0`: binary search the extent array for the extent covering `lbn`. Return `ee_start_lo + (lbn - ee_block)`.
3. If `depth > 0`: binary search the index array for the index entry pointing to the subtree covering `lbn`. Follow the `ei_leaf` pointer (load the block from disk). Repeat.

Binary search at each level: O(log(eh_max)) = O(log(340)) ≈ O(8) for an inode-resident tree.

### Insertion (Block Allocation)

When new blocks are appended to a file:

1. Try to extend the last extent (if new blocks are physically adjacent): just increment `ee_len`. O(1) if the extent tree leaf is in memory.
2. If not adjacent: allocate a new extent. Insert into the tree. If the leaf is full: split the node (creates a new level if the root is also full).

The block allocator prefers to allocate physically contiguous blocks (reducing the number of extents, improving sequential read performance).

### Deletion

When blocks are freed (file truncation or deletion):

1. Find extents covering the deleted range.
2. Split extents at boundaries if partial deletion.
3. Free the physical blocks (return to block group bitmap).
4. Remove extents from the tree.
5. Journal the inode and affected tree nodes.

## Sparse Files

An unmapped logical block range (a "hole") is represented by the absence of an extent covering that range. Reading from a hole returns zeroes without disk access.

```
Logical blocks:  0–99  [extent 1: phys 5000–5099]
                 100–199  [hole — no extent]
                 200–299  [extent 2: phys 7000–7099]

read(offset=400KB): returns 4 KB of zeroes, no disk access
```

## Related Documents

- [ext4-overview.md](ext4-overview.md)
- [ext4-journaling.md](ext4-journaling.md)
- [page-cache.md](page-cache.md)
