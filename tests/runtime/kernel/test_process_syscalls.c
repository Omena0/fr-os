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
#include <uapi/sysinfo.h>
#include <uapi/signal.h>
#include <signal.h>
#include <stdint.h>
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
    asm volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall4(long n, long a1, long a2, long a3, long a4) {
    long ret;
    register long r10 asm("r10") = a4;
    asm volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10) : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall5(long n, long a1, long a2, long a3, long a4, long a5) {
    long ret;
    register long r10 asm("r10") = a4;
    register long r8 asm("r8") = a5;
    asm volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8) : "rcx", "r11", "memory");
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
        /* Parent: getpid should return its own positive PID */
        pid_t my_pid = (pid_t)syscall1(SYS_getpid, 0);
        TEST_ASSERT(my_pid > 0, "parent getpid should return positive PID");

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

static int test_execve_empty_env(void) {
    /* The child replaces itself, so the test runner has to be the parent.
     * An empty environment exercises the envp[] = { NULL } path. */
    char *argv[] = { "hello", NULL };
    char *envp[] = { NULL };
    int wstatus = 0;
    pid_t pid = (pid_t)syscall1(SYS_fork, 0);

    TEST_ASSERT(pid >= 0, "fork should not fail");
    if (pid == 0) {
        syscall3(SYS_execve, (long)"hello", (long)argv, (long)envp);
        syscall1(SYS_exit, 127);
    }
    TEST_ASSERT_EQ(syscall4(SYS_wait4, pid, (long)&wstatus, 0, 0), pid, "wait4 should return the child");
    TEST_ASSERT(WIFEXITED(wstatus), "hello should exit normally");
    TEST_ASSERT_EQ(WEXITSTATUS(wstatus), 0, "hello should exit 0 with an empty environment");
    return TEST_PASS;
}

static int test_execve_not_executable(void) {
    char *argv[] = { "/etc/passwd", NULL };
    char *envp[] = { NULL };
    long ret = syscall3(SYS_execve, (long)"/etc/passwd", (long)argv, (long)envp);
    TEST_ASSERT(ret < 0, "execve non-existent should fail");
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
    struct sigaction ign;
    struct sigaction dfl;

    memset(&ign, 0, sizeof(ign));
    memset(&dfl, 0, sizeof(dfl));
    ign.sa_handler = SIG_IGN;
    dfl.sa_handler = SIG_DFL;
    TEST_ASSERT_EQ(sigaction(SIGUSR1, &ign, 0), 0, "ignoring SIGUSR1 should succeed");
    TEST_ASSERT_EQ(kill(getpid(), SIGUSR1), 0, "kill self with an ignored signal should succeed");
    TEST_ASSERT_EQ(sigaction(SIGUSR1, &dfl, 0), 0, "restoring SIGUSR1 should succeed");
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

static volatile int sig_hits;

static void sig_count_handler(int sig) {
    (void)sig;
    sig_hits++;
}

static int install_count_handler(int sig) {
    struct sigaction act;

    memset(&act, 0, sizeof(act));
    act.sa_handler = sig_count_handler;
    sigemptyset(&act.sa_mask);
    sig_hits = 0;
    return sigaction(sig, &act, 0);
}

static void restore_default(int sig) {
    struct sigaction dfl;

    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigaction(sig, &dfl, 0);
}

static int test_signal_handler_runs_on_return(void) {
    TEST_ASSERT_EQ(install_count_handler(SIGUSR1), 0, "installing a handler should succeed");
    TEST_ASSERT_EQ(kill(getpid(), SIGUSR1), 0, "kill self should succeed");
    TEST_ASSERT_EQ(sig_hits, 1, "the handler should have run before kill returned");
    restore_default(SIGUSR1);
    return TEST_PASS;
}

static int test_signal_blocked_until_unblocked(void) {
    sigset_t set, old, pending;

    TEST_ASSERT_EQ(install_count_handler(SIGUSR2), 0, "installing a handler should succeed");
    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    TEST_ASSERT_EQ(sigprocmask(SIG_BLOCK, &set, &old), 0, "blocking SIGUSR2 should succeed");
    TEST_ASSERT_EQ(kill(getpid(), SIGUSR2), 0, "kill self should succeed");
    TEST_ASSERT_EQ(sig_hits, 0, "a blocked signal must not run its handler");
    TEST_ASSERT_EQ(sigpending(&pending), 0, "sigpending should succeed");
    TEST_ASSERT_EQ(sigismember(&pending, SIGUSR2), 1, "the blocked signal should be pending");
    TEST_ASSERT_EQ(sigprocmask(SIG_UNBLOCK, &set, 0), 0, "unblocking SIGUSR2 should succeed");
    TEST_ASSERT_EQ(sig_hits, 1, "the handler should run once the signal is unblocked");
    TEST_ASSERT_EQ(sigprocmask(SIG_SETMASK, &old, 0), 0, "restoring the mask should succeed");
    restore_default(SIGUSR2);
    return TEST_PASS;
}

static int test_wait4_unknown_option(void) {
    int wstatus = 0;
    TEST_ASSERT_EQ(syscall4(SYS_wait4, -1, (long)&wstatus, 0x100, 0), -EINVAL, "an unknown wait option should return EINVAL");
    return TEST_PASS;
}

static int test_wait4_stop_options_accepted(void) {
    int wstatus = 0;
    long ret = syscall4(SYS_wait4, -1, (long)&wstatus, WUNTRACED | WCONTINUED | WNOHANG, 0);
    TEST_ASSERT(ret != -EINVAL, "WUNTRACED and WCONTINUED are valid options");
    return TEST_PASS;
}

static int test_signal_default_terminates(void) {
    pid_t pid = (pid_t)syscall1(SYS_fork, 0);
    int wstatus = 0;

    TEST_ASSERT(pid >= 0, "fork should not fail");
    if (pid == 0) {
        kill(getpid(), SIGTERM);
        syscall1(SYS_exit, 3);
    }
    TEST_ASSERT_EQ(syscall4(SYS_wait4, pid, (long)&wstatus, 0, 0), pid, "wait4 should return the child");
    TEST_ASSERT(WIFSIGNALED(wstatus), "a child killed by SIGTERM should report a signal death");
    TEST_ASSERT_EQ(WTERMSIG(wstatus), SIGTERM, "the terminating signal should be SIGTERM");
    return TEST_PASS;
}

static int test_kill_other_process_handler(void) {
    pid_t parent = getpid();
    pid_t pid;
    int wstatus = 0;

    TEST_ASSERT_EQ(install_count_handler(SIGUSR2), 0, "installing a handler should succeed");
    pid = (pid_t)syscall1(SYS_fork, 0);
    TEST_ASSERT(pid >= 0, "fork should not fail");
    if (pid == 0) {
        kill(parent, SIGUSR2);
        syscall1(SYS_exit, 0);
    }
    TEST_ASSERT_EQ(syscall4(SYS_wait4, pid, (long)&wstatus, 0, 0), pid, "wait4 should return the child");
    TEST_ASSERT_EQ(sig_hits, 1, "a signal sent from another process should run the handler once");
    restore_default(SIGUSR2);
    return TEST_PASS;
}

static int test_signal_interrupts_busy_loop(void) {
    pid_t pid;
    int wstatus = 0;

    TEST_ASSERT_EQ(install_count_handler(SIGUSR2), 0, "installing a handler should succeed");
    pid = (pid_t)syscall1(SYS_fork, 0);
    TEST_ASSERT(pid >= 0, "fork should not fail");
    if (pid == 0) {
        while (sig_hits == 0)
            ;
        syscall1(SYS_exit, 7);
    }
    TEST_ASSERT_EQ(kill(pid, SIGUSR2), 0, "SIGUSR2 to a busy child should succeed");
    TEST_ASSERT_EQ(syscall4(SYS_wait4, pid, (long)&wstatus, 0, 0), pid, "wait4 should return the child");
    TEST_ASSERT(WIFEXITED(wstatus), "a busy child should exit normally once its handler has run");
    TEST_ASSERT_EQ(WEXITSTATUS(wstatus), 7, "the child should have seen its handler run");
    restore_default(SIGUSR2);
    return TEST_PASS;
}

static int test_kill_sigkill_other(void) {
    struct timespec req = { .tv_sec = 0, .tv_nsec = 10000000 };
    pid_t pid = (pid_t)syscall1(SYS_fork, 0);
    int wstatus = 0;

    TEST_ASSERT(pid >= 0, "fork should not fail");
    if (pid == 0) {
        for (;;)
            syscall2(SYS_nanosleep, (long)&req, 0);
    }
    TEST_ASSERT_EQ(kill(pid, SIGKILL), 0, "SIGKILL to a child should succeed");
    TEST_ASSERT_EQ(syscall4(SYS_wait4, pid, (long)&wstatus, 0, 0), pid, "wait4 should return the child");
    TEST_ASSERT(WIFSIGNALED(wstatus), "a SIGKILLed child should report a signal death");
    TEST_ASSERT_EQ(WTERMSIG(wstatus), SIGKILL, "the terminating signal should be SIGKILL");
    return TEST_PASS;
}

static int test_fault_raises_sigsegv(void) {
    pid_t pid = (pid_t)syscall1(SYS_fork, 0);
    int wstatus = 0;

    TEST_ASSERT(pid >= 0, "fork should not fail");
    if (pid == 0) {
        volatile int *wild = (volatile int *)0;
        *wild = 1;
        syscall1(SYS_exit, 3);
    }
    TEST_ASSERT_EQ(syscall4(SYS_wait4, pid, (long)&wstatus, 0, 0), pid, "wait4 should return the child");
    TEST_ASSERT(WIFSIGNALED(wstatus), "a faulting child should report a signal death");
    TEST_ASSERT_EQ(WTERMSIG(wstatus), SIGSEGV, "a null write should raise SIGSEGV");
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
    TEST_ASSERT(tp.tv_sec >= 0, "tv_sec should be non-negative");
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

static int test_uname_basic(void) {
    struct utsname u;
    TEST_ASSERT_EQ(syscall1(SYS_uname, (long)&u), 0, "uname should succeed");
    TEST_ASSERT(strlen(u.sysname) > 0, "sysname should be set");
    TEST_ASSERT(strlen(u.release) > 0, "release should be set");
    TEST_ASSERT(strcmp(u.machine, "x86_64") == 0, "machine should be x86_64");
    return TEST_PASS;
}

static int test_uname_null(void) {
    TEST_ASSERT_EQ(syscall1(SYS_uname, 0), -EFAULT, "uname into NULL should return EFAULT");
    return TEST_PASS;
}

static int test_sysinfo_basic(void) {
    struct sysinfo si;
    TEST_ASSERT_EQ(syscall1(SYS_sysinfo, (long)&si), 0, "sysinfo should succeed");
    TEST_ASSERT(si.totalram > 0, "totalram should be nonzero");
    TEST_ASSERT(si.freeram <= si.totalram, "freeram should not exceed totalram");
    TEST_ASSERT_EQ(si.mem_unit, 1, "mem_unit should be 1 (bytes)");
    TEST_ASSERT(si.procs >= 1, "at least the caller should be counted");
    return TEST_PASS;
}

static int test_sysinfo_null(void) {
    TEST_ASSERT_EQ(syscall1(SYS_sysinfo, 0), -EFAULT, "sysinfo into NULL should return EFAULT");
    return TEST_PASS;
}

static int test_getrlimit_nofile(void) {
    struct rlimit rl;
    TEST_ASSERT_EQ(syscall2(SYS_getrlimit, RLIMIT_NOFILE, (long)&rl), 0, "getrlimit NOFILE should succeed");
    TEST_ASSERT(rl.rlim_cur > 0 && rl.rlim_cur <= rl.rlim_max, "NOFILE soft limit should be positive and <= hard");
    return TEST_PASS;
}

static int test_getrlimit_invalid_resource(void) {
    struct rlimit rl;
    TEST_ASSERT_EQ(syscall2(SYS_getrlimit, 999, (long)&rl), -EINVAL, "getrlimit unknown resource should return EINVAL");
    return TEST_PASS;
}

static int test_getrlimit_null(void) {
    TEST_ASSERT_EQ(syscall2(SYS_getrlimit, RLIMIT_NOFILE, 0), -EFAULT, "getrlimit into NULL should return EFAULT");
    return TEST_PASS;
}

static int test_setrlimit_roundtrip(void) {
    struct rlimit before, lower, after;
    TEST_ASSERT_EQ(syscall2(SYS_getrlimit, RLIMIT_NOFILE, (long)&before), 0, "getrlimit should succeed");
    lower = before;
    lower.rlim_cur = before.rlim_cur > 1 ? before.rlim_cur - 1 : before.rlim_cur;
    TEST_ASSERT_EQ(syscall2(SYS_setrlimit, RLIMIT_NOFILE, (long)&lower), 0, "setrlimit lowering soft limit should succeed");
    TEST_ASSERT_EQ(syscall2(SYS_getrlimit, RLIMIT_NOFILE, (long)&after), 0, "getrlimit should succeed after set");
    TEST_ASSERT_EQ(after.rlim_cur, lower.rlim_cur, "soft limit should read back as set");
    TEST_ASSERT_EQ(syscall2(SYS_setrlimit, RLIMIT_NOFILE, (long)&before), 0, "restoring the original limit should succeed");
    return TEST_PASS;
}

static int test_setrlimit_cur_above_max(void) {
    struct rlimit rl = { 10, 5 };
    TEST_ASSERT_EQ(syscall2(SYS_setrlimit, RLIMIT_NOFILE, (long)&rl), -EINVAL, "soft limit above hard limit should return EINVAL");
    return TEST_PASS;
}

static int test_setrlimit_raise_hard_eperm(void) {
    struct rlimit rl = { 1, RLIM_INFINITY };
    TEST_ASSERT_EQ(syscall2(SYS_setrlimit, RLIMIT_NOFILE, (long)&rl), -EPERM, "raising the hard limit should return EPERM");
    return TEST_PASS;
}

static void sig_test_handler(int sig) {
    (void)sig;
}

static int test_rt_sigaction_roundtrip(void) {
    struct kernel_sigaction act = { (uint64_t)(uintptr_t)sig_test_handler, SA_RESTART, 0, 0 };
    struct kernel_sigaction now = { 0, 0, 0, 0 };
    struct kernel_sigaction dfl = { SIGACT_DFL, 0, 0, 0 };
    TEST_ASSERT_EQ(syscall4(SYS_rt_sigaction, SIGUSR1, (long)&act, 0, 8), 0, "rt_sigaction should accept a user handler");
    TEST_ASSERT_EQ(syscall4(SYS_rt_sigaction, SIGUSR1, 0, (long)&now, 8), 0, "rt_sigaction should report the disposition");
    TEST_ASSERT(now.sa_handler == act.sa_handler, "handler should read back as set");
    TEST_ASSERT(now.sa_flags == SA_RESTART, "flags should read back as set");
    TEST_ASSERT_EQ(syscall4(SYS_rt_sigaction, SIGUSR1, (long)&dfl, 0, 8), 0, "rt_sigaction should restore SIG_DFL");
    return TEST_PASS;
}

static int test_rt_sigaction_unblockable(void) {
    struct kernel_sigaction act = { SIGACT_IGN, 0, 0, 0 };
    TEST_ASSERT_EQ(syscall4(SYS_rt_sigaction, SIGKILL, (long)&act, 0, 8), -EINVAL, "SIGKILL cannot be caught or ignored");
    TEST_ASSERT_EQ(syscall4(SYS_rt_sigaction, SIGSTOP, (long)&act, 0, 8), -EINVAL, "SIGSTOP cannot be caught or ignored");
    return TEST_PASS;
}

static int test_rt_sigaction_bad_signal(void) {
    TEST_ASSERT_EQ(syscall4(SYS_rt_sigaction, 0, 0, 0, 8), -EINVAL, "signal 0 is not valid");
    TEST_ASSERT_EQ(syscall4(SYS_rt_sigaction, 65, 0, 0, 8), -EINVAL, "signal 65 is not valid");
    return TEST_PASS;
}

static int test_rt_sigaction_bad_sigsetsize(void) {
    TEST_ASSERT_EQ(syscall4(SYS_rt_sigaction, SIGUSR1, 0, 0, 16), -EINVAL, "a sigset size other than 8 should return EINVAL");
    return TEST_PASS;
}

static int test_rt_sigaction_bad_handler(void) {
    struct kernel_sigaction act = { 0xffff800000000000ULL, 0, 0, 0 };
    TEST_ASSERT_EQ(syscall4(SYS_rt_sigaction, SIGUSR1, (long)&act, 0, 8), -EINVAL, "a kernel address is not a valid handler");
    return TEST_PASS;
}

static int test_rt_sigprocmask_block_roundtrip(void) {
    sigset_t bit = 1ULL << (SIGUSR2 - 1);
    sigset_t old = 0, now = 0, restore;
    TEST_ASSERT_EQ(syscall4(SYS_rt_sigprocmask, SIG_BLOCK, (long)&bit, (long)&old, 8), 0, "SIG_BLOCK should succeed");
    TEST_ASSERT_EQ(syscall4(SYS_rt_sigprocmask, SIG_BLOCK, 0, (long)&now, 8), 0, "a NULL set should only read the mask");
    TEST_ASSERT((now & bit) != 0, "SIGUSR2 should be blocked after SIG_BLOCK");
    TEST_ASSERT_EQ(syscall4(SYS_rt_sigprocmask, SIG_UNBLOCK, (long)&bit, 0, 8), 0, "SIG_UNBLOCK should succeed");
    TEST_ASSERT_EQ(syscall4(SYS_rt_sigprocmask, SIG_BLOCK, 0, (long)&now, 8), 0, "mask should be readable after unblock");
    TEST_ASSERT((now & bit) == 0, "SIGUSR2 should be unblocked after SIG_UNBLOCK");
    restore = old;
    TEST_ASSERT_EQ(syscall4(SYS_rt_sigprocmask, SIG_SETMASK, (long)&restore, 0, 8), 0, "SIG_SETMASK should restore the original mask");
    return TEST_PASS;
}

static int test_rt_sigprocmask_unblockable(void) {
    sigset_t old = 0, want = (1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)), now = ~0ULL;
    TEST_ASSERT_EQ(syscall4(SYS_rt_sigprocmask, SIG_BLOCK, 0, (long)&old, 8), 0, "reading the mask should succeed");
    TEST_ASSERT_EQ(syscall4(SYS_rt_sigprocmask, SIG_SETMASK, (long)&want, 0, 8), 0, "SIG_SETMASK should succeed");
    TEST_ASSERT_EQ(syscall4(SYS_rt_sigprocmask, SIG_BLOCK, 0, (long)&now, 8), 0, "reading the mask should succeed");
    TEST_ASSERT((now & want) == 0, "SIGKILL and SIGSTOP must never be blocked");
    TEST_ASSERT_EQ(syscall4(SYS_rt_sigprocmask, SIG_SETMASK, (long)&old, 0, 8), 0, "mask should be restored");
    return TEST_PASS;
}

static int test_rt_sigprocmask_bad_how(void) {
    sigset_t set = 0;
    TEST_ASSERT_EQ(syscall4(SYS_rt_sigprocmask, 99, (long)&set, 0, 8), -EINVAL, "an unknown how should return EINVAL");
    return TEST_PASS;
}

static int test_rt_sigpending_empty(void) {
    sigset_t pending = ~0ULL;
    TEST_ASSERT_EQ(syscall2(SYS_rt_sigpending, (long)&pending, 8), 0, "rt_sigpending should succeed");
    TEST_ASSERT_EQ(pending, 0, "nothing has been raised, so nothing should be pending");
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
        { "execve_empty_env", test_execve_empty_env, false },
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
        { "uname_basic", test_uname_basic, false },
        { "uname_null", test_uname_null, false },
        { "sysinfo_basic", test_sysinfo_basic, false },
        { "sysinfo_null", test_sysinfo_null, false },
        { "rt_sigaction_roundtrip", test_rt_sigaction_roundtrip, false },
        { "rt_sigaction_unblockable", test_rt_sigaction_unblockable, false },
        { "rt_sigaction_bad_signal", test_rt_sigaction_bad_signal, false },
        { "rt_sigaction_bad_sigsetsize", test_rt_sigaction_bad_sigsetsize, false },
        { "rt_sigaction_bad_handler", test_rt_sigaction_bad_handler, false },
        { "rt_sigprocmask_block_roundtrip", test_rt_sigprocmask_block_roundtrip, false },
        { "rt_sigprocmask_unblockable", test_rt_sigprocmask_unblockable, false },
        { "rt_sigprocmask_bad_how", test_rt_sigprocmask_bad_how, false },
        { "rt_sigpending_empty", test_rt_sigpending_empty, false },
        { "signal_handler_runs_on_return", test_signal_handler_runs_on_return, false },
        { "signal_blocked_until_unblocked", test_signal_blocked_until_unblocked, false },
        { "wait4_unknown_option", test_wait4_unknown_option, false },
        { "wait4_stop_options_accepted", test_wait4_stop_options_accepted, false },
        { "signal_default_terminates", test_signal_default_terminates, false },
        { "kill_other_process_handler", test_kill_other_process_handler, false },
        { "kill_sigkill_other", test_kill_sigkill_other, false },
        { "fault_raises_sigsegv", test_fault_raises_sigsegv, false },
        { "signal_interrupts_busy_loop", test_signal_interrupts_busy_loop, false },
        { "getrlimit_nofile", test_getrlimit_nofile, false },
        { "getrlimit_invalid_resource", test_getrlimit_invalid_resource, false },
        { "getrlimit_null", test_getrlimit_null, false },
        { "setrlimit_roundtrip", test_setrlimit_roundtrip, false },
        { "setrlimit_cur_above_max", test_setrlimit_cur_above_max, false },
        { "setrlimit_raise_hard_eperm", test_setrlimit_raise_hard_eperm, false },
        { "clock_gettime_monotonic", test_clock_gettime_monotonic, false },
        { "clock_gettime_invalid", test_clock_gettime_invalid, false },
        { "getuid_nonnegative", test_getuid_nonnegative, false },
        { "getgid_nonnegative", test_getgid_nonnegative, false },
        { "arch_prctl_get_fs", test_arch_prctl_get_fs, false },
        { "arch_prctl_get_gs", test_arch_prctl_get_gs, false },
    },
    .num_cases = 55,
};