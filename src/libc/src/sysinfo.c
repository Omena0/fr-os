/*
 * sysinfo.c - uname and sysinfo wrappers.
 */
#include <sys/utsname.h>
#include <sys/sysinfo.h>

extern int sys_uname(struct utsname *buf);
extern int sys_sysinfo(struct sysinfo *info);

int uname(struct utsname *buf)
{
    return sys_uname(buf);
}

int sysinfo(struct sysinfo *info)
{
    return sys_sysinfo(info);
}
