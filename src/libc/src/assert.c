/*
 * assert.c — the assert() failure path.
 *
 * Declared in <assert.h> and previously defined nowhere. It linked only because
 * assert() expands to a call the compiler can see is never taken on every
 * current code path, so the undefined reference never reached the linker; the
 * first assert that can fail would have turned a libc invariant violation into
 * a link error. Defining it here, in a file of its own, keeps <assert.h> free
 * of a dependency on stdio.
 *
 * The message goes to fd 2 directly, through the raw write wrapper, rather than
 * through fprintf. assert() must work before stdio's buffers exist, and inside
 * abort(), and a failed assertion is exactly the moment where a formatted print
 * that itself faults would hide the original failure.
 */
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int sys_write(int fd, const void *buf, size_t count);

static void put(const char *s)
{
    size_t n = 0;

    while (s[n])
        n++;
    if (n)
        (void)sys_write(2, s, n);
}

static void put_num(long v)
{
    char buf[24];
    int i = 0;
    int j = 0;
    unsigned long mag = v < 0 ? (unsigned long)(-(v + 1)) + 1
                  : (unsigned long)v;

    if (mag == 0)
        buf[i++] = '0';
    while (mag) {
        buf[i++] = (char)('0' + (mag % 10));
        mag /= 10;
    }
    if (v < 0)
        buf[i++] = '-';
    /* Reverse the digits in place */
    for (j = 0; j < i / 2; j++) {
        char tmp = buf[j];
        buf[j] = buf[i - 1 - j];
        buf[i - 1 - j] = tmp;
    }
    if (i)
        (void)sys_write(2, buf, i);
}

void __assert_fail(const char *assertion, const char *file, int line,
           const char *function)
{
    put("Fr OS assertion failed: ");
    put(assertion ? assertion : "(null)");
    put(" at ");
    put(file ? file : "(null)");
    put(":");
    put_num(line);
    if (function) {
        put(" in ");
        put(function);
    }
    put("\n");
    abort();
}
