# ext4 Journaling

## Purpose

The ext4 journal (JBD2 — Journaling Block Device 2) ensures filesystem consistency after crashes. Without journaling, a crash during a metadata update (e.g., file creation involves updating the inode bitmap, the directory entry, and the inode itself) can leave the filesystem in an inconsistent state requiring a slow `fsck` scan at next boot.

With journaling, all metadata changes are written to a sequential journal first. If a crash occurs, the journal is replayed at next mount to bring the filesystem to a consistent state. This reduces recovery time from minutes to seconds.

## Journal Location

The journal is stored in a dedicated inode (inode 8 by convention) on the filesystem. The journal occupies a contiguous region of blocks (default: 128 MB).

## Journal Modes

| Mode | What is journaled | Performance | Data safety |
|---|---|---|---|
| `data=journal` | All metadata + data | Slowest | Highest |
| `data=ordered` (default) | Metadata only; data written before metadata | Good | High |
| `data=writeback` | Metadata only; data order not enforced | Fastest | Lower |

Default mode: `data=ordered`. Data blocks are written to disk before the metadata commit, ensuring that committed metadata always points to valid data.

## Journal Transaction Model

Changes to filesystem metadata are grouped into **transactions**. A transaction is:

1. **Started**: A handle is obtained (`jbd2_journal_start`).
2. **Operations**: Metadata blocks are modified in memory. Each modified block is tagged as part of the transaction.
3. **Committed**: The transaction is submitted (`jbd2_journal_commit`). The journal writes a **commit block** followed by all dirty metadata blocks in a single sequential write.
4. **Checkpointed**: After the metadata blocks are written to their final on-disk locations, the journal space is marked free.

## Journal Data Structures

```c
struct journal_header {
    uint32_t magic;        // JBD2_MAGIC = 0xC03B3998
    uint32_t blocktype;    // descriptor, commit, revoke, superblock
    uint32_t sequence;     // transaction sequence number
};

struct journal_superblock {
    struct journal_header header;
    uint32_t blocksize;
    uint32_t maxlen;       // journal length in blocks
    uint32_t first;        // first journal block
    uint32_t sequence;     // next transaction sequence to use
    uint32_t start;        // start of log
    // ...
};
```

## Recovery

On mount, if the filesystem was not cleanly unmounted (the superblock's `s_state` flag is not clean):

1. Read the journal superblock.
2. Scan from `journal.start` for valid transactions (valid magic + matching sequence).
3. Replay each committed transaction: write the journaled metadata blocks to their final locations.
4. Mark the journal clean.
5. Mount proceeds normally.

Recovery is O(journal size), not O(filesystem size).

## Barrier Writes

To ensure write ordering guarantees (prevent reordering by device firmware), the journal uses **write barriers** (cache flush commands) between:

1. Data writes and the journal commit.
2. The journal commit and the checkpoint writes.

On QEMU with virtio-blk, barriers are handled by the virtio driver's flush command.

## Related Documents

- [ext4-overview.md](ext4-overview.md)
- [ext4-extents.md](ext4-extents.md)
- [page-cache.md](page-cache.md)
- [drivers/storage-driver.md](../drivers/storage-driver.md)
