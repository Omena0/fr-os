/*
 * sys/utsname.h - system identification.
 */
#ifndef SYS_UTSNAME_H
#define SYS_UTSNAME_H

#include <uapi/sysinfo.h>

#ifdef __cplusplus
extern "C" {
#endif

int uname(struct utsname *buf);

#ifdef __cplusplus
}
#endif

#endif /* SYS_UTSNAME_H */
