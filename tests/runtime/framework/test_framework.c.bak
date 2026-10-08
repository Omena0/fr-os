#include "test_framework.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <syscall.h>

static int test_passed = 0;
static int test_failed = 0;
static int test_skipped = 0;
static const char *current_test = NULL;

void test_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    write(STDERR_FILENO, buf, strlen(buf));
    write(STDERR_FILENO, "\n", 1);
}

void test_fail(const char *file, int line, const char *cond, const char *msg) {
    test_failed++;
    test_log("[FAIL] %s:%d: %s failed: %s", file, line, cond, msg ? msg : "");
}

void test_fail_eq(const char *file, int line, const char *a_name, const char *b_name,
                  long long a, long long b, const char *msg) {
    test_failed++;
    test_log("[FAIL] %s:%d: %s == %s failed: %lld != %lld: %s",
             file, line, a_name, b_name, a, b, msg ? msg : "");
}

void test_fail_ne(const char *file, int line, const char *a_name, const char *b_name,
                  long long a, long long b, const char *msg) {
    test_failed++;
    test_log("[FAIL] %s:%d: %s != %s failed: %lld == %lld: %s",
             file, line, a_name, b_name, a, b, msg ? msg : "");
}

void test_fail_rel(const char *file, int line, const char *a_name, const char *b_name,
                   long long a, long long b, const char *op, const char *msg) {
    test_failed++;
    test_log("[FAIL] %s:%d: %s %s %s failed: %lld %s %lld: %s",
             file, line, a_name, op, b_name, a, op, b, msg ? msg : "");
}

void test_fail_str(const char *file, int line, const char *a_name, const char *b_name,
                   const char *a, const char *b, const char *msg) {
    test_failed++;
    test_log("[FAIL] %s:%d: %s == %s failed: \"%s\" != \"%s\": %s",
             file, line, a_name, b_name, a, b, msg ? msg : "");
}

void test_skip(const char *file, int line, const char *msg) {
    test_skipped++;
    test_log("[SKIP] %s:%d: %s", file, line, msg ? msg : "");
}

int test_get_passed(void) { return test_passed; }
int test_get_failed(void) { return test_failed; }
int test_get_skipped(void) { return test_skipped; }

int test_run_suite(const struct test_suite *suite) {
    test_passed = 0;
    test_failed = 0;
    test_skipped = 0;

    test_log("=== Test Suite: %s ===", suite->name);
    
    if (suite->setup) {
        int ret = suite->setup();
        if (ret != TEST_PASS) {
            test_log("Setup failed with %d", ret);
            return ret;
        }
    }
    
    for (size_t i = 0; i < suite->num_cases; i++) {
        const struct test_case *tc = &suite->cases[i];
        current_test = tc->name;
        
        if (tc->skip) {
            test_skipped++;
            test_log("[SKIP] %s (marked skip)", tc->name);
            continue;
        }
        
        test_log("[RUN]  %s", tc->name);
        int ret = tc->fn();
        if (ret == TEST_PASS) {
            test_passed++;
            test_log("[PASS] %s", tc->name);
        } else if (ret == TEST_SKIP) {
            test_skipped++;
        }
    }
    
    if (suite->teardown) {
        suite->teardown();
    }
    
    test_log("=== Suite %s: %d passed, %d failed, %d skipped ===",
             suite->name, test_passed, test_failed, test_skipped);
    
    return test_failed ? TEST_FAIL : TEST_PASS;
}