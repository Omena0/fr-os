/*
 * poll.c - poll(2) over SYS_poll.
 */
#include <poll.h>

extern int sys_poll(struct pollfd *fds, unsigned long nfds, int timeout);

int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    return sys_poll(fds, nfds, timeout);
}
