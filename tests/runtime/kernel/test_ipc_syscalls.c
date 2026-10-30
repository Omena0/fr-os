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
#include <uapi/syscall.h>

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

static int test_poll_pipe_ready(void) {
    int fds[2];
    struct pollfd pfd;
    char byte = 'x';

    TEST_ASSERT_EQ(syscall2(SYS_pipe, (long)fds, 0), 0, "pipe should succeed");
    TEST_ASSERT_EQ(syscall3(SYS_write, fds[1], (long)&byte, 1), 1, "pipe write should succeed");
    pfd.fd = fds[0];
    pfd.events = POLLIN;
    pfd.revents = 0;
    TEST_ASSERT_EQ(syscall3(SYS_poll, (long)&pfd, 1, 0), 1, "poll should report the readable end ready");
    TEST_ASSERT((pfd.revents & POLLIN) != 0, "revents should contain POLLIN");
    syscall1(SYS_close, fds[0]);
    syscall1(SYS_close, fds[1]);
    return TEST_PASS;
}

static int test_poll_empty_timeout(void) {
    int fds[2];
    struct pollfd pfd;

    TEST_ASSERT_EQ(syscall2(SYS_pipe, (long)fds, 0), 0, "pipe should succeed");
    pfd.fd = fds[0];
    pfd.events = POLLIN;
    pfd.revents = 1;
    TEST_ASSERT_EQ(syscall3(SYS_poll, (long)&pfd, 1, 0), 0, "poll of an empty pipe should time out at once");
    TEST_ASSERT_EQ(pfd.revents, 0, "revents should be cleared on timeout");
    syscall1(SYS_close, fds[0]);
    syscall1(SYS_close, fds[1]);
    return TEST_PASS;
}

static int test_poll_write_end_ready(void) {
    int fds[2];
    struct pollfd pfd;

    TEST_ASSERT_EQ(syscall2(SYS_pipe, (long)fds, 0), 0, "pipe should succeed");
    pfd.fd = fds[1];
    pfd.events = POLLOUT;
    pfd.revents = 0;
    TEST_ASSERT_EQ(syscall3(SYS_poll, (long)&pfd, 1, 0), 1, "poll should report the write end ready");
    TEST_ASSERT((pfd.revents & POLLOUT) != 0, "revents should contain POLLOUT");
    syscall1(SYS_close, fds[0]);
    syscall1(SYS_close, fds[1]);
    return TEST_PASS;
}

static int test_poll_bad_fd_nval(void) {
    struct pollfd pfd;

    pfd.fd = 999;
    pfd.events = POLLIN;
    pfd.revents = 0;
    TEST_ASSERT_EQ(syscall3(SYS_poll, (long)&pfd, 1, 0), 1, "poll should count an invalid descriptor as ready");
    TEST_ASSERT((pfd.revents & POLLNVAL) != 0, "revents should contain POLLNVAL");
    return TEST_PASS;
}

static int test_poll_pipe_eof(void) {
    int fds[2];
    struct pollfd pfd;

    TEST_ASSERT_EQ(syscall2(SYS_pipe, (long)fds, 0), 0, "pipe should succeed");
    syscall1(SYS_close, fds[1]);
    pfd.fd = fds[0];
    pfd.events = POLLIN;
    pfd.revents = 0;
    TEST_ASSERT(syscall3(SYS_poll, (long)&pfd, 1, 0) == 1, "a closed write end should make the read end ready");
    TEST_ASSERT((pfd.revents & POLLHUP) != 0, "revents should contain POLLHUP");
    syscall1(SYS_close, fds[0]);
    return TEST_PASS;
}

static int test_poll_timeout_waits(void) {
    int fds[2];
    struct pollfd pfd;

    TEST_ASSERT_EQ(syscall2(SYS_pipe, (long)fds, 0), 0, "pipe should succeed");
    pfd.fd = fds[0];
    pfd.events = POLLIN;
    pfd.revents = 0;
    TEST_ASSERT_EQ(syscall3(SYS_poll, (long)&pfd, 1, 10), 0, "a 10 ms poll of an empty pipe should return 0");
    syscall1(SYS_close, fds[0]);
    syscall1(SYS_close, fds[1]);
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
        { "poll_pipe_ready", test_poll_pipe_ready, false },
        { "poll_empty_timeout", test_poll_empty_timeout, false },
        { "poll_write_end_ready", test_poll_write_end_ready, false },
        { "poll_bad_fd_nval", test_poll_bad_fd_nval, false },
        { "poll_pipe_eof", test_poll_pipe_eof, false },
        { "poll_timeout_waits", test_poll_timeout_waits, false },
    },
    .num_cases = 11,
};