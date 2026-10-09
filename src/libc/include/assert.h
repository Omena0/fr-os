/*
 * assert.h — the assert macro.
 *
 * assert() is a compile-time no-op when NDEBUG is defined, so it cannot be
 * used for runtime checks that must always fire. When NDEBUG is not defined
 * the failure path calls abort(), which is a noreturn function in libc.
 */
#ifndef ASSERT_H
#define ASSERT_H

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else

extern void __assert_fail(const char *assertion, const char *file,
             int line, const char *function);

#define assert(expr)                                                    \
    ((expr)                                                         \
         ? (void)0                                                   \
         : __assert_fail(#expr, __FILE__, __LINE__, __func__))

#endif /* NDEBUG */

#endif /* ASSERT_H */