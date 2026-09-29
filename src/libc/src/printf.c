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
#define LEN_INT		0
#define LEN_LONG	1
#define LEN_LLONG	2
#define LEN_SHORT	(-1)
#define LEN_CHAR	(-2)

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
 */
static size_t utoa(char *buf, size_t size, unsigned long long value,
		   unsigned base, int upper)
{
	static const char lower_digits[] = "0123456789abcdef";
	static const char upper_digits[] = "0123456789ABCDEF";
	const char *digits = upper ? upper_digits : lower_digits;
	char tmp[24];
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
	len = strlen(s);
	if (has_precision && precision >= 0 && (size_t)precision < len)
		len = (size_t)precision;
	emit_padded(st, s, len, 0, 0, width, left_align, 0, NULL);
}

static void handle_unsigned(struct fmt_state *st, unsigned long long value,
			    unsigned base, int upper, int width, int left_align,
			    int zero_pad, int precision, int has_precision)
{
	char buf[24];
	size_t len = utoa(buf, sizeof(buf), value, base, upper);

	/* "%.0d" of zero is an empty field, not "0": the precision is a floor
	 * on the digit count, and the value happens to need none. */
	if (has_precision && precision == 0 && value == 0)
		len = 0;
	emit_padded(st, buf, len, (size_t)precision, has_precision, width,
		    left_align, zero_pad, NULL);
}

static void handle_signed(struct fmt_state *st, long long value, unsigned base,
			  int width, int left_align, int zero_pad,
			  int precision, int has_precision)
{
	char buf[24];
	int negative = 0;
	size_t len = itoa(buf, sizeof(buf), value, base, 0, &negative);

	if (has_precision && precision == 0 && value == 0)
		len = 0;
	emit_padded(st, buf, len, (size_t)precision, has_precision, width,
		    left_align, zero_pad, negative ? "-" : NULL);
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
		int width = 0, precision = 0;
		int longness = 0;	/* 0 = int, 1 = long, 2 = long long, -1 = short */

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
			} else if (*p == '+' || *p == ' ') {
				p++;   /* accepted and ignored: no sign decoration */
			} else if (*p == '#') {
				p++;   /* accepted and ignored */
			} else {
				break;
			}
		}

		/* Width. A negative `*` means left-justify. */
		if (*p == '*') {
			width = va_arg(st.ap, int);
			p++;
			if (width < 0) {
				left_align = 1;
				width = -width;
			}
		} else {
			while (*p >= '0' && *p <= '9')
				width = width * 10 + (*p++ - '0');
		}

		/* Precision. Minimum digits for an integer, maximum characters
		 * for a string. */
		if (*p == '.') {
			p++;
			has_precision = 1;
			if (*p == '*') {
				precision = va_arg(st.ap, int);
				p++;
			} else {
				while (*p >= '0' && *p <= '9')
					precision = precision * 10 + (*p++ - '0');
			}
		}

		/* Length modifier. 'z' is a signed size_t, which on this target
		 * is the same width as 'l'. */
		for (;;) {
			if (*p == 'l') {
				longness++;
				p++;
			} else if (*p == 'h') {
				longness--;
				p++;
			} else if (*p == 'z') {
				longness = 1;
				p++;
			} else {
				break;
			}
		}

		switch (*p) {
		case 'd':
		case 'i': {
			long long v;

			if (longness >= 2)
				v = va_arg(st.ap, long long);
			else if (longness == 1)
				v = va_arg(st.ap, long);
			else if (longness == -1)
				v = (short)va_arg(st.ap, int);
			else
				v = va_arg(st.ap, int);
			handle_signed(&st, v, 10, width, left_align, zero_pad,
				      precision, has_precision);
			break;
		}
		case 'u':
		case 'x':
		case 'X':
		case 'o':
		case 'b': {
			unsigned long long v;
			unsigned base;

			switch (*p) {
			case 'u': base = 10; break;
			case 'o': base = 8; break;
			case 'b': base = 2; break;
			default:   base = 16; break;
			}

			if (longness >= 2)
				v = va_arg(st.ap, unsigned long long);
			else if (longness == 1)
				v = va_arg(st.ap, unsigned long);
			else if (longness == -1)
				v = (unsigned short)va_arg(st.ap, unsigned int);
			else
				v = va_arg(st.ap, unsigned int);
			handle_unsigned(&st, v, base, *p == 'X', width, left_align,
					zero_pad, precision, has_precision);
			break;
		}
		case 'c': {
			char ch = (char)va_arg(st.ap, int);

			emit_padded(&st, &ch, 1, 0, 0, width, left_align, 0, NULL);
			break;
		}
		case 's':
			handle_string(&st, width, left_align, precision,
				      has_precision);
			break;
		case 'p': {
			unsigned long long v =
				(unsigned long long)(uintptr_t)va_arg(st.ap, void *);
			char buf[24];
			char prefix[2] = { '0', 'x' };
			size_t len = utoa(buf, sizeof(buf), v, 16, 0);
			size_t pad = len < 16 ? 16 - len : 0;

			/* Pointers are always 0x-prefixed and at least 16 hex
			 * digits, because an abbreviated address is a
			 * debugging trap. */
			sink_put(&st, prefix, sizeof(prefix));
			sink_repeat(&st, '0', pad);
			sink_put(&st, buf, len);
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
			 * Unsupported or unknown conversion: the specifier is
			 * echoed verbatim so a typo is visible in the output
			 * instead of silently vanishing, and so %f/%e/%g/%a
			 * and %L... read as unsupported rather than as a
			 * format string that happens to consume nothing.
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