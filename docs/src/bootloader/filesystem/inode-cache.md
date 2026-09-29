# Inode Cache

## Overview

The inode cache (icache) holds `struct inode` objects in memory to avoid repeated disk reads for filesystem metadata. Every file system object that has been recently accessed has its inode in the cache.

## Cache Organization

The inode cache is a hash table indexed by `(superblock *, inode_number)`:

```c
#define ICACHE_HASH_BITS 12
#define ICACHE_HASH_SIZE (1 << ICACHE_HASH_BITS)  // 4096 buckets

struct hlist_head icache_table[ICACHE_HASH_SIZE];
spinlock_t        icache_table_lock;  // one lock per bucket (bucket-granular)
```

Each bucket uses a fine-grained per-bucket spinlock, allowing concurrent access to different inodes without serialization.

Additionally, all unused inodes are tracked in an LRU list for reclamation under memory pressure.

## Inode Lifecycle

```
[disk]                            [icache]
  │                                   │
  │─── iget(sb, ino_number) ──────────►│  allocated + read from disk
  │                                   │  refcount = 1
  │                                   │  (in use, not in LRU)
  │                                   │
  │    iput(inode) ──────────────────►│  refcount-- 
  │                                   │  if refcount == 0: add to LRU tail
  │                                   │  (unused, cached, reclaimable)
  │                                   │
  │    (memory pressure) ────────────►│  if refcount == 0 and LRU eviction:
  │◄── write_inode(inode) ────────────│    write back if dirty
  │◄── free inode ────────────────────│    remove from icache, free memory
```

## `iget` — Lookup or Allocate

```c
struct inode *iget(struct super_block *sb, uint64_t ino);
```

1. Hash `(sb, ino)` → bucket.
2. Lock bucket. Search for an existing inode with matching `(sb, i_ino)`.
3. **Found**: increment `i_refcount`. If it was on the LRU list: remove it. Unlock. Return.
4. **Not found**: allocate a new `struct inode` via the inode slab cache. Set `i_ino = ino`. Call `sb->s_ops->read_inode(inode)` to fill it from disk. Insert into hash table. Unlock. Return.

## `iput` — Release Reference

```c
void iput(struct inode *inode);
```

1. Decrement `i_refcount` (atomic).
2. If `i_refcount > 0`: return (still in use).
3. If `i_refcount == 0`:
   - If `i_links_count == 0` (file deleted while open): call `sb->s_ops->delete_inode()`. Free all data blocks. Free the inode from disk.
   - Else: add to the LRU tail (available for reclamation, but keep in cache for now).

## Inode Dirty Tracking

When an inode's metadata is modified (permissions, timestamps, size), it is marked dirty:

```c
mark_inode_dirty(inode);
```

Dirty inodes are flushed to disk by the writeback daemon or on `fsync`/`sync`. The ext4 filesystem writes dirty inodes through the journal (see [ext4-journaling.md](ext4-journaling.md)).

## Reclamation

The inode cache contributes to kernel memory pressure. Under memory pressure:

1. The page reclamation daemon calls `icache_shrink(count)`.
2. `count` inodes are removed from the LRU head (oldest unused inodes).
3. Dirty inodes are written back before eviction.
4. The inode's associated page cache pages may also be dropped.

## Related Documents

- [dentry-cache.md](dentry-cache.md)
- [page-cache.md](page-cache.md)
- [vfs-layer.md](vfs-layer.md)
- [memory/page-reclamation.md](../memory/page-reclamation.md)
