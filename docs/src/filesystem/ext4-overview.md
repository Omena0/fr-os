# ext4 Overview

## Design

The OS implements a feature-complete subset of the ext4 filesystem. ext4 is used as the primary on-disk filesystem for the root partition and all data volumes.

## Supported Features

| Feature | Status | Notes |
|---|---|---|
| Journaling (metadata) | Full | Journal-based crash recovery |
| Extents | Full | Replaces legacy indirect blocks |
| HTree (large directories) | Full | B-tree directory indexing |
| Large files (> 2 GB) | Full | 64-bit file sizes |
| Extended attributes | Partial | In-inode xattrs; separate xattr block |
| Inline data | Planned | Small files still use a data block |
| Sparse files | Full | Holes supported via extent tree |
| Timestamps (nanosecond) | Full | |
| `dir_index` (HTree) | Full | |
| `64bit` feature flag | Full | Required for large volumes |
| `flex_bg` | Full | Flexible block groups |
| `has_journal` | Full | |
| Encryption (`encrypt`) | Not implemented | Future milestone |
| Checksums (`metadata_csum`) | Partial | Superblock and group descriptor checksums only |

## On-Disk Layout

```
Block 0:           Boot sector (unused for ext4)
Block 1:           Superblock (1024-byte offset, 1 KB)
Block 2:           Group descriptor table
Block groups:
  Each group contains:
    - Group descriptor (in the descriptor table)
    - Block bitmap (tracks free/used blocks in this group)
    - Inode bitmap (tracks free/used inodes in this group)
    - Inode table (array of struct ext4_inode)
    - Data blocks
```

The superblock is the authoritative source for filesystem parameters: block size, inode count, block count, journal inode number, feature flags, and UUID.

## Block and Inode Sizes

- Block size: 4 KB (default; configurable at mkfs time as 1 KB, 2 KB, 4 KB).
- Inode size: 256 bytes (ext4 standard; allows inline xattrs in the extra space).

## Inode Structure

```c
struct ext4_inode {
    uint16_t i_mode;         // file type and permissions
    uint16_t i_uid;          // lower 16 bits of owner UID
    uint32_t i_size_lo;      // lower 32 bits of file size
    uint32_t i_atime, i_ctime, i_mtime, i_dtime; // timestamps
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks_lo;    // lower 32 bits of block count
    uint32_t i_flags;        // EXT4_EXTENTS_FL, etc.
    uint32_t i_block[15];    // extent tree root (when EXT4_EXTENTS_FL) or direct/indirect blocks
    // ... additional fields for ext4 features
    uint32_t i_uid_high, i_gid_high;  // upper 16 bits
    uint32_t i_size_high;
};
```

## Registration with VFS

ext4 registers as a filesystem type with VFS:

```c
static struct filesystem_type ext4_fs_type = {
    .name = "ext4",
    .ops  = &ext4_fs_ops,
};

void ext4_init(void) {
    vfs_register_filesystem(&ext4_fs_type);
}
```

`ext4_fs_ops.mount()` reads the superblock, validates the magic number (`0xEF53`), and initializes the in-memory superblock representation.

## Related Documents

- [ext4-journaling.md](ext4-journaling.md)
- [ext4-extents.md](ext4-extents.md)
- [vfs-layer.md](vfs-layer.md)
- [filesystem/overview.md](overview.md)
