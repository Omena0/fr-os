/*
 * test_scheduling_syscalls.c - Scheduling syscall tests
 *
 * Tests for syscalls 180-194: sched_yield, sched_setscheduler, ...
 */

#include "test_framework.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

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

static inline long syscall4(long n, long a1, long a2, long a3, long a4) {
    register long r10 asm("r10") = a4;
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10) : "rcx", "r11", "memory");
    return ret;
}

/* wait.h macros */
#define WIFEXITED(status)   (((status) & 0x7f) == 0)

static int test_sched_yield_basic(void) {
    long ret = syscall1(SYS_sched_yield, 0);
    /* Should return 0 on success */
    TEST_ASSERT_EQ(ret, 0, "sched_yield should succeed");
    return TEST_PASS;
}

static int test_sched_yield_many_times(void) {
    /* Call sched_yield a small number of times and assert each call
     * succeeds. The original loop ran 1000 iterations with no assertions
     * at all, which only ever proved the kernel did not deadlock -- and
     * burned CPU on an idle machine. 16 iterations is enough to exercise
     * a reschedule without dominating the test run. */
    for (int i = 0; i < 16; i++) {
        long ret = syscall1(SYS_sched_yield, 0);
        TEST_ASSERT_EQ(ret, 0, "sched_yield should succeed");
    }
    return TEST_PASS;
}

static int test_sched_yield_with_fork(void) {
    pid_t pid = (pid_t)syscall1(SYS_fork, 0);
    TEST_ASSERT(pid >= 0, "fork should succeed");
    
    if (pid == 0) {
        /* Child */
        syscall1(SYS_sched_yield, 0);
        syscall1(SYS_exit, 0);
    } else {
        /* Parent */
        int wstatus = 0;
        syscall4(SYS_wait4, pid, (long)&wstatus, 0, 0);
        TEST_ASSERT(WIFEXITED(wstatus), "child should exit normally");
    }
    return TEST_PASS;
}

/* ===== getcpu ===== */

static int test_getcpu_basic(void) {
    int cpu = 0, node = 0;
    long ret = syscall2(SYS_getcpu, (long)&cpu, (long)&node);
    /* getcpu returns 0 on success, or -ENOSYS if not implemented */
    TEST_ASSERT(ret == 0 || ret == -ENOSYS,
                "getcpu should succeed or return ENOSYS");
    if (ret == 0) {
        TEST_ASSERT(cpu >= 0, "cpu should be non-negative");
        TEST_ASSERT(node >= 0, "node should be non-negative");
    }
    return TEST_PASS;
}

static int test_getcpu_invalid_ptr(void) {
    /* Test getcpu with NULL pointers */
    long ret = syscall2(SYS_getcpu, 0, 0); /* both NULL */
    TEST_ASSERT(ret == -EFAULT || ret == -ENOSYS,
                "getcpu with NULL should return EFAULT/ENOSYS");
    return TEST_PASS;
}

/* ===== Test Suite Registration ===== */

struct test_suite test_scheduling_syscalls = {
    .name = "Scheduling Syscalls",
    .cases = (struct test_case[]) {
        { "sched_yield_basic", test_sched_yield_basic, false },
        { "sched_yield_many_times", test_sched_yield_many_times, false },
        { "sched_yield_with_fork", test_sched_yield_with_fork, false },
        { "getcpu_basic", test_getcpu_basic, false },
        { "getcpu_invalid_ptr", test_getcpu_invalid_ptr, false },
    },
    .num_cases = 5,
};