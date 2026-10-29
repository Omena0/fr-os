/*
 * uapi/sysinfo.h — system identity and resource limits.
 *
 * These structures are what uname(2), sysinfo(2) and getrlimit(2) copy to a
 * caller. They live in the shared ABI header set because the layout is the
 * wire format between the kernel and libc.
 */
#ifndef UAPI_SYSINFO_H
#define UAPI_SYSINFO_H

#include <stdint.h>

#define UTS_FIELD_LEN 65

struct utsname {
    char sysname[UTS_FIELD_LEN];
    char nodename[UTS_FIELD_LEN];
    char release[UTS_FIELD_LEN];
    char version[UTS_FIELD_LEN];
    char machine[UTS_FIELD_LEN];
    char domainname[UTS_FIELD_LEN];
};

/*
 * A practical subset of Linux's struct sysinfo. Memory is reported in bytes
 * with mem_unit = 1; loads and swap are zero because there is no load average
 * and no swap.
 */
struct sysinfo {
    uint64_t uptime;
    uint64_t loads[3];
    uint64_t totalram;
    uint64_t freeram;
    uint64_t sharedram;
    uint64_t bufferram;
    uint64_t totalswap;
    uint64_t freeswap;
    uint16_t procs;
    uint16_t pad;
    uint64_t totalhigh;
    uint64_t freehigh;
    uint32_t mem_unit;
    uint32_t reserved;
};

#endif /* UAPI_SYSINFO_H */
