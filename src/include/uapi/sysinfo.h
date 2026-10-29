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

struct rlimit {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};

/* RLIM_INFINITY is all ones, as in Linux. */
#define RLIM_INFINITY (~0ULL)

#define RLIMIT_CPU        0
#define RLIMIT_FSIZE      1
#define RLIMIT_DATA       2
#define RLIMIT_STACK      3
#define RLIMIT_CORE       4
#define RLIMIT_RSS        5
#define RLIMIT_NPROC      6
#define RLIMIT_NOFILE     7
#define RLIMIT_MEMLOCK    8
#define RLIMIT_AS         9
#define RLIMIT_LOCKS      10
#define RLIMIT_SIGPENDING 11
#define RLIMIT_MSGQUEUE   12
#define RLIMIT_NICE       13
#define RLIMIT_RTPRIO     14
#define RLIMIT_RTTIME     15
#define RLIMIT_NLIMITS    16

#endif /* UAPI_SYSINFO_H */
