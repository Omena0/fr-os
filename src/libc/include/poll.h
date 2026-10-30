/*
 * poll.h - wait for a set of descriptors.
 */
#ifndef POLL_H
#define POLL_H

#include <uapi/syscall.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long nfds_t;

int poll(struct pollfd *fds, nfds_t nfds, int timeout);

#ifdef __cplusplus
}
#endif

#endif /* POLL_H */
