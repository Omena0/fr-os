/*
 * scanf.c — formatted input (sscanf/vsscanf).
 *
 * A practical subset of the C format language: whitespace and literal
 * matching, %d %i %u %o %x %X %c %s %p %n, field widths, '*' assignment
 * suppression, and the h/hh/l/ll/z length modifiers. Scansets (%[...]) and
 * floating point are not implemented; an unsupported conversion stops the
 * scan, which is the conservative behaviour.
 *
 * No locale, no mbstate: a byte is a character, as in the rest of this libc.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define LEN_INT    0
#define LEN_LONG   1
#define LEN_LLONG  2
#define LEN_SIZE   3
#define LEN_SHORT  (-1)
#define LEN_CHAR   (-2)

static void store_signed(int len, va_list ap, long long v)
{
    switch (len) {
    case LEN_LLONG: *va_arg(ap, long long *) = v; break;
    case LEN_LONG:  *va_arg(ap, long *) = (long)v; break;
    case LEN_SIZE:  *va_arg(ap, ssize_t *) = (ssize_t)v; break;
    case LEN_SHORT: *va_arg(ap, short *) = (short)v; break;
    case LEN_CHAR:  *va_arg(ap, signed char *) = (signed char)v; break;
    default:        *va_arg(ap, int *) = (int)v; break;
    }
}

static void store_unsigned(int len, va_list ap, unsigned long long v)
{
    switch (len) {
    case LEN_LLONG: *va_arg(ap, unsigned long long *) = v; break;
    case LEN_LONG:  *va_arg(ap, unsigned long *) = (unsigned long)v; break;
    case LEN_SIZE:  *va_arg(ap, size_t *) = (size_t)v; break;
    case LEN_SHORT: *va_arg(ap, unsigned short *) = (unsigned short)v; break;
    case LEN_CHAR:  *va_arg(ap, unsigned char *) = (unsigned char)v; break;
    default:        *va_arg(ap, unsigned int *) = (unsigned int)v; break;
    }
}

/* Copy at most `width` bytes (or a bounded chunk when width is 0) into tmp so
 * the integer parsers cannot run off the end of the input. Returns the input
 * position after the copied region and the copied length. */
static const char *grab(const char *s, int width, char *tmp, size_t tmpsz,
                        size_t *len)
{
    size_t n = 0;

    while (*s && (width == 0 || n < (size_t)width) && n + 1 < tmpsz)
        tmp[n++] = *s++;
    tmp[n] = '\0';
    *len = n;
    return s;
}

int vsscanf(const char *str, const char *format, va_list ap)
{
    const char *s = str;
    const char *f = format;
    int assigned = 0;
    int input_failure = 0;

    if (!str || !format)
        return -1;

    while (*f) {
        if (isspace((unsigned char)*f)) {
            while (isspace((unsigned char)*f))
                f++;
            while (isspace((unsigned char)*s))
                s++;
            continue;
        }

        if (*f != '%') {
            if (*s != *f)
                break;
            s++;
            f++;
            continue;
        }

        f++;
        if (*f == '%') {
            if (*s != '%')
                break;
            s++;
            f++;
            continue;
        }

        int suppress = 0;
        if (*f == '*') {
            suppress = 1;
            f++;
        }

        int width = 0;
        while (isdigit((unsigned char)*f)) {
            width = width * 10 + (*f - '0');
            f++;
        }

        int len = LEN_INT;
        for (;;) {
            if (*f == 'h') {
                len = (len == LEN_SHORT) ? LEN_CHAR : LEN_SHORT;
                f++;
            } else if (*f == 'l') {
                len = (len == LEN_LONG) ? LEN_LLONG : LEN_LONG;
                f++;
            } else if (*f == 'z') {
                len = LEN_SIZE;
                f++;
            } else if (*f == 'j' || *f == 't') {
                len = LEN_LLONG;
                f++;
            } else {
                break;
            }
        }

        char conv = *f++;
        if (!conv)
            break;

        if (conv != 'c' && conv != 'n' && conv != '[')
            while (isspace((unsigned char)*s))
                s++;

        if (conv == 'n') {
            if (!suppress)
                *va_arg(ap, int *) = (int)(s - str);
            continue;
        }

        if (!*s) {
            input_failure = 1;
            break;
        }

        if (conv == 'c') {
            int n = width ? width : 1;
            char *out = suppress ? NULL : va_arg(ap, char *);

            for (int i = 0; i < n; i++) {
                if (!*s) {
                    input_failure = 1;
                    break;
                }
                if (out)
                    out[i] = *s;
                s++;
            }
            if (input_failure)
                break;
            if (!suppress)
                assigned++;
            continue;
        }

        if (conv == 's') {
            char *out = suppress ? NULL : va_arg(ap, char *);
            int n = 0;

            while (*s && !isspace((unsigned char)*s) &&
                   (width == 0 || n < width)) {
                if (out)
                    out[n] = *s;
                n++;
                s++;
            }
            if (n == 0) {
                input_failure = 1;
                break;
            }
            if (out)
                out[n] = '\0';
            if (!suppress)
                assigned++;
            continue;
        }

        int base;
        int is_unsigned = 0;

        switch (conv) {
        case 'd': base = 10; break;
        case 'i': base = 0; break;
        case 'u': base = 10; is_unsigned = 1; break;
        case 'o': base = 8; is_unsigned = 1; break;
        case 'x':
        case 'X': base = 16; is_unsigned = 1; break;
        case 'p': base = 16; is_unsigned = 1; len = LEN_LLONG; break;
        default:
            /* Unsupported conversion: stop rather than guess. */
            input_failure = 1;
            goto done;
        }

        char tmp[64];
        size_t copied;
        const char *after = grab(s, width, tmp, sizeof(tmp), &copied);
        char *end = NULL;

        if (copied == 0) {
            input_failure = 1;
            break;
        }

        if (is_unsigned) {
            unsigned long long v = strtoull(tmp, &end, base);

            if (end == tmp) {
                input_failure = 1;
                break;
            }
            if (!suppress) {
                if (conv == 'p')
                    *va_arg(ap, void **) = (void *)(uintptr_t)v;
                else
                    store_unsigned(len, ap, v);
                assigned++;
            }
        } else {
            long long v = strtoll(tmp, &end, base);

            if (end == tmp) {
                input_failure = 1;
                break;
            }
            if (!suppress) {
                store_signed(len, ap, v);
                assigned++;
            }
        }

        s += (size_t)(end - tmp);
        (void)after;
    }

done:
    if (assigned == 0 && input_failure && *str == '\0')
        return EOF;
    return assigned;
}

int sscanf(const char *str, const char *format, ...)
{
    va_list ap;
    int ret;

    va_start(ap, format);
    ret = vsscanf(str, format, ap);
    va_end(ap);
    return ret;
}
