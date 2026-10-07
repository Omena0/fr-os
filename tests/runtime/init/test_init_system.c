/*
 * test_userspace.c - Userspace libc and syscall tests
 *
 * Tests for libc functions and syscalls from userspace.
 */

#include "test_framework.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* errno values from libc */
#define ERANGE      1
#define ENOMEM      12
#define EINVAL      22

/* Implement missing string functions */
char *strcpy(char *dst, const char *src) {
    char *ret = dst;
    while ((*dst++ = *src++))
        ;
    return ret;
}

char *strncpy(char *dst, const char *src, size_t n) {
    char *ret = dst;
    while (n > 0 && *src) {
        *dst++ = *src++;
        n--;
    }
    while (n > 0) {
        *dst++ = '\0';
        n--;
    }
    return ret;
}

char *strcat(char *dst, const char *src) {
    char *ret = dst;
    while (*dst)
        dst++;
    while ((*dst++ = *src++))
        ;
    return ret;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *ret = dst;
    while (*dst)
        dst++;
    while (n > 0 && *src) {
        *dst++ = *src++;
        n--;
    }
    *dst = '\0';
    return ret;
}

/* ===== String functions ===== */

static int test_strlen_basic(void) {
    const char *s = "hello";
    size_t len = strlen(s);
    TEST_ASSERT_EQ(len, 5, "strlen should return length of string");
    return TEST_PASS;
}

static int test_strnlen_basic(void) {
    const char *s = "hello world";
    size_t len = strnlen(s, 5);
    TEST_ASSERT_EQ(len, 5, "strnlen with limit should return limited length");
    
    len = strnlen(s, 100);
    TEST_ASSERT_EQ(len, 11, "strnlen without limit should return full length");
    return TEST_PASS;
}

static int test_strcmp_basic(void) {
    TEST_ASSERT_EQ(strcmp("abc", "abc"), 0, "strcmp should return 0 for equal strings");
    TEST_ASSERT_LT(strcmp("a", "b"), 0, "strcmp should return negative for 'a' < 'b'");
    TEST_ASSERT_GT(strcmp("b", "a"), 0, "strcmp should return positive for 'b' > 'a'");
    return TEST_PASS;
}

static int test_strncmp_basic(void) {
    TEST_ASSERT_EQ(strncmp("abc", "abc", 3), 0, "strncmp should return 0 for equal strings");
    TEST_ASSERT_LT(strncmp("ab", "ac", 2), 0, "strncmp should return negative for 'ab' < 'ac'");
    TEST_ASSERT_GT(strncmp("ac", "ab", 2), 0, "strncmp should return positive for 'ac' > 'ab'");
    TEST_ASSERT_EQ(strncmp("ab", "abc", 2), 0, "strncmp should truncate comparison");
    return TEST_PASS;
}

static int test_strcpy_basic(void) {
    char dst[20];
    const char *src = "hello";
    char *ret = strcpy(dst, src);
    TEST_ASSERT(ret == dst, "strcpy should return dst");
    TEST_ASSERT_EQ(strcmp(dst, src), 0, "strcpy should copy string");
    return TEST_PASS;
}

static int test_strncpy_basic(void) {
    char dst[20];
    const char *src = "hello world";
    char *ret = strncpy(dst, src, 5);
    TEST_ASSERT(ret == dst, "strncpy should return dst");
    TEST_ASSERT_EQ(strlen(dst), 5, "strncpy should copy at most 5 chars");
    TEST_ASSERT_EQ(dst[5], '\0', "strncpy should null-terminate");
    return TEST_PASS;
}

static int test_strdup_basic(void) {
    char *s = strdup("hello");
    TEST_ASSERT(s != NULL, "strdup should allocate memory");
    TEST_ASSERT_EQ(strcmp(s, "hello"), 0, "strdup should copy string");
    free(s);
    return TEST_PASS;
}

static int test_strchr_basic(void) {
    const char *s = "hello world";
    char *ret = strchr(s, 'o');
    TEST_ASSERT_EQ(ret - s, 4, "strchr should find 'o' at position 4");
    TEST_ASSERT(strchr(s, 'x') == NULL, "strchr should return NULL for not found");
    return TEST_PASS;
}

static int test_strstr_basic(void) {
    const char *s = "hello world";
    const char *ret = strstr(s, "lo wo");
    TEST_ASSERT_EQ(ret - s, 3, "strstr should find substring at position 3");
    TEST_ASSERT(strstr(s, "not") == NULL, "strstr should return NULL for not found");
    return TEST_PASS;
}

static int test_strchr_null(void) {
    const char *s = "hello";
    char *ret = strchr(s, '\0');
    TEST_ASSERT_EQ(ret - s, 5, "strchr should find null at end");
    return TEST_PASS;
}

/* ===== Memory functions ===== */

static int test_memcpy_basic(void) {
    char src[20] = "hello";
    char dst[20];
    void *ret = memcpy(dst, src, 6);
    TEST_ASSERT_EQ((long long)ret, (long long)dst, "memcpy should return dst");
    TEST_ASSERT_EQ(strcmp(dst, src), 0, "memcpy should copy memory");
    return TEST_PASS;
}

static int test_memmove_overlap_basic(void) {
    char src[] = "abcdefghij";
    char dst[20];
    memcpy(dst, src, 11);
    
    memmove(dst + 3, dst + 1, 5); /* "defgh" -> "cdef" */
    TEST_ASSERT_EQ(memcmp(dst + 3, "cdefg", 5), 0, "memmove with overlap should work");
    return TEST_PASS;
}

static int test_memset_basic(void) {
    char arr[10];
    void *ret = memset(arr, 0xaa, 10);
    TEST_ASSERT_EQ((long long)ret, (long long)arr, "memset should return dst");
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_EQ(arr[i], 0xaa, "memset should fill with pattern");
    }
    return TEST_PASS;
}

static int test_memcmp_basic(void) {
    char a1[] = "abc", a2[] = "abc";
    TEST_ASSERT_EQ(memcmp(a1, a2, 3), 0, "memcmp equal strings");
    char b1[] = "abc", b2[] = "abd";
    TEST_ASSERT_LT(memcmp(b1, b2, 3), 0, "memcmp 'abc' < 'abd'");
    char c1[] = "abd", c2[] = "abc";
    TEST_ASSERT_GT(memcmp(c1, c2, 3), 0, "memcmp 'abd' > 'abc'");
    return TEST_PASS;
}

static int test_malloc_basic(void) {
    void *ptr = malloc(1024);
    TEST_ASSERT(ptr != NULL, "malloc should allocate");
    /* Write to it to ensure it's usable */
    volatile char *c = (volatile char *)ptr;
    *c = 'x';
    free(ptr);
    return TEST_PASS;
}

static int test_realloc_basic(void) {
    char *ptr = malloc(10);
    TEST_ASSERT(ptr != NULL, "malloc should allocate");
    ptr = realloc(ptr, 20);
    TEST_ASSERT(ptr != NULL, "realloc should resize");
    free(ptr);
    return TEST_PASS;
}

static int test_calloc_basic(void) {
    int *ptr = calloc(100, sizeof(int));
    TEST_ASSERT(ptr != NULL, "calloc should allocate");
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_EQ(ptr[i], 0, "calloc should zero-initialize");
    }
    free(ptr);
    return TEST_PASS;
}

/* ===== String utilities ===== */

static int test_strcat_basic(void) {
    char dst[20] = "hello ";
    const char *src = "world";
    char *ret = strcat(dst, src);
    TEST_ASSERT(ret == dst, "strcat should return dst");
    TEST_ASSERT_EQ(strcmp(dst, "hello world"), 0, "strcat should concatenate");
    return TEST_PASS;
}

static int test_strncat_basic(void) {
    char dst[20] = "hello ";
    const char *src = "world";
    char *ret = strncat(dst, src, 4);
    TEST_ASSERT(ret == dst, "strncat should return dst");
    TEST_ASSERT_EQ(strncmp(dst, "hello wor", 8), 0, "strncat should concatenate limited");
    return TEST_PASS;
}

/* ===== File operations ===== */

static int test_fread_basic(void) {
    /* read() - cannot easily test without actual file descriptor */
    return TEST_PASS;
}

static int test_fwrite_basic(void) {
    /* write() - can test with stdout */
    const char *msg = "test\n";
    ssize_t ret = write(STDOUT_FILENO, msg, 5);
    TEST_ASSERT_EQ(ret, 5, "write to stdout should succeed");
    return TEST_PASS;
}

static int test_fread_line_basic(void) {
    /* readline() - cannot easily test without actual file descriptor */
    return TEST_PASS;
}

static int test_fputs_basic(void) {
    const char *msg = "test\n";
    int ret = puts(msg);
    TEST_ASSERT(ret == 4, "puts should return length without newline");
    return TEST_PASS;
}

static int test_getchar_basic(void) {
    /* getchar() - returns EOF when no input */
    /* Skip in test context */
    return TEST_PASS;
}

static int test_getline_basic(void) {
    /* getline() - returns -1 on EOF when no input */
    /* Skip in test context */
    return TEST_PASS;
}

/* ===== Time functions ===== */

static int test_clock_basic(void) {
    struct timespec ts;
    long ret = clock_gettime(CLOCK_MONOTONIC, &ts);
    TEST_ASSERT(ret == 0, "clock_gettime should succeed");
    TEST_ASSERT(ts.tv_sec >= 0, "tv_sec should be non-negative");
    TEST_ASSERT(ts.tv_nsec >= 0 && ts.tv_nsec < 1000000000, "tv_nsec should be valid");
    return TEST_PASS;
}

static int test_clock_realtime(void) {
    struct timespec ts;
    long ret = clock_gettime(CLOCK_REALTIME, &ts);
    /* CLOCK_REALTIME may or may not be implemented */
    TEST_ASSERT(ret == 0 || ret == -EINVAL, "clock_gettime REALTIME should succeed or return ENOSYS");
    return TEST_PASS;
}

/* ===== Process operations ===== */

static int test_getpid_basic(void) {
    pid_t pid = getpid();
    TEST_ASSERT(pid > 0, "getpid should return positive PID");
    return TEST_PASS;
}

static int test_getppid_basic(void) {
    pid_t ppid = getppid();
    TEST_ASSERT(ppid >= 0, "getppid should return non-negative PPID");
    return TEST_PASS;
}

static int test_exit_basic(void) {
    /* exit() cannot be tested directly - it would terminate the process */
    return TEST_PASS;
}

static int test_abort_basic(void) {
    /* abort() cannot be tested directly - it would terminate the process */
    return TEST_PASS;
}

/* ===== Test Suite Registration ===== */

struct test_suite test_init_system = {
    .name = "Userspace Tests",
    .cases = (struct test_case[]) {
        { "strlen_basic", test_strlen_basic, false },
        { "strnlen_basic", test_strnlen_basic, false },
        { "strcmp_basic", test_strcmp_basic, false },
        { "strncmp_basic", test_strncmp_basic, false },
        { "strcpy_basic", test_strcpy_basic, false },
        { "strncpy_basic", test_strncpy_basic, false },
        { "strdup_basic", test_strdup_basic, false },
        { "strchr_basic", test_strchr_basic, false },
        { "strstr_basic", test_strstr_basic, false },
        { "strchr_null", test_strchr_null, false },
        { "memcpy_basic", test_memcpy_basic, false },
        { "memmove_overlap_basic", test_memmove_overlap_basic, false },
        { "memset_basic", test_memset_basic, false },
        { "memcmp_basic", test_memcmp_basic, false },
        { "malloc_basic", test_malloc_basic, false },
        { "realloc_basic", test_realloc_basic, false },
        { "calloc_basic", test_calloc_basic, false },
        { "strcat_basic", test_strcat_basic, false },
        { "strncat_basic", test_strncat_basic, false },
        { "fread_basic", test_fread_basic, false },
        { "fwrite_basic", test_fwrite_basic, false },
        { "fread_line_basic", test_fread_line_basic, false },
        { "fputs_basic", test_fputs_basic, false },
        { "getchar_basic", test_getchar_basic, false },
        { "getline_basic", test_getline_basic, false },
        { "clock_basic", test_clock_basic, false },
        { "clock_realtime", test_clock_realtime, false },
        { "getpid_basic", test_getpid_basic, false },
        { "getppid_basic", test_getppid_basic, false },
        { "exit_basic", test_exit_basic, false },
        { "abort_basic", test_abort_basic, false },
    },
    .num_cases = 32,
};