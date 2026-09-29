# Pipes

## Overview

Pipes provide a unidirectional byte stream between two processes. They are the simplest and most commonly used IPC mechanism in Unix systems.

## Two Types

| Type | Creation | Filesystem presence |
|---|---|---|
| Anonymous pipe | `pipe(fds)` syscall | None — only between related processes (parent/child after `fork`) |
| Named pipe (FIFO) | `mkfifo(path, mode)` + `open()` | Entry in filesystem; any process can open by path |

## Ring Buffer Implementation

Each pipe has a kernel-allocated ring buffer:

```c
#define PIPE_BUFFER_SIZE (64 * 1024)   // 64 KB default

struct pipe {
    uint8_t     buf[PIPE_BUFFER_SIZE]; // ring buffer data
    uint32_t    head;                  // write position
    uint32_t    tail;                  // read position
    uint32_t    count;                 // bytes currently in buffer
    atomic_t    readers;               // number of open read ends
    atomic_t    writers;               // number of open write ends
    spinlock_t  lock;                  // protects head/tail/count
    wait_queue_t reader_wait;          // waiting readers (blocked on empty)
    wait_queue_t writer_wait;          // waiting writers (blocked on full)
};
```

## Write Path

`write(write_fd, buf, len)`:

1. Acquire pipe lock.
2. Available space = `PIPE_BUFFER_SIZE - count`.
3. If space available: copy `min(len, space)` bytes from `buf` into the ring buffer at `head`. Advance `head`. Increment `count`. Release lock.
4. If pipe is full: release lock. Block on `writer_wait` until space becomes available.
5. Wake any sleeping readers (`reader_wait`).
6. Return bytes written.

**Atomicity guarantee**: A write of ≤ `PIPE_BUF` (4096 bytes, POSIX minimum) is atomic — it either fully succeeds or blocks until it can complete without interleaving with other writers.

## Read Path

`read(read_fd, buf, len)`:

1. Acquire pipe lock.
2. If `count > 0`: copy `min(len, count)` bytes from `tail`. Advance `tail`. Decrement `count`. Release lock.
3. If pipe is empty:
   - If no writers (write end closed): return 0 (EOF).
   - Else: release lock. Block on `reader_wait`.
4. Wake any sleeping writers (`writer_wait`).
5. Return bytes read.

## End-of-File and SIGPIPE

- **EOF on read**: When all write ends of a pipe are closed (`writers == 0`) and the buffer is empty: `read` returns 0.
- **SIGPIPE on write**: When all read ends are closed (`readers == 0`): `write` sends `SIGPIPE` to the writing process (default action: terminate) and returns `-EPIPE` if the signal is caught or ignored.

## Zero-Copy Optimization (Same-Process)

For pipes where both ends are in the same process (e.g., a thread writes while another reads), a zero-copy path using page remapping is possible:

- Instead of copying bytes into the ring buffer, remap the source page directly as the pipe buffer.
- Receiver reads from the remapped page.
- This is activated when the write is page-aligned and page-sized.

This optimization is activated when the writer calls `vmsplice(write_fd, iov, ...)` (a planned feature).

## Named Pipes (FIFOs)

Named pipes are created by `mkfifo(path, mode)`, which creates a special inode in the filesystem (type `S_IFIFO`). Opening a FIFO with `O_RDONLY` blocks until a writer opens the other end (and vice versa), unless `O_NONBLOCK` is set.

Once both ends are open, named pipes behave identically to anonymous pipes with the same ring buffer mechanism.

## Related Documents

- [fd-abstraction.md](fd-abstraction.md)
- [unix-sockets.md](unix-sockets.md)
- [ipc/overview.md](overview.md)
- [filesystem/vfs-api.md](../filesystem/vfs-api.md)
