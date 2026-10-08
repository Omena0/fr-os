/*
 * test_ipc_syscalls.c - IPC syscall tests
 *
 * Tests for syscalls 128-139: pipe, pipe2, futex, ...
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

static inline long syscall3(long n, long a1, long a2, long a3) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
    return ret;
}

/* errno values */
#define ENOMEM      12
#define EFAULT      14
#define EINVAL      22
#define EMFILE      24
#define ENFILE      23

/* ===== pipe ===== */

static int test_pipe_basic(void) {
    int fds[2];
    long ret = syscall2(SYS_pipe, (long)fds, 0);
    TEST_ASSERT_EQ(ret, 0, "pipe should succeed");
    TEST_ASSERT(fds[0] >= 0 && fds[1] >= 0, "both fds should be valid");
    
    /* Test they're readable/writable */
    syscall1(SYS_close, fds[0]);
    syscall1(SYS_close, fds[1]);
    return TEST_PASS;
}

static int test_pipe_invalid_fds_ptr(void) {
    /* Pass a NULL fds pointer - kernel must reject with EFAULT.
     * NOTE: SYS_pipe takes only (int fds[2]); the second syscall
     * arg is ignored by the kernel, so passing NULL/0 exercises the
     * same user_range_ok() EFAULT path that a bad userspace pointer
     * triggers. */
    long ret = syscall2(SYS_pipe, (long)NULL, 0);
    TEST_ASSERT_EQ(ret, -EFAULT, "pipe with NULL fds should return EFAULT");
    return TEST_PASS;
}

static int test_pipe_read_write(void) {
    int fds[2];
    long ret = syscall2(SYS_pipe, (long)fds, 0);
    TEST_ASSERT_EQ(ret, 0, "pipe should succeed");
    
    const char *msg = "hello\n";
    ssize_t wret = syscall3(SYS_write, fds[1], (long)msg, 6);
    TEST_ASSERT_EQ(wret, 6, "pipe write should succeed");
    
    char buf[10];
    ssize_t rret = syscall3(SYS_read, fds[0], (long)buf, sizeof(buf));
    TEST_ASSERT_EQ(rret, 6, "pipe read should return same data");
    TEST_ASSERT_EQ(memcmp(buf, msg, 6), 0, "pipe data should match");
    
    syscall1(SYS_close, fds[0]);
    syscall1(SYS_close, fds[1]);
    return TEST_PASS;
}

/* ===== futex ===== */

static int test_futex_basic(void) {
	/* FUTEX_WAKE on a futex whose value is not the expected one is a
	 * non-blocking operation: it wakes waiters (here, none) and returns
	 * the number woken. This exercises the syscall without the risk of
	 * blocking the test harness forever, which a FUTEX_WAIT with no
	 * timeout would carry. */
	int val = 0;
	long ret = syscall3(SYS_futex, (long)&val, FUTEX_WAKE, 1);
	TEST_ASSERT(ret == 0, "futex wake on uncontended word should return 0");
	return TEST_PASS;
}

static int test_futex_invalid_addr(void) {
	/* Test futex with an unmapped address. FUTEX_WAKE is non-blocking, so
	 * the kernel must reject the bad pointer without the test hanging. */
	long ret = syscall3(SYS_futex, 0x1000, FUTEX_WAKE, 1);
	TEST_ASSERT(ret == -EFAULT || ret == -EINVAL,
		    "futex with invalid address should return EFAULT/EINVAL");
	return TEST_PASS;
}

/* ===== Test Suite Registration ===== */

struct test_suite test_ipc_syscalls = {
    .name = "IPC Syscalls",
    .cases = (struct test_case[]) {
        { "pipe_basic", test_pipe_basic, false },
        { "pipe_invalid_fds_ptr", test_pipe_invalid_fds_ptr, false },
        { "pipe_read_write", test_pipe_read_write, false },
        { "futex_basic", test_futex_basic, false },
        { "futex_invalid_addr", test_futex_invalid_addr, false },
    },
    .num_cases = 5,
};