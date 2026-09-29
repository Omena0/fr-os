# Dentry Cache

## Overview

The dentry cache (dcache) caches directory entries — the associations between filenames and inodes — to avoid repeated directory traversals and disk reads for path resolution.

Every path lookup (`open`, `stat`, `mkdir`, `rename`, etc.) traverses the dentry tree. Without caching, each component of a path would require a disk read. The dcache makes path resolution O(1) for cached paths.

## Structure

The dcache is a hash table indexed by `(parent_dentry *, filename_hash)`:

```c
#define DCACHE_HASH_BITS 14
#define DCACHE_HASH_SIZE (1 << DCACHE_HASH_BITS)  // 16384 buckets

struct hlist_head dcache_table[DCACHE_HASH_SIZE];
// Per-bucket RCU-protected (reads lock-free, writes use dcache_lock)
```

All unused dentries are also maintained on a global LRU list for reclamation.

## Dentry States

| State | `d_inode` | `d_refcount` | Description |
|---|---|---|---|
| Used | Non-NULL | > 0 | Referenced by an open file or path in use |
| Unused (positive) | Non-NULL | 0 | Cached but no current users; reclaimable |
| Negative | NULL | 0 | Cached non-existence; lookup returned ENOENT |
| In-use root | Non-NULL | > 0 | Root dentry of a mounted filesystem |

Negative dentries cache the fact that a filename does not exist. This avoids repeated disk lookups for names like `/usr/lib/nonexistent.so` that are probed repeatedly.

## Lookup

```c
struct dentry *dcache_lookup(struct dentry *parent, const char *name, uint32_t len);
```

1. Compute hash of `(parent, name, len)`.
2. RCU read lock.
3. Search the hash bucket for a matching dentry (same parent pointer and name).
4. If found: increment refcount, RCU unlock, return.
5. If not found: RCU unlock. Call `parent->d_inode->i_ops->lookup(parent->d_inode, name)`. Allocate and insert a new dentry (positive or negative). Return.

## Insertion and Invalidation

When a file is created: a positive dentry is inserted.
When a file is deleted: the dentry is **invalidated** (set to negative). Any cached positive dentry pointing to the deleted inode is also invalidated.
When a file is renamed: the old dentry is invalidated; a new dentry is inserted for the new name.

Invalidation is propagated to all CPUs via the dcache lock and (for RCU readers) a grace period before the dentry memory is freed.

## RCU-Based Read Path

Dentry cache reads use the RCU (Read-Copy-Update) mechanism:

- Readers: acquire an RCU read lock (just a memory barrier + preemption disable). No spinning, no blocking.
- Writers: hold `dcache_lock` (a global spinlock; future: per-bucket). After modifying, wait for an RCU grace period before freeing old dentries.

This allows concurrent path resolution on all CPUs without lock contention, even on 18+ CPUs.

## Reclamation

The dcache LRU is shrunk under memory pressure. The reclamation path:

1. Remove the dentry from the LRU (oldest unused dentries first).
2. If the dentry has children (it is a directory with cached children): recurse, evicting children first.
3. Decrement the inode's refcount via `iput`.
4. Free the `struct dentry` via the dentry slab cache.

## Mount Point Handling

When a filesystem is mounted on a directory:

- The directory's dentry is marked `DCACHE_MOUNTED`.
- Path traversal through a `DCACHE_MOUNTED` dentry switches to the mounted filesystem's root dentry.
- On unmount: the `DCACHE_MOUNTED` flag is cleared; the original directory's dentry is restored.

## Related Documents

- [inode-cache.md](inode-cache.md)
- [page-cache.md](page-cache.md)
- [vfs-layer.md](vfs-layer.md)
- [mount-system.md](mount-system.md)
