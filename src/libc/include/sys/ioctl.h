/*
 * sys/ioctl.h — terminal ioctls.
 *
 * The ioctl request macros and the winsize struct come straight from the
 * kernel ABI; the libc side only needs to know the struct layout to fill it
 * in before calling TIOCGWINSZ.
 */
#ifndef SYS_IOCTL_H
#define SYS_IOCTL_H

#include <uapi/syscall.h>

#ifdef __cplusplus
extern "C" {
#endif

int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif

#endif /* SYS_IOCTL_H */