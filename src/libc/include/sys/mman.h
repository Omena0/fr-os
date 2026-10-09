/*
 * sys/mman.h — memory mapping.
 *
 * mmap is anonymous-only on this kernel: MAP_FIXED is honoured but there is
 * no file-backed mapping. Allocations larger than 128 KiB go straight to
 * mmap/munmap in the allocator, so the protection and flag constants here are
 * the ones the allocator actually uses.
 */
#ifndef SYS_MMAN_H
#define SYS_MMAN_H

#include <stdint.h>
#include <stddef.h>
#include <uapi/syscall.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAP_FAILED ((void *)-1)

#define MADV_NORMAL   0
#define MADV_RANDOM   1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED 3
#define MADV_DONTNEED 4

void *mmap(void *addr, size_t length, int prot, int flags, int fd,
       off_t offset);
int   munmap(void *addr, size_t length);
int   mprotect(void *addr, size_t len, int prot);

#ifdef __cplusplus
}
#endif

#endif /* SYS_MMAN_H */