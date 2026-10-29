/*
 * resource.c - getrlimit and setrlimit wrappers.
 */
#include <sys/resource.h>

extern int sys_getrlimit(int resource, struct rlimit *rlim);
extern int sys_setrlimit(int resource, const struct rlimit *rlim);

int getrlimit(int resource, struct rlimit *rlim)
{
    return sys_getrlimit(resource, rlim);
}

int setrlimit(int resource, const struct rlimit *rlim)
{
    return sys_setrlimit(resource, rlim);
}
