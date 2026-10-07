/*
 * test_memory_syscalls.c - Memory management syscall tests
 *
 * Tests for syscalls 32-38: mmap, munmap, mprotect, mremap, madvise, brk, mlock
 */

#include "test_framework.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* include libc time.h for CLOCK_* constants */
#include "time.h"

/* Syscall numbers - mmap, munmap, mprotect, brk, madvise are in uapi/syscall.h via <unistd.h> */
/* mremap, mlock are planned for COMPLETE version (TDD) - define locally */
#ifndef SYS_mremap
#define SYS_mremap   35
#define SYS_mlock    38
#endif

/* MAP_* flags - defined in uapi/syscall.h via <unistd.h> */
/* Do NOT redefine: MAP_PRIVATE, MAP_SHARED, MAP_ANONYMOUS, MAP_FIXED, MAP_POPULATE, MAP_GROWSDOWN */

/* PROT_* flags - defined in uapi/syscall.h via <unistd.h> */
/* Do NOT redefine: PROT_NONE, PROT_READ, PROT_WRITE, PROT_EXEC */

/* MADV_* flags - not in uapi/syscall.h, define locally */
#define MADV_NORMAL    0
#define MADV_SEQUENTIAL 2
#define MADV_RANDOM    1
#define MADV_WILLNEED  3
#define MADV_DONTNEED  4
#define MADV_FREE      8
#define MADV_HUGEPAGE  14
#define MADV_NOHUGEPAGE 15

/* MAP_FAILED from Linux */
#define MAP_FAILED ((void*)-1)

/* errno values - use from errno.h included above or define locally */
#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif
#ifndef EACCES
#define EACCES 13
#endif
#ifndef EFAULT
#define EFAULT 14
#endif

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

static inline long syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    long ret;
    register long r10 asm("r10") = a3;
    register long r8 asm("r8") = a4;
    register long r9 asm("r9") = a5;
    asm volatile("push %%r10; mov %6, %%r10; syscall; pop %%r10"
                 : "=a"(ret)
                 : "a"(n), "D"(a1), "S"(a2), "r"(r10), "r"(r8), "r"(r9), "r"(a6)
                 : "rcx", "r11", "memory");
    return ret;
}

#define PAGE_SIZE 4096

/* ===== mmap / munmap ===== */

static int test_mmap_anonymous_basic(void) {
    void *addr = (void*)syscall6(SYS_mmap, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    TEST_ASSERT(addr != MAP_FAILED, "mmap anonymous should succeed");
    TEST_ASSERT(((uintptr_t)addr % PAGE_SIZE) == 0, "mmap should return page-aligned address");

    /* Write to it */
    *(volatile char*)addr = 42;
    TEST_ASSERT_EQ(*(volatile char*)addr, 42, "should be able to write to mmap'd memory");

    long ret = syscall2(SYS_munmap, (long)addr, PAGE_SIZE);
    TEST_ASSERT_EQ(ret, 0, "munmap should succeed");
    return TEST_PASS;
}

static int test_mmap_multiple_pages(void) {
    size_t size = 10 * PAGE_SIZE;
    void *addr = (void*)syscall6(SYS_mmap, 0, size, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    TEST_ASSERT(addr != MAP_FAILED, "mmap multiple pages should succeed");

    /* Write to first and last page */
    *(volatile char*)addr = 1;
    *(volatile char*)((char*)addr + size - 1) = 2;

    long ret = syscall2(SYS_munmap, (long)addr, size);
    TEST_ASSERT_EQ(ret, 0, "munmap multiple pages should succeed");
    return TEST_PASS;
}

static int test_mmap_prot_none(void) {
    void *addr = (void*)syscall6(SYS_mmap, 0, PAGE_SIZE, PROT_NONE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    TEST_ASSERT(addr != MAP_FAILED, "mmap PROT_NONE should succeed");

    /* Access should fault - we can't easily test this without signal handler */
    /* Just verify mapping exists */
    long ret = syscall2(SYS_munmap, (long)addr, PAGE_SIZE);
    TEST_ASSERT_EQ(ret, 0, "munmap should succeed");
    return TEST_PASS;
}

static int test_mmap_invalid_flags(void) {
    void *addr = (void*)syscall6(SYS_mmap, 0, PAGE_SIZE, PROT_READ,
                                 MAP_PRIVATE | MAP_SHARED, -1, 0);
    TEST_ASSERT_EQ((long)addr, -EINVAL, "mmap with both PRIVATE and SHARED should fail");
    return TEST_PASS;
}

static int test_mmap_zero_size(void) {
    void *addr = (void*)syscall6(SYS_mmap, 0, 0, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    TEST_ASSERT_EQ((long)addr, -EINVAL, "mmap zero size should fail");
    return TEST_PASS;
}

static int test_munmap_invalid_address(void) {
    long ret = syscall2(SYS_munmap, 0x1000, PAGE_SIZE);
    TEST_ASSERT_EQ(ret, -EINVAL, "munmap invalid address should fail");
    return TEST_PASS;
}

static int test_munmap_partial(void) {
    void *addr = (void*)syscall6(SYS_mmap, 0, 4 * PAGE_SIZE, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    TEST_ASSERT(addr != MAP_FAILED, "mmap should succeed");

    long ret = syscall2(SYS_munmap, (long)addr, 2 * PAGE_SIZE);
    TEST_ASSERT_EQ(ret, 0, "munmap partial should succeed");

    /* Remaining should still be accessible */
    *(volatile char*)((char*)addr + 3 * PAGE_SIZE) = 42;

    ret = syscall2(SYS_munmap, (long)((char*)addr + 2 * PAGE_SIZE), 2 * PAGE_SIZE);
    TEST_ASSERT_EQ(ret, 0, "munmap remaining should succeed");
    return TEST_PASS;
}

/* ===== mprotect ===== */

static int test_mprotect_readonly(void) {
    void *addr = (void*)syscall6(SYS_mmap, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    TEST_ASSERT(addr != MAP_FAILED, "mmap should succeed");

    long ret = syscall3(SYS_mprotect, (long)addr, PAGE_SIZE, PROT_READ);
    TEST_ASSERT_EQ(ret, 0, "mprotect to READ should succeed");

    /* Write should fault - can't test easily without signal handler */

    ret = syscall2(SYS_munmap, (long)addr, PAGE_SIZE);
    TEST_ASSERT_EQ(ret, 0, "munmap should succeed");
    return TEST_PASS;
}

static int test_mprotect_noaccess(void) {
    void *addr = (void*)syscall6(SYS_mmap, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    TEST_ASSERT(addr != MAP_FAILED, "mmap should succeed");

    long ret = syscall3(SYS_mprotect, (long)addr, PAGE_SIZE, PROT_NONE);
    TEST_ASSERT_EQ(ret, 0, "mprotect to NONE should succeed");

    ret = syscall2(SYS_munmap, (long)addr, PAGE_SIZE);
    TEST_ASSERT_EQ(ret, 0, "munmap should succeed");
    return TEST_PASS;
}

static int test_mprotect_invalid_addr(void) {
    long ret = syscall3(SYS_mprotect, 0x1000, PAGE_SIZE, PROT_READ);
    TEST_ASSERT_EQ(ret, -EINVAL, "mprotect invalid address should fail");
    return TEST_PASS;
}

/* ===== mremap ===== */

static int test_mremap_expand(void) {
    void *addr = (void*)syscall6(SYS_mmap, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    TEST_ASSERT(addr != MAP_FAILED, "mmap should succeed");

    *(volatile char*)addr = 42;

    void *new_addr = (void*)syscall4(SYS_mremap, (long)addr, PAGE_SIZE, 2 * PAGE_SIZE, 0);
    TEST_ASSERT(new_addr != MAP_FAILED, "mremap expand should succeed");

    TEST_ASSERT_EQ(*(volatile char*)new_addr, 42, "data should be preserved after mremap");

    long ret = syscall2(SYS_munmap, (long)new_addr, 2 * PAGE_SIZE);
    TEST_ASSERT_EQ(ret, 0, "munmap should succeed");
    return TEST_PASS;
}

static int test_mremap_shrink(void) {
    void *addr = (void*)syscall6(SYS_mmap, 0, 4 * PAGE_SIZE, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    TEST_ASSERT(addr != MAP_FAILED, "mmap should succeed");

    *(volatile char*)addr = 1;
    *(volatile char*)((char*)addr + 3 * PAGE_SIZE) = 2;

    void *new_addr = (void*)syscall4(SYS_mremap, (long)addr, 4 * PAGE_SIZE, 2 * PAGE_SIZE, 0);
    TEST_ASSERT(new_addr != MAP_FAILED, "mremap shrink should succeed");

    TEST_ASSERT_EQ(*(volatile char*)new_addr, 1, "first byte preserved");

    long ret = syscall2(SYS_munmap, (long)new_addr, 2 * PAGE_SIZE);
    TEST_ASSERT_EQ(ret, 0, "munmap should succeed");
    return TEST_PASS;
}

static int test_mremap_invalid(void) {
    long ret = (long)syscall4(SYS_mremap, 0x1000, PAGE_SIZE, 2 * PAGE_SIZE, 0);
    TEST_ASSERT_EQ(ret, -EINVAL, "mremap invalid address should fail");
    return TEST_PASS;
}

/* ===== madvise ===== */

static int test_madvise_normal(void) {
    void *addr = (void*)syscall6(SYS_mmap, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    TEST_ASSERT(addr != MAP_FAILED, "mmap should succeed");

    long ret = syscall3(SYS_madvise, (long)addr, PAGE_SIZE, MADV_NORMAL);
    TEST_ASSERT_EQ(ret, 0, "madvise NORMAL should succeed");

    ret = syscall2(SYS_munmap, (long)addr, PAGE_SIZE);
    TEST_ASSERT_EQ(ret, 0, "munmap should succeed");
    return TEST_PASS;
}

static int test_madvise_dontneed(void) {
    void *addr = (void*)syscall6(SYS_mmap, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    TEST_ASSERT(addr != MAP_FAILED, "mmap should succeed");

    *(volatile char*)addr = 42;

    long ret = syscall3(SYS_madvise, (long)addr, PAGE_SIZE, MADV_DONTNEED);
    TEST_ASSERT_EQ(ret, 0, "madvise DONTNEED should succeed");

    /* Page should be zeroed on next access */
    /* Note: behavior may vary - just test it doesn't crash */

    ret = syscall2(SYS_munmap, (long)addr, PAGE_SIZE);
    TEST_ASSERT_EQ(ret, 0, "munmap should succeed");
    return TEST_PASS;
}

static int test_madvise_invalid(void) {
    void *addr = (void*)syscall6(SYS_mmap, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    TEST_ASSERT(addr != MAP_FAILED, "mmap should succeed");

    long ret = syscall3(SYS_madvise, (long)addr, PAGE_SIZE, 999);
    TEST_ASSERT_EQ(ret, -EINVAL, "madvise invalid advice should fail");

    ret = syscall2(SYS_munmap, (long)addr, PAGE_SIZE);
    TEST_ASSERT_EQ(ret, 0, "munmap should succeed");
    return TEST_PASS;
}

/* ===== brk ===== */

static int test_brk_get_current(void) {
    void *cur = (void*)syscall1(SYS_brk, 0);
    TEST_ASSERT(cur != NULL, "brk(0) should return current brk");
    TEST_ASSERT(((uintptr_t)cur % PAGE_SIZE) == 0, "brk should be page-aligned");
    return TEST_PASS;
}

static int test_brk_expand(void) {
    void *cur = (void*)syscall1(SYS_brk, 0);
    void *new_brk = (void*)syscall1(SYS_brk, (long)cur + PAGE_SIZE);
    TEST_ASSERT(new_brk == cur + PAGE_SIZE, "brk expand should return new brk");

    /* Write to new memory */
    *(volatile char*)cur = 42;
    TEST_ASSERT_EQ(*(volatile char*)cur, 42, "should be able to write to expanded heap");

    /* Shrink back */
    void *shrunk = (void*)syscall1(SYS_brk, (long)cur);
    TEST_ASSERT(shrunk == cur, "brk shrink should return old brk");
    return TEST_PASS;
}

static int test_brk_shrink_below_start(void) {
    void *cur = (void*)syscall1(SYS_brk, 0);
    void *new_brk = (void*)syscall1(SYS_brk, (long)cur - PAGE_SIZE);
    /* Should return current brk (can't shrink below start) */
    TEST_ASSERT(new_brk == cur, "brk shrink below start should not change brk");
    return TEST_PASS;
}

/* ===== mlock ===== */

static int test_mlock_basic(void) {
    void *addr = (void*)syscall6(SYS_mmap, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    TEST_ASSERT(addr != MAP_FAILED, "mmap should succeed");

    long ret = syscall2(SYS_mlock, (long)addr, PAGE_SIZE);
    /* May fail if no CAP_IPC_LOCK or RLIMIT_MEMLOCK */
    TEST_ASSERT(ret == 0 || ret == -EPERM || ret == -ENOMEM,
                "mlock should succeed or return appropriate error");

    ret = syscall2(SYS_munmap, (long)addr, PAGE_SIZE);
    TEST_ASSERT_EQ(ret, 0, "munmap should succeed");
    return TEST_PASS;
}

static int test_mlock_invalid_addr(void) {
    long ret = syscall2(SYS_mlock, 0x1000, PAGE_SIZE);
    TEST_ASSERT_EQ(ret, -EINVAL, "mlock invalid address should fail");
    return TEST_PASS;
}

/* ===== Test Suite Registration ===== */

struct test_suite test_memory_syscalls = {
    .name = "Memory Syscalls",
    .cases = (struct test_case[]) {
        { "mmap_anonymous_basic", test_mmap_anonymous_basic, false },
        { "mmap_multiple_pages", test_mmap_multiple_pages, false },
        { "mmap_prot_none", test_mmap_prot_none, false },
        { "mmap_invalid_flags", test_mmap_invalid_flags, false },
        { "mmap_zero_size", test_mmap_zero_size, false },
        { "munmap_invalid_address", test_munmap_invalid_address, false },
        { "munmap_partial", test_munmap_partial, false },
        { "mprotect_readonly", test_mprotect_readonly, false },
        { "mprotect_noaccess", test_mprotect_noaccess, false },
        { "mprotect_invalid_addr", test_mprotect_invalid_addr, false },
        { "mremap_expand", test_mremap_expand, false },
        { "mremap_shrink", test_mremap_shrink, false },
        { "mremap_invalid", test_mremap_invalid, false },
        { "madvise_normal", test_madvise_normal, false },
        { "madvise_dontneed", test_madvise_dontneed, false },
        { "madvise_invalid", test_madvise_invalid, false },
        { "brk_get_current", test_brk_get_current, false },
        { "brk_expand", test_brk_expand, false },
        { "brk_shrink_below_start", test_brk_shrink_below_start, false },
        { "mlock_basic", test_mlock_basic, false },
        { "mlock_invalid_addr", test_mlock_invalid_addr, false },
    },
    .num_cases = 21,
};