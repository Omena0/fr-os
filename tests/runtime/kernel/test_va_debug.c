/*
 * test_va_debug.c - Diagnostic test for va_list copy behavior.
 *
 * This test runs in the guest and exercises the exact va_list copy chain
 * that test_log() uses: va_start -> va_copy -> vsnprintf -> va_arg.
 * It prints the raw pointer values so we can see whether va_arg through
 * a copied va_list returns the correct arguments.
 */
#include "test_framework.h"
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>

static void dump_va(const char *fmt, ...)
{
    va_list ap;
    va_list ap_copy;

    va_start(ap, fmt);
    va_copy(ap_copy, ap);

    /* Read the first arg through the copy, then through the original. */
    const char *s_copy = va_arg(ap_copy, const char *);
    const char *s_orig = va_arg(ap, const char *);

    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "fmt='%s' s_copy=%p s_orig=%p match=%s",
                     fmt, (void *)s_copy, (void *)s_orig,
                     s_copy == s_orig ? "yes" : "NO");
    write(STDOUT_FILENO, buf, n);
    write(STDOUT_FILENO, "\n", 1);

    va_end(ap_copy);
    va_end(ap);
}

static int test_va_copy_string(void)
{
    dump_va("=== Suite %s: %d passed, %d failed, %d skipped ===",
            "suite_a", 1, 2, 3);
    return TEST_PASS;
}

struct test_suite test_va_debug_suite = {
    .name = "VA Debug",
    .cases = (struct test_case[]) {
        { "va_copy_string", test_va_copy_string, false },
    },
    .num_cases = 1,
};