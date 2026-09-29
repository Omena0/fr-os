# Page Cache

## Overview

The page cache is the primary caching layer for file data. It stores 4 KB pages of file content in memory, reducing disk I/O by serving repeated reads from RAM.

## Structure

The page cache is indexed by `(inode *, page_offset)`:

```c
struct address_space {
    struct inode    *host;         // owning inode
    struct radix_tree_root pages;  // radix tree: page_offset → struct page *
    spinlock_t       lock;
    uint64_t         nrpages;      // number of pages currently cached
    uint64_t         nrdirty;      // number of dirty pages
    const struct address_space_ops *a_ops;  // readpage, writepage, etc.
};
```

Each inode has an `address_space`. The radix tree enables O(log N) lookup by page offset. In practice, for sequential access, the radix tree provides O(1) access to the next page via a cursor.

## Read Path

On `file_ops.read()`:

1. For each 4 KB chunk of the read range:
   a. Look up the page in `inode->i_mapping->pages` (radix tree).
   b. **Cache hit**: copy from the cached page to user buffer via `copy_to_user`. Advance offset.
   c. **Cache miss**: allocate a new `struct page`. Call `address_space_ops.readpage(page)` to fill it from disk (synchronous block I/O). Once the page is filled, cache it and copy to user.

## Write Path (Writeback)

On `file_ops.write()`:

1. For each 4 KB chunk:
   a. Find or allocate the page in the page cache.
   b. Copy user data into the page via `copy_from_user`.
   c. Mark the page **dirty** (`SetPageDirty`).
2. The write returns immediately (asynchronous — data is in cache, not yet on disk).

Dirty pages are written back to disk by:

- The **writeback daemon** (`kwritebackd`), which periodically flushes dirty pages older than `dirty_expire_secs` (default: 30 s) and limits dirty page ratio to `dirty_ratio` (default: 20% of RAM).
- `fsync(fd)`: forces immediate writeback of all dirty pages for a specific file.
- `sync()`: forces writeback of all dirty pages system-wide.

## Read-Ahead

For sequential access patterns, the page cache proactively reads ahead:

- When a page N is read, the read-ahead engine submits I/O for pages N+1 through N+K (where K is dynamically tuned, default up to 32 pages = 128 KB).
- Read-ahead pages are loaded asynchronously. If the process accesses them next, they are already in cache.
- Read-ahead is disabled for random access patterns (detected when reads skip many pages).

## Page Cache Reclamation

Under memory pressure, clean pages (not dirty, not locked) are reclaimed first:

1. Remove from the radix tree.
2. Return the physical page to the buddy allocator.

On the next access, the data is re-read from disk (cache miss). Dirty pages must be written back before they can be reclaimed.

## Fast Path

Sequential read (all pages cached):

- Per-page: radix tree lookup (O(log N)) + `copy_to_user`.
- No disk I/O. No locks except a brief spinlock for the radix tree read.
- Throughput: limited by memory bandwidth and `copy_to_user` speed.

## Related Documents

- [inode-cache.md](inode-cache.md)
- [dentry-cache.md](dentry-cache.md)
- [vfs-api.md](vfs-api.md)
- [ext4-journaling.md](ext4-journaling.md)
- [memory/page-reclamation.md](../memory/page-reclamation.md)
