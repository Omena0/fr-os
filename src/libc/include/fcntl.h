/*
 * fcntl.h — file control.
 *
 * The kernel implements F_GETFL and F_SETFL; F_SETFL only honours O_NONBLOCK.
 * That is enough to put stdin in non-blocking mode for a program that has to
 * keep animating (a game) while it polls for input.
 */
#ifndef FCNTL_H
#define FCNTL_H

#include <uapi/syscall.h>

#ifdef __cplusplus
extern "C" {
#endif

int fcntl(int fd, int cmd, ...);

#ifdef __cplusplus
}
#endif

#endif /* FCNTL_H */
