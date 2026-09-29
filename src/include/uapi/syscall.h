/*
 * uapi/syscall.h — the userspace syscall ABI.
 *
 * This header is the single source of truth for the kernel/userspace contract.
 * It is compiled into both the kernel dispatcher and libc, so a change to a
 * number, a signature, or a struct layout cannot land on one side only.
 *
 * Stability rules, enforced by convention here and by the numbering scheme:
 *
 *   - A number is assigned once and never reused, even if the syscall is
 *     deprecated.
 *   - Numbers are grouped by domain in fixed ranges. A gap inside a range is
 *     either a reserved or an unassigned number, never a reallocation.
 *   - Signatures are only ever extended by appending arguments or by adding
 *     flag bits, so a caller compiled against version 1.0 keeps working
 *     against a 1.N kernel.
 *   - Structs that cross the boundary carry an explicit `size` field so the
 *     kernel can accept both older and newer layouts.
 *
 * See docs/architecture/abi-stability.md for the full policy.
 */
#ifndef UAPI_SYSCALL_H
#define UAPI_SYSCALL_H

#include <stdint.h>
#include <stddef.h>

/*
 * ABI version, encoded as MAJOR * 1000 + MINOR. A MAJOR bump permits breaking
 * changes and activates the compat dispatch table; a MINOR bump is additive.
 * The kernel publishes this to new processes through the ELF auxiliary vector
 * (AT_ABI_VERSION) and libc compares it at startup.
 */
#define KERNEL_ABI_VERSION 1000u
#define KERNEL_ABI_MAJOR   (KERNEL_ABI_VERSION / 1000u)
#define KERNEL_ABI_MINOR   (KERNEL_ABI_VERSION % 1000u)

/* --------------------------------------------------------------- numbers --- */
/*
 * The ranges are fixed. Each domain gets 32 numbers at a time so a domain can
 * grow without renumbering anything, and so a bad number from a confused
 * caller lands in an unmapped range and returns -ENOSYS rather than invoking
 * an unrelated handler.
 */
#define SYSCALL_DOMAIN_PROCESS     0u     /*   0 -  31 */
#define SYSCALL_DOMAIN_MEMORY      32u    /*  32 -  63 */
#define SYSCALL_DOMAIN_FILE        64u    /*  64 - 127 */
#define SYSCALL_DOMAIN_IPC        128u    /* 128 - 159 */
#define SYSCALL_DOMAIN_SOCKET     160u    /* 160 - 199 */
#define SYSCALL_DOMAIN_SCHED      200u    /* 200 - 231 */
#define SYSCALL_DOMAIN_SECURITY   256u    /* 256 - 287 */

/* Process management. */
#define SYS_fork          0u
#define SYS_execve        1u
#define SYS_exit          2u
#define SYS_exit_group    3u
#define SYS_wait4         4u
#define SYS_getpid        5u
#define SYS_getppid       6u
#define SYS_gettid        7u
#define SYS_clone         8u
#define SYS_kill          9u
#define SYS_getpgid      10u
#define SYS_setpgid      11u
#define SYS_setsid       12u
#define SYS_nanosleep    13u
#define SYS_clock_gettime 14u
#define SYS_getuid       15u
#define SYS_getgid       16u

/* Memory management. */
#define SYS_mmap         32u
#define SYS_munmap       33u
#define SYS_mprotect     34u
#define SYS_brk          35u
#define SYS_madvise      36u

/* Files. */
#define SYS_open         64u
#define SYS_close        65u
#define SYS_read         66u
#define SYS_write        67u
#define SYS_lseek        68u
#define SYS_fstat        69u
#define SYS_ioctl        70u
#define SYS_getdents     71u
#define SYS_dup          72u
#define SYS_fcntl        73u

/* IPC. */
#define SYS_pipe        128u
#define SYS_pipe2       129u
#define SYS_futex       130u

/* Sockets. */
#define SYS_socket      160u

/* Scheduling. */
#define SYS_sched_yield 200u
#define SYS_getcpu      201u

/* Highest number the kernel will accept before returning -ENOSYS without
 * consulting the dispatch table. Bounds-checking this is what keeps a garbage
 * RAX from becoming an arbitrary function-pointer call. */
#define SYSCALL_MAX     288u

/* ------------------------------------------------------------- structures -- */

struct timespec {
	int64_t tv_sec;
	int64_t tv_nsec;
};

struct timeval {
	int64_t tv_sec;
	int64_t tv_usec;
};

/* Enough for the stat fields this kernel actually maintains, with `size` so a
 * future libc can pass a larger struct and have the kernel fill what it knows
 * without a new syscall number. */
struct kstat {
	uint64_t size;        /* total size in bytes */
	uint64_t blksize;     /* preferred I/O block size */
	uint64_t blocks;      /* blocks occupied (512-byte units) */
	uint64_t ino;         /* inode number */
	uint32_t mode;        /* file type and permission bits */
	uint32_t nlink;       /* hard link count */
	uint32_t uid;         /* owning user */
	uint32_t gid;         /* owning group */
	int64_t  atime_sec;
	uint64_t atime_nsec;
	int64_t  mtime_sec;
	uint64_t mtime_nsec;
	int64_t  ctime_sec;
	uint64_t ctime_nsec;
};

/* Directory entry returned by SYS_getdents. `d_off` is the cookie to pass back
 * to resume iteration, matching the POSIX readdir contract rather than
 * requiring a cursor object. */
struct kdirent {
	uint64_t d_ino;
	int64_t  d_off;
	uint16_t d_reclen;
	uint8_t  d_type;
	uint8_t  d_namlen;
	char     d_name[];
};

/* clone(2) flags. Kept compatible with the widely-deployed Linux values so
 * ported pthreads implementations work unmodified. */
#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

/* mmap protection bits. Values match the POSIX/Linux assignment. */
#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4

/* mmap flags. */
#define MAP_SHARED            0x01
#define MAP_PRIVATE           0x02
#define MAP_SHARED_VALIDATE   0x03
#define MAP_FIXED             0x10
#define MAP_ANONYMOUS         0x20
#define MAP_HUGETLB           0x40000
#define MAP_GROWSDOWN         0x0100
#define MAP_POPULATE          0x8000
#define MAP_STACK             0x20000

/* open(2) flags. */
#define O_RDONLY    0x0
#define O_WRONLY    0x1
#define O_RDWR      0x2
#define O_ACCMODE   0x3
#define O_CREAT     0x40
#define O_EXCL      0x80
#define O_TRUNC     0x200
#define O_APPEND    0x400
#define O_NONBLOCK  0x800
#define O_DIRECTORY 0x10000
#define O_CLOEXEC   0x80000

/* ioctl request encoding: direction, size, type, number. */
#define IOC_NRBITS   8
#define IOC_TYPEBITS 8
#define IOC_SIZEBITS 14

#define IOC_NRSHIFT   0
#define IOC_TYPESHIFT (IOC_NRSHIFT + IOC_NRBITS)
#define IOC_SIZESHIFT (IOC_TYPESHIFT + IOC_TYPEBITS)
#define IOC_DIRSHIFT  (IOC_SIZESHIFT + IOC_SIZEBITS)

#define IOC_NONE  0U
#define IOC_WRITE 1U
#define IOC_READ  2U

#define _IOC(dir, type, nr, size) \
	(((dir) << IOC_DIRSHIFT) | ((type) << IOC_TYPESHIFT) | \
	 ((nr) << IOC_NRSHIFT) | ((size) << IOC_SIZESHIFT))

#define _IOR(type, nr, size) _IOC(IOC_READ,  type, nr, sizeof(size))
#define _IOW(type, nr, size) _IOC(IOC_WRITE, type, nr, sizeof(size))
#define _IOWR(type, nr, size) _IOC(IOC_READ | IOC_WRITE, type, nr, sizeof(size))

/* Terminal ioctls, matching the widely-used Linux numbers so existing
 * terminal libraries work. */
#define TIOCGWINSZ   _IOR('t', 104, struct winsize)
#define TIOCSWINSZ   _IOW('t', 103, struct winsize)
#define TCGETS       _IOR('t', 16, struct termios)

struct winsize {
	uint16_t ws_row, ws_col;
	uint16_t ws_xpixel, ws_ypixel;
};

struct termios {
	uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
	uint8_t  c_line;
	uint8_t  c_cc[32];
};

/* futex operations. */
#define FUTEX_WAIT            0u
#define FUTEX_WAKE            1u
#define FUTEX_PRIVATE_FLAG    128u
#define FUTEX_WAIT_PRIVATE    (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_PRIVATE    (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)

/* Auxiliary vector entries published to new processes. */
#define AT_NULL   0u
#define AT_IGNORE 1u
#define AT_EXECFD 2u
#define AT_PHDR   3u
#define AT_PHNUM  4u
#define AT_PAGESZ 6u
#define AT_BASE   7u
#define AT_ENTRY  9u
#define AT_UID    11u
#define AT_EUID   12u
#define AT_GID    13u
#define AT_RANDOM 25u
#define AT_ABI_VERSION 31u

#endif /* UAPI_SYSCALL_H */
