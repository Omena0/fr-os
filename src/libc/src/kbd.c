/*
 * kbd.c — libc wrapper for the keyboard typematic syscall.
 */
#include <errno.h>
#include <kbd.h>
#include <sys/syscall.h>

extern long __syscall1(long number, long a1);

int kbd_typematic(int value)
{
    long ret = __syscall1(SYS_kbd_typematic, (long)value);

    if (ret < 0) {
        __errno = (int)-ret;
        return -1;
    }
    return (int)ret;
}
