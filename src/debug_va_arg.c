/*
 * debug_va_arg.c — Standalone test for the va_arg bug in __libc_vformat
 *
 * This program directly tests the formatting engine (same as libc's
 * __libc_vformat) by including the relevant code with debug logging added.
 * It runs on the host and shows how va_list state changes through each call.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>

/* ---- Debug logging helpers ---- */

static void dlog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[DEBUG] ");
    vfprintf(stderr, fmt, ap);
    putchar('\n');
    va_end(ap);
}

static void show_va_list(const char *label, const va_list ap) {
    /* Print the va_list contents for debugging. On x86-64 System V ABI,
     * va_list is an array of one __va_list_tag struct containing gp_offset,
     * fp_offset, overflow_arg_area, and reg_save_area. We dump it. */
    const unsigned long *p = (const unsigned long *)&ap;
    fprintf(stderr, "[DEBUG] %s - va_list @%p words=%016lx %016lx\n",
            label, (const void*)&ap, p[0], p[1]);
}

/* ---- Copy of the libc's format state ---- */

#define LEN_INT        0
#define LEN_LONG    1
#define LEN_LLONG    2
#define LEN_SHORT    (-1)
#define LEN_CHAR    (-2)

struct fmt_state {
    void (*sink)(void *ctx, const char *data, size_t n);
    void *ctx;
    va_list ap;
    size_t total;
};

static void sink_put(struct fmt_state *st, const char *data, size_t n) {
    if (n) {
        st->sink(st->ctx, data, n);
        st->total += n;
    }
}

static void sink_repeat(struct fmt_state *st, char c, size_t n) {
    char padbuf[64];
    size_t chunk;
    if (n == 0) return;
    memset(padbuf, c, sizeof(padbuf));
    while (n) {
        chunk = n < sizeof(padbuf) ? n : sizeof(padbuf);
        sink_put(st, padbuf, chunk);
        n -= chunk;
    }
}

#define DIGITS_MAX 64

static size_t utoa(char *buf, size_t size, unsigned long long value,
           unsigned base, int upper) {
    static const char lower_digits[] = "0123456789abcdef";
    static const char upper_digits[] = "0123456789ABCDEF";
    const char *digits = upper ? upper_digits : lower_digits;
    char tmp[DIGITS_MAX];
    size_t n = 0;
    size_t i;
    do {
        tmp[n++] = digits[value % base];
        value /= base;
    } while (value != 0 && n < sizeof(tmp));
    for (i = 0; i < n && i < size; i++)
        buf[i] = tmp[n - 1 - i];
    return i;
}

static size_t itoa(char *buf, size_t size, long long value, unsigned base,
           int upper, int *negative) {
    unsigned long long mag;
    if (value < 0) {
        *negative = 1;
        mag = (unsigned long long)(-(value + 1)) + 1;
    } else {
        *negative = 0;
        mag = (unsigned long long)value;
    }
    return utoa(buf, size, mag, base, upper);
}

/* Forward declarations */
static void handle_signed(struct fmt_state *st, long long value, unsigned base,
              int width, int left_align, int zero_pad,
              int precision, int has_precision,
              int plus, int space, const char *alternate);
static void handle_unsigned(struct fmt_state *st, unsigned long long value,
                unsigned base, int upper, int width, int left_align,
                int zero_pad, int precision, int has_precision,
                const char *alternate);
static void handle_string(struct fmt_state *st, int width, int left_align,
              int precision, int has_precision);
static void emit_padded(struct fmt_state *st, const char *body, size_t len,
             size_t precision, int has_precision, int width,
             int left_align, int zero_pad, const char *sign);

static inline long long get_signed_arg(struct fmt_state *st, int longness) {
    if (longness >= LEN_LLONG) return va_arg(st->ap, long long);
    else if (longness == LEN_LONG) return va_arg(st->ap, long);
    else if (longness == LEN_SHORT) return (short)va_arg(st->ap, int);
    else if (longness <= LEN_CHAR) return (signed char)va_arg(st->ap, int);
    else return va_arg(st->ap, int);
}

static inline unsigned long long get_unsigned_arg(struct fmt_state *st, int longness) {
    if (longness >= LEN_LLONG) return va_arg(st->ap, unsigned long long);
    else if (longness == LEN_LONG) return va_arg(st->ap, unsigned long);
    else if (longness == LEN_SHORT) return (unsigned short)va_arg(st->ap, unsigned int);
    else if (longness <= LEN_CHAR) return (unsigned char)va_arg(st->ap, unsigned int);
    else return va_arg(st->ap, unsigned int);
}

/* ---- Implementation of format handlers with debug logging ---- */

static void emit_padded(struct fmt_state *st, const char *body, size_t len,
             size_t precision, int has_precision, int width,
             int left_align, int zero_pad, const char *sign) {
    dlog("[emit_padded] body=%p len=%zu prec=%zu has_prec=%d width=%d left=%d zero=%d sign=%s",
         (void*)body, len, precision, has_precision, width, left_align, zero_pad,
         sign ? sign : "NULL");

    size_t sign_len = sign ? strlen(sign) : 0;
    size_t total = len + sign_len;
    size_t pad;
    size_t zero_extra;

    if (has_precision && precision > len)
        zero_extra = precision - len;
    else
        zero_extra = 0;
    total += zero_extra;

    if (width > 0 && (size_t)width > total)
        pad = (size_t)width - total;
    else
        pad = 0;

    if (!left_align && zero_pad) {
        if (sign)
            sink_put(st, sign, sign_len);
        sink_repeat(st, '0', pad);
    } else {
        if (!left_align)
            sink_repeat(st, ' ', pad);
        if (sign)
            sink_put(st, sign, sign_len);
    }

    if (zero_extra)
        sink_repeat(st, '0', zero_extra);
    sink_put(st, body, len);

    if (left_align)
        sink_repeat(st, ' ', pad);
}

static void handle_string(struct fmt_state *st, int width, int left_align,
              int precision, int has_precision) {
    const char *s = va_arg(st->ap, const char *);
    size_t len;

    dlog("[handle_string] va_arg returned pointer %p (width=%d left=%d prec=%d has_prec=%d)",
         (void*)s, width, left_align, precision, has_precision);

    if (!s)
        s = "(null)";
    if (has_precision)
        len = strnlen(s, (size_t)precision);
    else
        len = strlen(s);
    dlog("[handle_string] s=%p len=%zu '%s'", (void*)s, len, s ? s : "(null)");
    emit_padded(st, s, len, 0, 0, width, left_align, 0, NULL);
}

static void handle_unsigned(struct fmt_state *st, unsigned long long value,
                unsigned base, int upper, int width, int left_align,
                int zero_pad, int precision, int has_precision,
                const char *alternate) {
    char buf[DIGITS_MAX];
    size_t len = utoa(buf, sizeof(buf), value, base, upper);

    dlog("[handle_unsigned] value=%llu base=%u upper=%d width=%d left=%d zero=%d prec=%d has_prec=%d alt=%s",
         value, base, upper, width, left_align, zero_pad, precision, has_precision,
         alternate ? alternate : "NULL");

    if (has_precision && precision == 0 && value == 0)
        len = 0;

    if (alternate) {
        if (base == 8) {
            size_t need = 0;
            if (len == 0) need = 1;
            else if (buf[0] != '0') need = len + 1;
            if (need && (!has_precision || (size_t)precision < need)) {
                precision = (int)need;
                has_precision = 1;
            }
            alternate = NULL;
        } else if (value == 0 || len == 0) {
            alternate = NULL;
        }
    }
    emit_padded(st, buf, len, (size_t)precision, has_precision, width,
            left_align, zero_pad, alternate);
}

static void handle_signed(struct fmt_state *st, long long value, unsigned base,
              int width, int left_align, int zero_pad,
              int precision, int has_precision,
              int plus, int space, const char *alternate) {
    char buf[DIGITS_MAX];
    int negative = 0;
    size_t len = itoa(buf, sizeof(buf), value, base, 0, &negative);

    dlog("[handle_signed] value=%lld base=%u width=%d left=%d zero=%d prec=%d has_prec=%d plus=%d space=%d alt=%s",
         value, base, width, left_align, zero_pad, precision, has_precision,
         plus, space, alternate ? alternate : "NULL");

    const char *sign = negative ? "-" : (alternate ? alternate : (plus ? "+" : (space ? " " : NULL)));

    if (has_precision && precision == 0 && value == 0)
        len = 0;
    emit_padded(st, buf, len, (size_t)precision, has_precision, width,
            left_align, zero_pad, sign);
}

/* ---- The full format engine with debug logging ---- */

size_t __libc_vformat(void (*sink)(void *ctx, const char *data, size_t n),
              void *ctx, const char *fmt, va_list ap) {
    struct fmt_state st = { .sink = sink, .ctx = ctx, .total = 0 };
    const char *p = fmt;

    dlog("[vformat] entered, fmt='%s'", fmt);
    show_va_list("input va_list", ap);

    va_copy(st.ap, ap);

    while (*p) {
        int left_align = 0, zero_pad = 0;
        int has_precision = 0;
        int plus = 0, space = 0, alternate = 0;
        int width = 0, precision = 0;
        int longness = 0;

        if (*p != '%') {
            char ch = *p++;
            sink_put(&st, &ch, 1);
            continue;
        }
        p++;

        for (;;) {
            if (*p == '-') { left_align = 1; p++; }
            else if (*p == '0') { zero_pad = 1; p++; }
            else if (*p == '+') { plus = 1; p++; }
            else if (*p == ' ') { space = 1; p++; }
            else if (*p == '#') { alternate = 1; p++; }
            else break;
        }

        if (*p == '*') {
            int w = va_arg(st.ap, int);
            p++;
            if (w < 0) {
                left_align = 1;
                if (w == INT_MIN) width = INT_MAX;
                else width = -w;
            } else width = w;
        } else {
            while (*p >= '0' && *p <= '9') {
                int d = *p++ - '0';
                if (width > (INT_MAX - d) / 10) width = INT_MAX;
                else width = width * 10 + d;
            }
        }

        if (*p == '.') {
            p++;
            has_precision = 1;
            if (*p == '*') {
                int pr = va_arg(st.ap, int);
                p++;
                if (pr < 0) has_precision = 0;
                else if (pr > INT_MAX) precision = INT_MAX;
                else precision = pr;
            } else {
                while (*p >= '0' && *p <= '9') {
                    int d = *p++ - '0';
                    if (precision > (INT_MAX - d) / 10) precision = INT_MAX;
                    else precision = precision * 10 + d;
                }
            }
            zero_pad = 0;
        }

        for (;;) {
            if (*p == 'l') { longness++; p++; }
            else if (*p == 'h') { longness--; p++; }
            else if (*p == 'z') { longness = LEN_LONG; p++; }
            else if (*p == 'j') { longness = LEN_LLONG; p++; }
            else if (*p == 't') { longness = LEN_LONG; p++; }
            else break;
        }

        switch (*p) {
        case 'd': case 'i': {
            long long v = get_signed_arg(&st, longness);
            dlog("[vformat] 'd/i' conversion, got v=%lld", v);
            handle_signed(&st, v, 10, width, left_align, zero_pad,
                      precision, has_precision, plus, space, NULL);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': case 'b': {
            unsigned long long v = get_unsigned_arg(&st, longness);
            unsigned base;
            const char *alt_prefix = NULL;
            switch (*p) {
            case 'u': base = 10; break;
            case 'o': base = 8; break;
            case 'b': base = 2; break;
            default:   base = 16; break;
            }
            if (alternate) {
                if (*p == 'o') alt_prefix = "0";
                else if (*p == 'x') alt_prefix = "0x";
                else if (*p == 'X') alt_prefix = "0X";
                else if (*p == 'b') alt_prefix = "0b";
            }
            handle_unsigned(&st, v, base, *p == 'X', width, left_align,
                    zero_pad, precision, has_precision, alt_prefix);
            break;
        }
        case 'c': {
            char ch = (char)va_arg(st.ap, int);
            dlog("[vformat] 'c' conversion, got ch=%c", ch);
            emit_padded(&st, &ch, 1, 0, 0, width, left_align, 0, NULL);
            break;
        }
        case 's': {
            dlog("[vformat] 's' conversion, calling handle_string");
            handle_string(&st, width, left_align, precision, has_precision);
            break;
        }
        case '%':
            sink_put(&st, p, 1);
            break;
        default:
            sink_put(&st, "%", 1);
            sink_put(&st, p, 1);
            break;
        }
        if (*p) p++;
    }

    dlog("[vformat] exiting, total=%zu", st.total);
    va_end(st.ap);
    return st.total;
}

/* ---- Buffer sink for capturing output ---- */
struct buffer_sink { char *buf; size_t size; size_t written; };

static void buffer_put(void *ctx, const char *data, size_t n) {
    struct buffer_sink *bs = ctx;
    size_t room;
    if (!bs->buf || bs->size == 0) return;
    room = bs->size - 1;
    if (bs->written >= room) return;
    if (n > room - bs->written) n = room - bs->written;
    memcpy(bs->buf + bs->written, data, n);
    bs->written += n;
}

/* ---- Test function using our debug format engine ---- */
void test_log_debug(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    
    dlog("[test_log_debug] called with fmt='%s'", fmt);
    show_va_list("test_log input va_list", ap);
    
    struct buffer_sink bs = { .buf = NULL, .size = 512, .written = 0 };
    bs.buf = malloc(512);
    bs.size = 512;
    
    va_list ap_copy;
    va_copy(ap_copy, ap);
    size_t total = __libc_vformat(buffer_put, &bs, fmt, ap_copy);
    va_end(ap_copy);
    
    bs.buf[bs.written] = '\0';
    printf("%s\n", bs.buf);
    
    dlog("[test_log_debug] total=%zu output='%s'", total, bs.buf);
    free(bs.buf);
    va_end(ap);
}

/* ---- Test cases ---- */

void test_suite_summary() {
    dlog("=== Test: Suite Summary ===");
    test_log_debug("=== Suite %s: %d passed, %d failed, %d skipped ===",
                   "suite_a", 1, 2, 3);
    dlog("=== End Suite Summary ===\n");
}

void test_simple() {
    dlog("=== Test: Simple String ===");
    test_log_debug("Hello %s!", "world");
}

void test_multiple() {
    dlog("=== Test: Multiple Strings ===");
    test_log_debug("%s %s %s", "one", "two", "three");
}

void test_precision() {
    dlog("=== Test: Precision ===");
    test_log_debug("%.5s", "hello world");
}

void test_width() {
    dlog("=== Test: Width ===");
    test_log_debug("%10s", "hello");
}

void test_edge() {
    dlog("=== Test: Edge Cases ===");
    test_log_debug("%s", NULL);  /* This exercises NULL string path */
    test_log_debug("%s", "");
}

void test_mixed() {
    dlog("=== Test: Mixed Formats ===");
    test_log_debug("Count: %d, Name: %s, Hex: %x", 42, "test", 255);
}

/* ---- Main ---- */
int main(void) {
    printf("=== VA_ARG DEBUG TEST START ===\n");
    
    test_suite_summary();
    test_simple();
    test_multiple();
    test_precision();
    test_width();
    test_edge();
    test_mixed();
    
    printf("\n=== VA_ARG DEBUG TEST END ===\n");
    return 0;
}