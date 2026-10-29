/*
 * sys/resource.h - resource limits.
 *
 * Limits are reported and validated, not enforced; see the kernel's
 * default_rlimit() for the values each resource reports.
 */
#ifndef SYS_RESOURCE_H
#define SYS_RESOURCE_H

#include <uapi/sysinfo.h>

#ifdef __cplusplus
extern "C" {
#endif

int getrlimit(int resource, struct rlimit *rlim);
int setrlimit(int resource, const struct rlimit *rlim);

#ifdef __cplusplus
}
#endif

#endif /* SYS_RESOURCE_H */
