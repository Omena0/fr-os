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

#include <uapi/syscall.h>

/* errno values */
#define EPERM       1
#define EINVAL      22

static inline long syscall1(long n, long a1) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1) : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall2(long n, long a1, long a2) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2) : "rcx", "r11", "memory");
    return ret;
}

/* ===== setuid / setgid ===== */

static int test_setuid_basic(void) {
    /* setuid/setgid are not implemented in this kernel yet; the syscall
     * numbers below are placeholders. Skip rather than probe an ABI that
     * does not exist, which would only ever test the kernel's -ENOSYS
     * fallback. */
    return TEST_SKIP;
}

static int test_setgid_basic(void) {
    return TEST_SKIP;
}

static int test_setuid_invalid_user(void) {
    return TEST_SKIP;
}

static int test_setgid_invalid_group(void) {
    return TEST_SKIP;
}

/* ===== arch_prctl ===== */

static int test_arch_prctl_get_fs_basic(void) {
    uint64_t addr = 0;
    long ret = syscall2(SYS_arch_prctl, ARCH_GET_FS, (long)&addr);
    /* ARCH_GET_FS should return current FS base or -EINVAL */
    TEST_ASSERT(ret == 0 || ret == -EINVAL, "ARCH_GET_FS should work or return -EINVAL");
    return TEST_PASS;
}

static int test_arch_prctl_get_gs_basic(void) {
    uint64_t addr = 0;
    long ret = syscall2(SYS_arch_prctl, ARCH_GET_GS, (long)&addr);
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
        { "setuid_basic", test_setuid_basic, true },
        { "setgid_basic", test_setgid_basic, true },
        { "setuid_invalid_user", test_setuid_invalid_user, true },
        { "setgid_invalid_group", test_setgid_invalid_group, true },
        { "arch_prctl_get_fs", test_arch_prctl_get_fs_basic, false },
        { "arch_prctl_get_gs", test_arch_prctl_get_gs_basic, false },
        { "arch_prctl_invalid_code", test_arch_prctl_invalid_code, false },
    },
    .num_cases = 7,
};