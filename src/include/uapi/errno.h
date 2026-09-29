/*
 * errno.h — POSIX error numbers shared by the kernel and libc.
 *
 * Values are the POSIX.1-2017 assignments, which are also the de facto Linux
 * values, so ported programs get the behaviour they expect. The kernel returns
 * -errno through the syscall ABI; libc converts that back into a positive errno
 * and sets the thread-local errno.
 */
#ifndef UAPI_ERRNO_H
#define UAPI_ERRNO_H

#define EPERM    1   /* Operation not permitted */
#define ENOENT   2   /* No such file or directory */
#define ESRCH    3   /* No such process */
#define EINTR    4   /* Interrupted system call */
#define EIO      5   /* Input/output error */
#define ENXIO    6   /* No such device or address */
#define E2BIG    7   /* Argument list too long */
#define ENOEXEC  8   /* Exec format error */
#define EBADF    9   /* Bad file descriptor */
#define ECHILD  10   /* No child processes */
#define EAGAIN  11   /* Resource temporarily unavailable */
#define ENOMEM  12   /* Cannot allocate memory */
#define EACCES  13   /* Permission denied */
#define EFAULT  14   /* Bad address */
#define ENOTBLK 15   /* Block device required */
#define EBUSY   16   /* Device or resource busy */
#define EEXIST  17   /* File exists */
#define EXDEV   18   /* Invalid cross-device link */
#define ENODEV  19   /* No such device */
#define ENOTDIR 20   /* Not a directory */
#define EISDIR  21   /* Is a directory */
#define EINVAL  22   /* Invalid argument */
#define ENFILE  23   /* Too many open files in system */
#define EMFILE  24   /* Too many open files */
#define ENOTTY  25   /* Inappropriate ioctl for device */
#define ETXTBSY 26   /* Text file busy */
#define EFBIG   27   /* File too large */
#define ENOSPC  28   /* No space left on device */
#define ESPIPE  29   /* Illegal seek */
#define EROFS   30   /* Read-only file system */
#define EMLINK  31   /* Too many links */
#define EPIPE   32   /* Broken pipe */
#define EDOM    33   /* Mathematical argument out of domain of func */
#define ERANGE  34   /* Mathematical result not representable */
#define ENAMETOOLONG 36
#define ENOSYS  38   /* Function not implemented */
#define ENOTEMPTY   39
#define ELOOP   40   /* Too many levels of symbolic links */
#define ENODATA 61   /* No data available */
#define EPROTONOSUPPORT 93
#define EAFNOSUPPORT     97
#define ECONNRESET      104
#define ENOBUFS         105
#define EADDRINUSE      98

#endif /* UAPI_ERRNO_H */
