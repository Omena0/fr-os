# Shared Memory

## Overview

POSIX shared memory allows two or more processes to share a physical memory region, enabling the fastest possible IPC by eliminating copies entirely. Once the region is mapped, processes communicate by reading and writing to it directly — no syscalls required for the data transfer itself.

## Creation and Lifetime

### Creating a Shared Memory Object

```c
int fd = shm_open("/my_region", O_CREAT | O_RDWR, 0600);
ftruncate(fd, SIZE);
void *ptr = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
```

`shm_open` creates a named object in the SHM namespace (analogous to a file in `/dev/shm/`). The object is backed by anonymous physical pages (not a disk file). `ftruncate` sets its size. `mmap(MAP_SHARED, fd)` maps the object into the calling process's address space.

### Connecting to an Existing Object

```c
int fd = shm_open("/my_region", O_RDWR, 0);
void *ptr = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
```

Both processes now have virtual addresses mapped to the same physical pages. Writes by one are immediately visible to the other (subject to CPU memory ordering).

### Destruction

```c
shm_unlink("/my_region");   // removes the name; object persists until last fd closed
munmap(ptr, SIZE);
close(fd);
```

`shm_unlink` removes the name from the SHM namespace. The underlying pages are freed when all mappings and file descriptors are closed.

## Kernel Implementation

Shared memory objects are implemented as anonymous inodes in a special SHM filesystem (mounted internally at boot, not visible in the user mount tree):

- `shm_open` creates an inode in the SHM filesystem. Returns a file descriptor.
- `ftruncate` allocates the backing pages (via the page allocator) and registers them with the inode's `address_space`.
- `mmap(MAP_SHARED, fd)` creates a VMA in the calling process backed by the SHM inode's `address_space`. The same `address_space` is used for all mappings — all processes see the same physical pages.

## Synchronization

Shared memory provides no built-in synchronization. Processes must coordinate access using:

- **POSIX semaphores** (mapped within the shared region or by name).
- **Futex** (mapped within the shared region for process-shared mode).
- **Atomic operations** (compiler/architecture intrinsics).

Without synchronization, concurrent writes produce undefined results (data races).

## Permissions and Security

Access to a named SHM object is controlled by:

- The mode bits set at creation (`O_CREAT, 0600`).
- Standard UID/GID permission checks on `shm_open`.

A process in a different IPC namespace cannot access SHM objects from another namespace. See [namespace-isolation.md](namespace-isolation.md).

## Userspace Driver Shared Memory

The hybrid driver model uses shared memory as the primary communication channel between the kernel driver core and userspace driver processes. The kernel allocates a shared memory region and maps it into the userspace driver's address space. Command queues and response queues are implemented as ring buffers within this region. See [drivers/userspace-driver-abi.md](../drivers/userspace-driver-abi.md).

## Related Documents

- [synchronization-primitives.md](synchronization-primitives.md)
- [namespace-isolation.md](namespace-isolation.md)
- [drivers/userspace-driver-abi.md](../drivers/userspace-driver-abi.md)
- [memory/virtual-memory.md](../memory/virtual-memory.md)
