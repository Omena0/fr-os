/*
 * sys/sysinfo.h - system statistics.
 */
#ifndef SYS_SYSINFO_H
#define SYS_SYSINFO_H

#include <uapi/sysinfo.h>

#ifdef __cplusplus
extern "C" {
#endif

int sysinfo(struct sysinfo *info);

#ifdef __cplusplus
}
#endif

#endif /* SYS_SYSINFO_H */
