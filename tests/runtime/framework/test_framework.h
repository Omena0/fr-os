#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define TEST_PASS 0
#define TEST_FAIL 1
#define TEST_SKIP 77

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            test_fail(__FILE__, __LINE__, #cond, msg); \
            return TEST_FAIL; \
        } \
    } while (0)

#define TEST_ASSERT_EQ(a, b, msg) \
    do { \
        if ((a) != (b)) { \
            test_fail_eq(__FILE__, __LINE__, #a, #b, a, b, msg); \
            return TEST_FAIL; \
        } \
    } while (0)

#define TEST_ASSERT_NE(a, b, msg) \
    do { \
        if ((a) == (b)) { \
            test_fail_ne(__FILE__, __LINE__, #a, #b, a, b, msg); \
            return TEST_FAIL; \
        } \
    } while (0)

#define TEST_ASSERT_LT(a, b, msg) \
    do { \
        if (!((a) < (b))) { \
            test_fail_rel(__FILE__, __LINE__, #a, #b, a, b, "<", msg); \
            return TEST_FAIL; \
        } \
    } while (0)

#define TEST_ASSERT_LE(a, b, msg) \
    do { \
        if (!((a) <= (b))) { \
            test_fail_rel(__FILE__, __LINE__, #a, #b, a, b, "<=", msg); \
            return TEST_FAIL; \
        } \
    } while (0)

#define TEST_ASSERT_GT(a, b, msg) \
    do { \
        if (!((a) > (b))) { \
            test_fail_rel(__FILE__, __LINE__, #a, #b, a, b, ">", msg); \
            return TEST_FAIL; \
        } \
    } while (0)

#define TEST_ASSERT_GE(a, b, msg) \
    do { \
        if (!((a) >= (b))) { \
            test_fail_rel(__FILE__, __LINE__, #a, #b, a, b, ">=", msg); \
            return TEST_FAIL; \
        } \
    } while (0)

#define TEST_ASSERT_STR_EQ(a, b, msg) \
    do { \
        if (strcmp((a), (b)) != 0) { \
            test_fail_str(__FILE__, __LINE__, #a, #b, a, b, msg); \
            return TEST_FAIL; \
        } \
    } while (0)

#define TEST_SKIP_IF(cond, msg) \
    do { \
        if (cond) { \
            test_skip(__FILE__, __LINE__, msg); \
            return TEST_SKIP; \
        } \
    } while (0)

struct test_case {
    const char *name;
    int (*fn)(void);
    bool skip;
};

struct test_suite {
    const char *name;
    struct test_case *cases;
    size_t num_cases;
    int (*setup)(void);
    int (*teardown)(void);
};

int test_run_suite(const struct test_suite *suite);
int test_get_passed(void);
int test_get_failed(void);
int test_get_skipped(void);
void test_fail(const char *file, int line, const char *cond, const char *msg);
void test_fail_eq(const char *file, int line, const char *a_name, const char *b_name,
                  long long a, long long b, const char *msg);
void test_fail_ne(const char *file, int line, const char *a_name, const char *b_name,
                  long long a, long long b, const char *msg);
void test_fail_rel(const char *file, int line, const char *a_name, const char *b_name,
                   long long a, long long b, const char *op, const char *msg);
void test_fail_str(const char *file, int line, const char *a_name, const char *b_name,
                   const char *a, const char *b, const char *msg);
void test_skip(const char *file, int line, const char *msg);
void test_log(const char *fmt, ...);

#endif