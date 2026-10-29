/*
 * termios.c - tcgetattr and tcsetattr over the terminal ioctls.
 */
#include <termios.h>
#include <errno.h>

extern int sys_ioctl(int fd, unsigned long request, ...);

int tcgetattr(int fd, struct termios *t)
{
    return sys_ioctl(fd, TCGETS, t);
}

int tcsetattr(int fd, int optional_actions, const struct termios *t)
{
    unsigned long request;

    switch (optional_actions) {
    case TCSANOW:
        request = TCSETS;
        break;
    case TCSADRAIN:
        request = TCSETSW;
        break;
    case TCSAFLUSH:
        request = TCSETSF;
        break;
    default:
        errno = EINVAL;
        return -1;
    }
    return sys_ioctl(fd, request, (struct termios *)t);
}
