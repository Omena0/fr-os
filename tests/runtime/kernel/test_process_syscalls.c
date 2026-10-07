/*
 * test_process_syscalls.c - Process management syscall tests
 *
 * Tests for syscalls 0-17: fork, execve, exit, exit_group, wait4,
 * getpid, getppid, gettid, clone, kill, getpgid, setpgid, setsid,
 * nanosleep, clock_gettime, getuid, getgid, arch_prctl
 */

#include "test_framework.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* include libc headers that come with the kernel's uapi headers */
#include "time.h"  /* defines CLOCK_REALTIME, CLOCK_MONOTONIC from <uapi/syscall.h> */

/* Do NOT redefine SYS_* macros - they come from the kernel ABI header already via <unistd.h> */

/* wait.h macros - we can't include <sys/wait.h> because it conflicts with kernel headers */
#define WIFEXITED(status)   (((status) & 0x7f) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xff)
#define WNOHANG             1

/* signal.h macros - we can't include <signal.h> because it conflicts with kernel headers */
#define SIGCHLD    17
#define SIGUSR1    10
#define SIGTERM    15
#define SIGKILL    9
#define ECHILD     10
#define EINVAL     22
#define ENOENT     2
#define EACCES     13
#define ENOEXEC    8
#define ESRCH      3
#define EPERM      1
#define ENOMEM     12

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
    register long r10 asm("r10") = a3;
    asm volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "r"(r10) : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall4(long n, long a1, long a2, long a3, long a4) {
    long ret;
    register long r10 asm("r10") = a3;
    register long r8 asm("r8") = a4;
    asm volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "r"(r10), "r"(r8) : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall5(long n, long a1, long a2, long a3, long a4, long a5) {
    long ret;
    register long r10 asm("r10") = a3;
    register long r8 asm("r8") = a4;
    register long r9 asm("r9") = a5;
    asm volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return ret;
}

/* ===== fork / exit / wait4 ===== */

static int test_fork_basic(void) {
    pid_t pid = (pid_t)syscall1(SYS_fork, 0);
    TEST_ASSERT(pid >= 0, "fork should not fail");

    if (pid == 0) {
        /* Child */
        syscall1(SYS_exit, 42);
    } else {
        /* Parent */
        int wstatus = 0;
        pid_t waited = (pid_t)syscall4(SYS_wait4, pid, (long)&wstatus, 0, 0);
        TEST_ASSERT_EQ(waited, pid, "wait4 should return child pid");
        TEST_ASSERT(WIFEXITED(wstatus), "child should have exited normally");
        TEST_ASSERT_EQ(WEXITSTATUS(wstatus), 42, "child exit status should be 42");
    }
    return TEST_PASS;
}

static int test_fork_pid_values(void) {
    pid_t pid = (pid_t)syscall1(SYS_fork, 0);
    TEST_ASSERT(pid >= 0, "fork should not fail");

    if (pid == 0) {
        /* Child: getpid should return its own PID */
        pid_t my_pid = (pid_t)syscall1(SYS_getpid, 0);
        TEST_ASSERT(my_pid > 0, "child getpid should return positive PID");

        /* getppid should return parent's PID */
        pid_t parent_pid = (pid_t)syscall1(SYS_getppid, 0);
        TEST_ASSERT(parent_pid > 0, "child getppid should return positive PID");

        syscall1(SYS_exit, 0);
    } else {
        /* Parent: getpid should return its own PID */
        pid_t my_pid = (pid_t)syscall1(SYS_getpid, 0);
        TEST_ASSERT_EQ(my_pid, pid, "parent getpid should match fork return value");

        int wstatus = 0;
        syscall4(SYS_wait4, pid, (long)&wstatus, 0, 0);
    }
    return TEST_PASS;
}

static int test_wait4_no_children(void) {
    int wstatus = 0;
    pid_t ret = (pid_t)syscall4(SYS_wait4, -1, (long)&wstatus, 0, 0);
    TEST_ASSERT_EQ(ret, -1, "wait4 with no children should return -1");
    TEST_ASSERT_EQ(errno, ECHILD, "errno should be ECHILD");
    return TEST_PASS;
}

static int test_wait4_wrong_pid(void) {
    pid_t pid = (pid_t)syscall1(SYS_fork, 0);
    TEST_ASSERT(pid >= 0, "fork should not fail");

    if (pid == 0) {
        syscall1(SYS_exit, 0);
    } else {
        int wstatus = 0;
        pid_t ret = (pid_t)syscall4(SYS_wait4, pid + 100, (long)&wstatus, 0, 0);
        TEST_ASSERT_EQ(ret, -1, "wait4 with wrong pid should return -1");
        TEST_ASSERT_EQ(errno, ECHILD, "errno should be ECHILD");

        /* Now wait correctly */
        syscall4(SYS_wait4, pid, (long)&wstatus, 0, 0);
    }
    return TEST_PASS;
}

/* ===== execve ===== */

static int test_execve_nonexistent(void) {
    char *argv[] = { "/nonexistent", NULL };
    char *envp[] = { NULL };
    long ret = syscall3(SYS_execve, (long)"/nonexistent", (long)argv, (long)envp);
    TEST_ASSERT_EQ(ret, -ENOENT, "execve nonexistent should return -ENOENT");
    return TEST_PASS;
}

static int test_execve_not_executable(void) {
    char *argv[] = { "/etc/passwd", NULL };
    char *envp[] = { NULL };
    long ret = syscall3(SYS_execve, (long)"/etc/passwd", (long)argv, (long)envp);
    TEST_ASSERT(ret == -EACCES || ret == -ENOEXEC, "execve non-executable should fail");
    return TEST_PASS;
}

/* ===== getpid / getppid / gettid ===== */

static int test_getpid_positive(void) {
    pid_t pid = (pid_t)syscall1(SYS_getpid, 0);
    TEST_ASSERT(pid > 0, "getpid should return positive PID");
    return TEST_PASS;
}

static int test_getppid_positive(void) {
    pid_t ppid = (pid_t)syscall1(SYS_getppid, 0);
    TEST_ASSERT(ppid > 0, "getppid should return positive PID");
    return TEST_PASS;
}

static int test_gettid_positive(void) {
    pid_t tid = (pid_t)syscall1(SYS_gettid, 0);
    TEST_ASSERT(tid > 0, "gettid should return positive TID");
    return TEST_PASS;
}

/* ===== clone ===== */

static int test_clone_basic(void) {
	/* Simple clone without CLONE_VM - creates a new process. The child
	 * stack is a static array rather than a malloc'd block: this is a
	 * kernel test and the kernel does not own the userspace allocator, so
	 * a malloc failure here would be a false positive about the clone. */
	unsigned char stack[65536];
	int child_tid = 0;
	int flags = 0; /* No CLONE_VM = new process */
	long ret = syscall5(SYS_clone, flags,
			    (long)(stack + sizeof(stack)), 0,
			    (long)&child_tid, 0);

	TEST_ASSERT(ret >= 0, "clone should not fail");

	if (ret == 0) {
		/* Child */
		syscall1(SYS_exit, 0);
	} else {
		/* Parent */
		int wstatus = 0;
		syscall4(SYS_wait4, ret, (long)&wstatus, 0, 0);
		TEST_ASSERT(WIFEXITED(wstatus), "cloned child should exit normally");
	}
	return TEST_PASS;
}

/* ===== kill ===== */

static int test_kill_self(void) {
    pid_t pid = (pid_t)syscall1(SYS_getpid, 0);
    long ret = syscall2(SYS_kill, pid, SIGUSR1);
    /* Should fail - signal not handled */
    TEST_ASSERT(ret == 0 || ret == -EINVAL, "kill self with SIGUSR1 should work or return error");
    return TEST_PASS;
}

static int test_kill_invalid_pid(void) {
    long ret = syscall2(SYS_kill, 999999, SIGTERM);
    TEST_ASSERT_EQ(ret, -ESRCH, "kill invalid PID should return -ESRCH");
    return TEST_PASS;
}

static int test_kill_invalid_signal(void) {
    pid_t pid = (pid_t)syscall1(SYS_getpid, 0);
    long ret = syscall2(SYS_kill, pid, 999);
    TEST_ASSERT_EQ(ret, -EINVAL, "kill invalid signal should return -EINVAL");
    return TEST_PASS;
}

/* ===== getpgid / setpgid / setsid ===== */

static int test_getpgid_self(void) {
    pid_t pid = (pid_t)syscall1(SYS_getpid, 0);
    long ret = syscall2(SYS_getpgid, pid, 0);
    TEST_ASSERT(ret >= 0, "getpgid self should succeed");
    return TEST_PASS;
}

static int test_setpgid_self(void) {
    pid_t pid = (pid_t)syscall1(SYS_getpid, 0);
    long ret = syscall2(SYS_setpgid, pid, pid);
    TEST_ASSERT(ret == 0 || ret == -EPERM, "setpgid to self should work or EPERM");
    return TEST_PASS;
}

static int test_setsid(void) {
    long ret = syscall1(SYS_setsid, 0);
    /* May fail if already session leader */
    TEST_ASSERT(ret >= 0 || ret == -EPERM, "setsid should succeed or return -EPERM");
    return TEST_PASS;
}

/* ===== nanosleep ===== */

static int test_nanosleep_basic(void) {
    struct timespec req = { .tv_sec = 0, .tv_nsec = 10000000 }; /* 10ms */
    struct timespec rem = { 0 };
    long ret = syscall2(SYS_nanosleep, (long)&req, (long)&rem);
    TEST_ASSERT(ret == 0 || ret == -EINTR, "nanosleep should succeed or be interrupted");
    return TEST_PASS;
}

static int test_nanosleep_zero(void) {
    struct timespec req = { .tv_sec = 0, .tv_nsec = 0 };
    struct timespec rem = { 0 };
    long ret = syscall2(SYS_nanosleep, (long)&req, (long)&rem);
    TEST_ASSERT_EQ(ret, 0, "nanosleep 0 should return immediately");
    return TEST_PASS;
}

static int test_nanosleep_invalid(void) {
    struct timespec req = { .tv_sec = 0, .tv_nsec = 1000000000 }; /* invalid: >= 1 sec */
    struct timespec rem = { 0 };
    long ret = syscall2(SYS_nanosleep, (long)&req, (long)&rem);
    TEST_ASSERT_EQ(ret, -EINVAL, "nanosleep invalid nsec should return -EINVAL");
    return TEST_PASS;
}

/* ===== clock_gettime ===== */

static int test_clock_gettime_realtime(void) {
    struct timespec tp = { 0 };
    long ret = syscall2(SYS_clock_gettime, CLOCK_REALTIME, (long)&tp);
    TEST_ASSERT_EQ(ret, 0, "clock_gettime REALTIME should succeed");
    TEST_ASSERT(tp.tv_sec > 0, "tv_sec should be positive");
    return TEST_PASS;
}

static int test_clock_gettime_monotonic(void) {
    struct timespec tp = { 0 };
    long ret = syscall2(SYS_clock_gettime, CLOCK_MONOTONIC, (long)&tp);
    TEST_ASSERT_EQ(ret, 0, "clock_gettime MONOTONIC should succeed");
    TEST_ASSERT(tp.tv_sec >= 0, "tv_sec should be non-negative");
    return TEST_PASS;
}

static int test_clock_gettime_invalid(void) {
    struct timespec tp = { 0 };
    long ret = syscall2(SYS_clock_gettime, 999, (long)&tp);
    TEST_ASSERT_EQ(ret, -EINVAL, "clock_gettime invalid clock should return -EINVAL");
    return TEST_PASS;
}

/* ===== getuid / getgid ===== */

static int test_getuid_nonnegative(void) {
    long ret = syscall1(SYS_getuid, 0);
    TEST_ASSERT(ret >= 0, "getuid should return non-negative UID");
    return TEST_PASS;
}

static int test_getgid_nonnegative(void) {
    long ret = syscall1(SYS_getgid, 0);
    TEST_ASSERT(ret >= 0, "getgid should return non-negative GID");
    return TEST_PASS;
}

/* ===== arch_prctl ===== */

static int test_arch_prctl_get_fs(void) {
	uint64_t addr = 0;
	long ret = syscall2(SYS_arch_prctl, ARCH_GET_FS, (long)&addr);
	TEST_ASSERT(ret == 0 || ret == -EINVAL, "ARCH_GET_FS should work or return -EINVAL");
	return TEST_PASS;
}

static int test_arch_prctl_get_gs(void) {
	uint64_t addr = 0;
	long ret = syscall2(SYS_arch_prctl, ARCH_GET_GS, (long)&addr);
	TEST_ASSERT(ret == 0 || ret == -EINVAL, "ARCH_GET_GS should work or return -EINVAL");
	return TEST_PASS;
}

/* ===== Test Suite Registration ===== */

struct test_suite test_process_syscalls = {
    .name = "Process Syscalls",
    .cases = (struct test_case[]) {
        { "fork_basic", test_fork_basic, false },
        { "fork_pid_values", test_fork_pid_values, false },
        { "wait4_no_children", test_wait4_no_children, false },
        { "wait4_wrong_pid", test_wait4_wrong_pid, false },
        { "execve_nonexistent", test_execve_nonexistent, false },
        { "execve_not_executable", test_execve_not_executable, false },
        { "getpid_positive", test_getpid_positive, false },
        { "getppid_positive", test_getppid_positive, false },
        { "gettid_positive", test_gettid_positive, false },
        { "clone_basic", test_clone_basic, false },
        { "kill_self", test_kill_self, false },
        { "kill_invalid_pid", test_kill_invalid_pid, false },
        { "kill_invalid_signal", test_kill_invalid_signal, false },
        { "getpgid_self", test_getpgid_self, false },
        { "setpgid_self", test_setpgid_self, false },
        { "setsid", test_setsid, false },
        { "nanosleep_basic", test_nanosleep_basic, false },
        { "nanosleep_zero", test_nanosleep_zero, false },
        { "nanosleep_invalid", test_nanosleep_invalid, false },
        { "clock_gettime_realtime", test_clock_gettime_realtime, false },
        { "clock_gettime_monotonic", test_clock_gettime_monotonic, false },
        { "clock_gettime_invalid", test_clock_gettime_invalid, false },
        { "getuid_nonnegative", test_getuid_nonnegative, false },
        { "getgid_nonnegative", test_getgid_nonnegative, false },
        { "arch_prctl_get_fs", test_arch_prctl_get_fs, false },
        { "arch_prctl_get_gs", test_arch_prctl_get_gs, false },
    },
    .num_cases = 26,
};