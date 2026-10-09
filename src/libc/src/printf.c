/*
 * printf.c — freestanding printf family, sink-based.
 *
 * The engine in format_to_sink() mirrors the kernel's kprintf algorithm
 * (src/kernel/kprintf.c): parse each specifier into flags, width, precision
 * and length, convert the integer into a local buffer, then emit through a
 * pluggable sink that receives chunks of bytes. format_to_sink() itself knows
 * nothing about buffers — the snprintf family and the FILE layer each supply
 * their own sink.
 *
 * No floating point. %f/%e/%g/%a and the L length modifier are unsupported on
 * this freestanding target (there is no FPU state save/restore, and enabling
 * the FPU for one conversion is not worth it). When one is encountered the
 * specifier is emitted as literal characters (% followed by the conversion
 * character) so a typo is visible in the output instead of vanishing.
 */
#include <limits.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>

/* Length modifier encoding. */
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

static void sink_put(struct fmt_state *st, const char *data, size_t n)
{
    if (n) {
        st->sink(st->ctx, data, n);
        st->total += n;
    }
}

static void sink_repeat(struct fmt_state *st, char c, size_t n)
{
    char padbuf[64];
    size_t chunk;

    if (n == 0)
        return;
    memset(padbuf, c, sizeof(padbuf));
    while (n) {
        chunk = n < sizeof(padbuf) ? n : sizeof(padbuf);
        sink_put(st, padbuf, chunk);
        n -= chunk;
    }
}

/*
 * Build a formatted representation of `value` in `base` into `buf`, returning
 * its length. Digits are produced least-significant first and reversed here.
 *
 * tmp has to hold the widest conversion this target can produce: a 64-bit
 * value in base 2 is 64 digits. At 24 entries the loop's `n < sizeof(tmp)`
 * guard stopped after 23 digits and the number was silently truncated --
 * printf("%b", ~0UL) printed 23 ones and no error. Every caller below sizes
 * its own buffer from this same bound.
 */
#define DIGITS_MAX 64

static size_t utoa(char *buf, size_t size, unsigned long long value,
           unsigned base, int upper)
{
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

/* Signed conversion; the sign is emitted separately by the caller. */
static size_t itoa(char *buf, size_t size, long long value, unsigned base,
           int upper, int *negative)
{
    unsigned long long mag;

    if (value < 0) {
        *negative = 1;
        /* Negate in unsigned space so LLONG_MIN does not overflow. */
        mag = (unsigned long long)(-(value + 1)) + 1;
    } else {
        *negative = 0;
        mag = (unsigned long long)value;
    }
    return utoa(buf, size, mag, base, upper);
}

/*
 * Emit `body` with the requested width, honouring precision (minimum digits
 * for numbers, zero-filled; maximum characters for strings) and the
 * left-justify flag. The sign participates in zero padding when it appears
 * before the pad: "-00042", not "000-42".
 */
static void emit_padded(struct fmt_state *st, const char *body, size_t len,
            size_t precision, int has_precision, int width,
            int left_align, int zero_pad, const char *sign)
{
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
              int precision, int has_precision)
{
    const char *s = va_arg(st->ap, const char *);
    size_t len;

    if (!s)
        s = "(null)";
    /*
     * strnlen, not strlen followed by a clamp. `%.4s` is specified to
     * read at most four bytes and stop: a bounded field is how a program
     * prints a fixed-width column out of a buffer that is known to be
     * shorter than the buffer's capacity. Calling strlen first walks to
     * the NUL regardless, which faults on exactly the input the precision
     * was written to protect against.
     */
    if (has_precision)
        len = strnlen(s, (size_t)precision);
    else
        len = strlen(s);
    emit_padded(st, s, len, 0, 0, width, left_align, 0, NULL);
}

/*
 * plus/space decide the sign of a non-negative result, and alternate is the
 * "#" flag: "0x" for x/X, "0" for o. Both are prefixes that go in before the
 * pad, which is exactly the slot `sign` occupies, so they travel the same way.
 * C99 7.21.6.1 p6: "#" has no effect on d, i, u, so no prefix is passed for
 * those.
 *
 * The sign survives an empty field. A precision of zero suppresses the *digits*
 * of a zero value ("%.0d" of 0 is the empty field), not the sign: "The result
 * of a signed conversion always begins with a plus or minus sign" is a
 * statement about the conversion, and it does not carve out an exception for
 * "%.0d". So "%+.0d" of 0 is "+" and "% .0d" of 0 is " ", which is what the
 * digits-suppressed rule leaves untouched.
 */
static void handle_signed(struct fmt_state *st, long long value, unsigned base,
              int width, int left_align, int zero_pad,
              int precision, int has_precision,
              int plus, int space, const char *alternate)
{
    char buf[DIGITS_MAX];
    const char *sign;
    int negative = 0;
    size_t len = itoa(buf, sizeof(buf), value, base, 0, &negative);

    if (negative)
        sign = "-";
    else if (alternate)
        sign = alternate;
    else if (plus)
        sign = "+";
    else if (space)
        sign = " ";
    else
        sign = NULL;

    if (has_precision && precision == 0 && value == 0)
        len = 0;
    emit_padded(st, buf, len, (size_t)precision, has_precision, width,
            left_align, zero_pad, sign);
}

/*
 * C99 7.21.6.1 p6, for "#":
 *
 *   x, X  a *nonzero* result has 0x (0X) prefixed to it.  The qualifier is
 *         part of the condition, so %#x of 0 is "0" and not "0x0", and
 *         %#.3x of 0 is "000".  A field that is empty for want of digits has
 *         no first digit to prefix either, so %#.0x of 0 is the empty field.
 *
 *   o     it increases the precision, if and only if necessary, to force the
 *         first digit of the result to be a zero.  There is no separate prefix
 *         to place: the extra digit is a leading zero like any other, so it is
 *         expressed by raising the precision floor just enough and letting
 *         emit_padded() zero-fill.  "Necessary" is the whole rule -- the floor
 *         only moves when the field would otherwise not start with a zero. So
 *         "%#o" of 1 is "01" (the "1" needs a zero in front) while "%#3o" of 1
 *         is "001" (the precision already supplies one), and "%#o" of 0 is "0"
 *         rather than "00" (the single digit already is that zero).
 */
static void handle_unsigned(struct fmt_state *st, unsigned long long value,
                unsigned base, int upper, int width, int left_align,
                int zero_pad, int precision, int has_precision,
                const char *alternate)
{
    char buf[DIGITS_MAX];
    size_t len = utoa(buf, sizeof(buf), value, base, upper);

    /* "%.0d" of zero is an empty field, not "0": the precision is a floor
     * on the digit count, and the value happens to need none. */
    if (has_precision && precision == 0 && value == 0)
        len = 0;

    if (alternate) {
        if (base == 8) {
            /*
             * The extra digit is needed only if the field does not
             * already begin with a zero. A value of 0 prints the
             * single digit "0", which is already the leading zero,
             * so "%#o" of 0 is "0" and not "00"; the same is true
             * of an empty field, which is the zero value at
             * precision 0 and so needs one digit back: "%#.0o" of
             * 0 is "0". Every other value starts with a nonzero
             * digit and needs exactly one more digit in front of
             * it, which the precision floor supplies.
             */
            size_t need = 0;

            if (len == 0)
                need = 1;
            else if (buf[0] != '0')
                need = len + 1;
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

static inline long long get_signed_arg(struct fmt_state *st, int longness)
{
    if (longness >= LEN_LLONG)
        return va_arg(st->ap, long long);
    else if (longness == LEN_LONG)
        return va_arg(st->ap, long);
    else if (longness == LEN_SHORT)
        return (short)va_arg(st->ap, int);
    else if (longness <= LEN_CHAR)
        return (signed char)va_arg(st->ap, int);
    else
        return va_arg(st->ap, int);
}

static inline unsigned long long get_unsigned_arg(struct fmt_state *st, int longness)
{
    if (longness >= LEN_LLONG)
        return va_arg(st->ap, unsigned long long);
    else if (longness == LEN_LONG)
        return va_arg(st->ap, unsigned long);
    else if (longness == LEN_SHORT)
        return (unsigned short)va_arg(st->ap, unsigned int);
    else if (longness <= LEN_CHAR)
        return (unsigned char)va_arg(st->ap, unsigned int);
    else
        return va_arg(st->ap, unsigned int);
}

/*
 * The single formatting entry point. Every printf in the library is this
 * function plus a sink: the string family counts what it dropped, the FILE
 * family hands chunks to the stream's buffer.
 */
size_t __libc_vformat(void (*sink)(void *ctx, const char *data, size_t n),
              void *ctx, const char *fmt, va_list ap)
{
    struct fmt_state st = { .sink = sink, .ctx = ctx, .total = 0 };
    const char *p = fmt;

    /*
     * The caller's va_list is consumed by an unknown number of arguments,
     * so the walk happens on a private copy and the caller's is left
     * untouched. va_copy rather than a plain assignment: va_list is an
     * array type on this ABI, so `st.ap = ap` would try to copy an array.
     */
    va_copy(st.ap, ap);

    while (*p) {
        int left_align = 0, zero_pad = 0;
        int has_precision = 0;
        int plus = 0, space = 0, alternate = 0;
        int width = 0, precision = 0;
        int longness = 0;    /* 0 = int, 1 = long, 2 = long long, -1 = short */

        if (*p != '%') {
            char ch = *p++;

            sink_put(&st, &ch, 1);
            continue;
        }
        p++;

        /* Flags. A '-' must be checked before '0' because '-' also
         * terminates the flag loop. */
        for (;;) {
            if (*p == '-') {
                left_align = 1;
                p++;
            } else if (*p == '0') {
                zero_pad = 1;
                p++;
            } else if (*p == '+') {
                plus = 1;
                p++;
            } else if (*p == ' ') {
                space = 1;
                p++;
            } else if (*p == '#') {
                /* Alternate form: "0x" for x/X, "0" for o, and
                 * nothing at all for d/i/u, which have no
                 * alternate form. Applied at the conversion
                 * so the prefix matches the base actually
                 * printed. */
                alternate = 1;
                p++;
            } else {
                break;
            }
        }

        /*
         * Width and precision are parsed with saturation rather than
         * by letting the accumulator wrap. `width * 10 + digit` on an
         * int that has run past ten digits is signed overflow, which is
         * undefined behaviour -- and a negative width becomes a negative
         * precision downstream, where `(size_t)precision` turns it into
         * SIZE_MAX and sink_repeat() then tries to emit SIZE_MAX bytes
         * of padding. Saturating at INT_MAX keeps the value a sane
         * field width and, for precision, still leaves "no digits were
         * requested" detectable.
         */
        if (*p == '*') {
            int w = va_arg(st.ap, int);

            p++;
            if (w < 0) {
                left_align = 1;
                /* Negating INT_MIN is not representable, so that
                 * one width saturates. Every other negative
                 * width keeps its magnitude, which is what
                 * %-*d with a small width has to do. */
                if (w == INT_MIN)
                    width = INT_MAX;
                else
                    width = -w;
            } else {
                width = w;
            }
        } else {
            while (*p >= '0' && *p <= '9') {
                int d = *p++ - '0';

                if (width > (INT_MAX - d) / 10)
                    width = INT_MAX;
                else
                    width = width * 10 + d;
            }
        }

        /* Precision. Minimum digits for an integer, maximum characters
         * for a string. */
        if (*p == '.') {
            p++;
            has_precision = 1;
            if (*p == '*') {
                int pr = va_arg(st.ap, int);

                p++;
                /* A negative precision means "as many as it
                 * takes" (C99 7.19.6.1), which is the same as
                 * no precision at all. */
                if (pr < 0)
                    has_precision = 0;
                else if (pr > INT_MAX)
                    precision = INT_MAX;
                else
                    precision = pr;
            } else {
                while (*p >= '0' && *p <= '9') {
                    int d = *p++ - '0';

                    if (precision > (INT_MAX - d) / 10)
                        precision = INT_MAX;
                    else
                        precision = precision * 10 + d;
                }
            }
            /*
             * C99 7.21.6.1 p6: for d, i, o, u, x, X the '0' flag is
             * *ignored* when a precision is specified, so the field is
             * space-padded and only the precision floor is zero-filled:
             * %08.5d of 42 is "   00042", not "00000042". Zero-filling
             * the width pad as well is what produced the second.
             */
            zero_pad = 0;
        }

        /* Length modifier. On this LP64 target 'z' (size_t), 'j'
         * (intmax_t) and 't' (ptrdiff_t) all have the same width as
         * 'l', so they map onto it; 'z' is signed here only in the
         * sense that the *unsigned* conversions are the ones that
         * reach for it, and the switch below picks the unsigned or
         * signed va_arg to match the conversion character. */
        for (;;) {
            if (*p == 'l') {
                longness++;
                p++;
            } else if (*p == 'h') {
                longness--;
                p++;
            } else if (*p == 'z') {
                longness = LEN_LONG;
                p++;
            } else if (*p == 'j') {
                longness = LEN_LLONG;
                p++;
            } else if (*p == 't') {
                longness = LEN_LONG;
                p++;
            } else {
                break;
            }
        }

        switch (*p) {
        case 'd':
        case 'i': {
            long long v = get_signed_arg(&st, longness);

            /* d/i have no alternate form, so '#' adds nothing. */
            handle_signed(&st, v, 10, width, left_align, zero_pad,
                      precision, has_precision, plus, space,
                      NULL);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o':
        case 'b': {
            unsigned long long v = get_unsigned_arg(&st, longness);
            unsigned base;
            /*
             * "#" prefixes a '0' for o and "0x"/"0X" for x/X, and
             * nothing for u. The '0' flag was already cleared
             * below if a precision was given, so the two never
             * compete for the same leading digit.
             */
            const char *alt_prefix = NULL;

            switch (*p) {
            case 'u': base = 10; break;
            case 'o': base = 8; break;
            case 'b': base = 2; break;
            default:   base = 16; break;
            }

            if (alternate) {
                if (*p == 'o')
                    alt_prefix = "0";
                else if (*p == 'x')
                    alt_prefix = "0x";
                else if (*p == 'X')
                    alt_prefix = "0X";
                else if (*p == 'b')
                    /* %b is this libc's own extension;
                     * it shares the arm with x/X so it
                     * follows the same rule. */
                    alt_prefix = "0b";
            }

            handle_unsigned(&st, v, base, *p == 'X', width, left_align,
                    zero_pad, precision, has_precision,
                    alt_prefix);
            break;
        }
        case 'c': {
            char ch = (char)va_arg(st.ap, int);

            /* The '0' flag is defined only for the numeric
             * conversions, so %05c pads with spaces like %5c. */
            zero_pad = 0;

            emit_padded(&st, &ch, 1, 0, 0, width, left_align, 0, NULL);
            break;
        }
        case 's':
            zero_pad = 0;
            handle_string(&st, width, left_align, precision,
                      has_precision);
            break;
        case 'p': {
            unsigned long long v =
                (unsigned long long)(uintptr_t)va_arg(st.ap, void *);
            char buf[DIGITS_MAX];
            size_t len = utoa(buf, sizeof(buf), v, 16, 0);
            size_t digits = 16;
            size_t fixed;
            size_t pad;

            /*
             * Pointers are always 0x-prefixed and zero-padded to
             * at least 16 hex digits, because an abbreviated address
             * is a debugging trap: `0x1234` from a pointer print is
             * indistinguishable from the integer 0x1234.
             *
             * This deliberately differs from glibc, which strips
             * leading zeros and prints NULL as "(nil)". Two
             * reasons to keep the difference: the width is
             * constant, so pointer columns line up, and a NULL
             * pointer prints as an address rather than a word
             * that means something else entirely. A program that
             * parses %p output as a fixed-length string is the
             * thing this breaks, and it should not exist.
             *
             * Width and '-' apply on top of that, as they do for
             * every other conversion: a field narrower than the
             * 18 characters the body needs is never truncated, and
             * a wider one is padded with spaces (or, for '0', with
             * zeros inside the body where the numeric conversions
             * put them -- after the 0x, not after the digits).
             *
             * Positional parameters (%1$d) are NOT implemented;
             * they are passed through as literal text. Nothing in
             * the tree uses them and supporting them means
             * reordering the va_list, which is a different piece
             * of work from anything else in this file.
             */
            if (zero_pad && width > 2 + 16)
                digits = (size_t)width - 2;
            fixed = 2 + digits;
            pad = (size_t)width > fixed ? (size_t)width - fixed : 0;

            if (!left_align)
                sink_repeat(&st, ' ', pad);
            sink_put(&st, "0x", 2);
            /* len <= 16 for every 64-bit value and digits >= 16. */
            sink_repeat(&st, '0', digits - len);
            sink_put(&st, buf, len);
            if (left_align)
                sink_repeat(&st, ' ', pad);
            break;
        }
        case '%':
            sink_put(&st, p, 1);
            break;
        case '\0':
            /* Trailing '%': emit it rather than reading past the end. */
            sink_put(&st, "%", 1);
            continue;
        default:
            /*
             * Unsupported or unknown conversion: "%" followed by the
             * conversion character is emitted so a typo is visible
             * in the output instead of silently vanishing, and so
             * %f/%e/%g/%a and %L... read as unsupported rather
             * than as a format string that happens to consume
             * nothing. Flags, width, precision and the length
             * modifier have already been consumed and are dropped
             * -- so "%20.3f" is reported as "%f", not as
             * "%20.3f". What identifies the unsupported
             * conversion is the character, and that is what is
             * kept. C99 7.21.6.1 p9 leaves an invalid conversion
             * specification undefined, so glibc's choice to fail
             * the whole call with a negative return is the other
             * permitted answer, not the required one.
             */
            sink_put(&st, "%", 1);
            sink_put(&st, p, 1);
            break;
        }
        if (*p)
            p++;
    }

    va_end(st.ap);
    return st.total;
}

/* ------------------------------------------------------------- buffer sink -- */

struct buffer_sink {
    char *buf;
    size_t size;
    size_t written;
};

static void buffer_put(void *ctx, const char *data, size_t n)
{
    struct buffer_sink *bs = ctx;
    size_t room;

    /* One byte is always reserved for the terminator, so vsnprintf output
     * is a valid C string even when it does not fit. */
    if (!bs->buf || bs->size == 0)
        return;
    room = bs->size - 1;
    if (bs->written >= room)
        return;
    if (n > room - bs->written)
        n = room - bs->written;
    memcpy(bs->buf + bs->written, data, n);
    bs->written += n;
}

int vsnprintf(char *str, size_t size, const char *format, va_list ap)
{
    struct buffer_sink bs = { .buf = str, .size = size, .written = 0 };
    size_t total = __libc_vformat(buffer_put, &bs, format, ap);

    if (str && size)
        str[bs.written] = '\0';
    /* The return value is what the format *would* have produced, so the
     * caller can detect truncation without re-running the conversion. */
    return total > (size_t)INT_MAX ? INT_MAX : (int)total;
}

int snprintf(char *str, size_t size, const char *format, ...)
{
    va_list ap;
    int ret;

    va_start(ap, format);
    ret = vsnprintf(str, size, format, ap);
    va_end(ap);
    return ret;
}

int vsprintf(char *str, const char *format, va_list ap)
{
    /*
     * The caller has promised a buffer big enough, so the length is not
     * known here. Scanning the finished string is cheaper than carrying a
     * second counting pass, and vsnprintf already has the terminating-NUL
     * logic in one place.
     */
    vsnprintf(str, (size_t)-1, format, ap);
    return (int)strlen(str);
}

int sprintf(char *str, const char *format, ...)
{
    va_list ap;
    int ret;

    va_start(ap, format);
    ret = vsprintf(str, format, ap);
    va_end(ap);
    return ret;
}