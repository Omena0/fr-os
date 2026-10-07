/*
 * test_security_syscalls.c - Security syscall tests
 *
 * Tests for syscalls 200-215: setuid, setgid, arch_prctl, ...
 */

#include "test_framework.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* syscalls in the COMPLETE version (not yet in uapi/syscall.h) */
#define SYS_setuid   200
#define SYS_setgid   201
#define SYS_setgroups 202

static inline long syscall2(long n, long a1, long a2) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2) : "rcx", "r11", "memory");
    return ret;
}

/* errno values */
#define EPERM       1
#define EINVAL      22

/* ===== setuid / setgid ===== */

static int test_setuid_basic(void) {
    /* setuid requires privileges - test with 0 (root) should succeed */
    long ret = syscall2(SYS_setuid, 0, 0);
    /* Root can setuid to any user */
    TEST_ASSERT(ret == 0 || ret == -EPERM || ret == -EINVAL,
                "setuid should succeed or return EPERM/EINVAL");
    return TEST_PASS;
}

static int test_setgid_basic(void) {
    long ret = syscall2(SYS_setgid, 0, 0);
    TEST_ASSERT(ret == 0 || ret == -EPERM || ret == -EINVAL,
                "setgid should succeed or return EPERM/EINVAL");
    return TEST_PASS;
}

static int test_setuid_invalid_user(void) {
    /* setuid with invalid user should fail */
    long ret = syscall2(SYS_setuid, 999, 0);
    TEST_ASSERT(ret == -EINVAL || ret == -EPERM, "setuid invalid user should fail");
    return TEST_PASS;
}

static int test_setgid_invalid_group(void) {
    long ret = syscall2(SYS_setgid, 999, 0);
    TEST_ASSERT(ret == -EINVAL || ret == -EPERM, "setgid invalid group should fail");
    return TEST_PASS;
}

/* ===== arch_prctl ===== */

static int test_arch_prctl_get_fs_basic(void) {
    uint64_t addr = 0;
    long ret = syscall2(SYS_arch_prctl, 0x1003 /* ARCH_GET_FS */, (long)&addr);
    /* ARCH_GET_FS should return current FS base or -EINVAL */
    TEST_ASSERT(ret == 0 || ret == -EINVAL, "ARCH_GET_FS should work or return -EINVAL");
    return TEST_PASS;
}

static int test_arch_prctl_get_gs_basic(void) {
    uint64_t addr = 0;
    long ret = syscall2(SYS_arch_prctl, 0x1004 /* ARCH_GET_GS */, (long)&addr);
    TEST_ASSERT(ret == 0 || ret == -EINVAL, "ARCH_GET_GS should work or return -EINVAL");
    return TEST_PASS;
}

static int test_arch_prctl_invalid_code(void) {
    uint64_t addr = 0;
    long ret = syscall2(SYS_arch_prctl, 999 /* invalid code */, (long)&addr);
    TEST_ASSERT(ret == -EINVAL, "invalid arch_prctl code should return -EINVAL");
    return TEST_PASS;
}

/* ===== Test Suite Registration ===== */

struct test_suite test_security_syscalls = {
    .name = "Security Syscalls",
    .cases = (struct test_case[]) {
        { "setuid_basic", test_setuid_basic, false },
        { "setgid_basic", test_setgid_basic, false },
        { "setuid_invalid_user", test_setuid_invalid_user, false },
        { "setgid_invalid_group", test_setgid_invalid_group, false },
        { "arch_prctl_get_fs", test_arch_prctl_get_fs_basic, false },
        { "arch_prctl_get_gs", test_arch_prctl_get_gs_basic, false },
        { "arch_prctl_invalid_code", test_arch_prctl_invalid_code, false },
    },
    .num_cases = 7,
};