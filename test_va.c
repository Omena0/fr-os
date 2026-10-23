#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <inttypes.h>

static void format_to_sink(const char *fmt, va_list ap)
{
    va_list ap_copy;
    va_copy(ap_copy, ap);
    const char *s = va_arg(ap_copy, const char *);
    va_end(ap_copy);
    printf("string arg via copy: %p %s\n", (void*)s, s ? s : "(null)");
    s = va_arg(ap, const char *);
    printf("string arg via original: %p %s\n", (void*)s, s ? s : "(null)");
    va_end(ap);
}

static void test_vsnprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    format_to_sink(fmt, ap);
    va_end(ap);
}

int main(void)
{
    test_vsnprintf("=== Suite %s: %d passed, %d failed, %d skipped ===",
                   "suite_a", 1, 2, 3);
    return 0;
}
