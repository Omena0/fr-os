/*
 * kprintf.c — formatted output built on a pluggable character sink.
 *
 * The formatter itself knows nothing about consoles. It emits characters one at
 * a time through a function pointer, and the callers adapt that to whatever
 * needs the text: the console, a fixed buffer, the panic dump, or a trace ring.
 *
 * Conversion is done into a fixed local buffer and emitted with the padding
 * logic in one place, rather than each conversion handling its own width. That
 * keeps the number parser free of presentation concerns and makes it
 * testable.
 */

#include <kprintf.h>
#include <kstring.h>

struct fmt_state {
	kvprintf_sink_t sink;
	void *arg;
	const char *fmt;
	va_list ap;
};

/*
 * A number needs at most 64 digits, and that is only for base 2; the buffer is
 * sized for the worst case rather than for hex so that %b of a 64-bit value is
 * not silently truncated. Every conversion buffer in this file is this size.
 */
#define FMT_NUMBUF 66

/*
 * Build a formatted representation of `value` in `base` into `buf`, returning
 * its length. Digits are produced least-significant first and reversed here, so
 * the conversion loop is a straight division with no lookahead.
 */
static size_t utoa(char *buf, size_t size, unsigned long long value,
		   unsigned base, bool upper)
{
	static const char lower_digits[] = "0123456789abcdef";
	static const char upper_digits[] = "0123456789ABCDEF";
	const char *digits = upper ? upper_digits : lower_digits;
	char tmp[FMT_NUMBUF];
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

/*
 * Signed conversion. The sign is emitted separately so the caller can control
 * whether it participates in zero padding: "-5" is zero-padded as "-0005", not
 * "000-5".
 */
static size_t itoa(char *buf, size_t size, long long value,
		   unsigned base, bool upper, bool *negative)
{
	unsigned long long mag;

	if (value < 0) {
		*negative = true;
		/* Negate in unsigned space so LLONG_MIN does not overflow. */
		mag = (unsigned long long)(-(value + 1)) + 1;
	} else {
		*negative = false;
		mag = (unsigned long long)value;
	}
	return utoa(buf, size, mag, base, upper);
}

/*
 * Emit `body` with the requested width, honouring precision and the '-'
 * left-justify flag.
 *
 * The rules are the ones in C99 7.21.6.1, and each of them has to be kept or
 * the output stops being usable as a fixed-column log:
 *
 *   - A precision on an integer is a floor on the *digit count*, zero filled
 *     whatever the flags say, and it applies even when the field is being
 *     left-justified: "%.5d" of 42 is "00042" and "%-8.5d" of 42 is
 *     "00042   ". For a string, precision is a maximum length instead, which
 *     handle_string applies before calling in here (it passes has_precision
 *     false), so the two never collide.
 *   - A '0' flag fills the *width* pad with zeros instead of spaces, but only
 *     if no precision was given, and only if '-' was not: C99 7.19.6.1p7 says
 *     the flag is ignored in both of those cases. That is why "%08.5d" of 42 is
 *     "   00042" and not "00000042".
 *   - Space padding goes outside the sign and '0' padding inside it, so "%5d"
 *     of -42 is "  -42" and "%05d" of -42 is "-0042".
 *
 * The digit floor and the width are kept separate because they add:
 * "%10.5d" of 42 is "     00042", five digits inside a ten-wide field.
 */
static void emit_padded(struct fmt_state *st, const char *body, size_t len,
			size_t precision, bool has_precision, int width,
			bool left_align, bool zero_pad, const char *sign)
{
	size_t sign_len = sign ? strlen(sign) : 0;
	size_t digit_pad;
	size_t width_pad;
	size_t zeros;
	bool zero_fill;
	size_t i;

	digit_pad = (has_precision && precision > len) ? precision - len : 0;

	zero_fill = zero_pad && !left_align && !has_precision;

	if (width > 0 && (size_t)width > len + sign_len + digit_pad)
		width_pad = (size_t)width - (len + sign_len + digit_pad);
	else
		width_pad = 0;

	zeros = digit_pad + (zero_fill ? width_pad : 0);

	if (!left_align && !zero_fill)
		for (i = 0; i < width_pad; i++)
			st->sink(' ', st->arg);

	if (sign)
		for (i = 0; i < sign_len; i++)
			st->sink(sign[i], st->arg);

	for (i = 0; i < zeros; i++)
		st->sink('0', st->arg);

	for (i = 0; i < len; i++)
		st->sink(body[i], st->arg);

	if (left_align)
		for (i = 0; i < width_pad; i++)
			st->sink(' ', st->arg);
}

/*
 * Apply the one precision rule that is about the value rather than the field:
 * C99 7.19.6.1p5 -- converting a zero value with a precision of zero produces
 * no characters at all, not "0". Anything else with a precision is handled by
 * emit_padded's digit floor.
 */
static size_t apply_zero_precision(char *buf, size_t len, int precision,
				   bool has_precision)
{
	if (has_precision && precision == 0 && len == 1 && buf[0] == '0')
		return 0;
	return len;
}

static void handle_string(struct fmt_state *st, int width, bool left_align,
			  int precision, bool has_precision)
{
	const char *s = va_arg(st->ap, const char *);
	size_t len;

	if (!s)
		s = "(null)";
	len = strlen(s);
	if (has_precision && precision >= 0 && (size_t)precision < len)
		len = (size_t)precision;
	emit_padded(st, s, len, 0, false, width, left_align, false, NULL);
}

static void handle_unsigned(struct fmt_state *st, unsigned long long value,
			    unsigned base, bool upper, int width, bool left_align,
			    bool zero_pad, int precision, bool has_precision)
{
	char buf[FMT_NUMBUF];
	size_t len = utoa(buf, sizeof(buf), value, base, upper);

	len = apply_zero_precision(buf, len, precision, has_precision);
	emit_padded(st, buf, len, (size_t)precision, has_precision, width,
		    left_align, zero_pad, NULL);
}

static void handle_signed(struct fmt_state *st, long long value, unsigned base,
			  int width, bool left_align, bool zero_pad,
			  int precision, bool has_precision, bool plus, bool blank)
{
	char buf[FMT_NUMBUF];
	bool negative = false;
	size_t len = itoa(buf, sizeof(buf), value, base, false, &negative);
	/* A negative value prints its own minus; '+' and ' ' only decorate a
	 * non-negative one, and neither is in the output if the value has a
	 * sign already. */
	const char *sign = negative ? "-" : (plus ? "+" : (blank ? " " : NULL));

	len = apply_zero_precision(buf, len, precision, has_precision);
	emit_padded(st, buf, len, (size_t)precision, has_precision, width,
		    left_align, zero_pad, sign);
}

void kvprintf(kvprintf_sink_t sink, void *arg, const char *fmt, va_list ap)
{
	struct fmt_state st = { .sink = sink, .arg = arg, .fmt = fmt };
	const char *p = fmt;

	/*
	 * The caller's va_list is consumed by an unknown number of arguments, so
	 * the walk happens on a private copy and the caller's is left untouched.
	 * That is what lets a caller reuse one va_list across several sinks.
	 *
	 * va_copy rather than a plain assignment: va_list is an array type on
	 * this ABI, so `st.ap = ap` would try to copy an array, which the
	 * compiler rejects outright. Passing it to va_copy decays both operands
	 * to the same struct pointer, which is what the copy actually means.
	 */
	va_copy(st.ap, ap);

	while (*p) {
		bool left_align = false, zero_pad = false;
		bool has_precision = false;
		bool plus = false, blank = false;
		int width = 0, precision = 0;
		int longness = 0;   /* 0 = int, 1 = long, 2 = long long, -1 = short */

		if (*p != '%') {
			st.sink(*p++, st.arg);
			continue;
		}
		p++;

		/*
		 * Flags, in any order and any number of times. Which one wins if
		 * both '-' and '0' are given is decided in emit_padded, which is
		 * the only place that knows what a sign is.
		 */
		for (;;) {
			if (*p == '-') {
				left_align = true;
				p++;
			} else if (*p == '0') {
				zero_pad = true;
				p++;
			} else if (*p == '+') {
				plus = true;
				p++;
			} else if (*p == ' ') {
				blank = true;
				p++;
			} else if (*p == '#') {
				p++;   /* accepted and ignored: no alternate form */
			} else {
				break;
			}
		}

		/* Width. */
		if (*p == '*') {
			width = va_arg(st.ap, int);
			p++;
			if (width < 0) {
				left_align = true;
				width = -width;
			}
		} else {
			while (*p >= '0' && *p <= '9')
				width = width * 10 + (*p++ - '0');
		}

		/* Precision. For integers this is a minimum digit count; for
		 * strings it is a maximum length. */
		if (*p == '.') {
			p++;
			has_precision = true;
			if (*p == '*') {
				precision = va_arg(st.ap, int);
				p++;
			} else {
				while (*p >= '0' && *p <= '9')
					precision = precision * 10 + (*p++ - '0');
			}
		}

		/* Length modifier. */
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
			else
				v = va_arg(st.ap, int);
			handle_signed(&st, v, 10, width, left_align, zero_pad,
				      precision, has_precision, plus, blank);
			break;
		}
		case 'u':
		case 'x':
		case 'X':
		case 'o':
		case 'b': {
			unsigned long long v;
			unsigned base;
			bool upper = (*p == 'X');

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
			else
				v = va_arg(st.ap, unsigned int);
			handle_unsigned(&st, v, base, upper, width, left_align,
					zero_pad, precision, has_precision);
			break;
		}
		case 'c': {
			char ch = (char)va_arg(st.ap, int);

			emit_padded(&st, &ch, 1, 0, false, width, left_align,
				    false, NULL);
			break;
		}
		case 's':
			handle_string(&st, width, left_align, precision,
				      has_precision);
			break;
		case 'p': {
			unsigned long long v = va_arg(st.ap, unsigned long long);
			char buf[FMT_NUMBUF];
			size_t len = utoa(buf, sizeof(buf), v, 16, false);
			/* Pointers are always 0x-prefixed and at least 16 hex
			 * digits, because an abbreviated kernel address is a
			 * debugging trap. */
			size_t zpad = len + 2 < 18 ? 18 - (len + 2) : 0;
			size_t spaces = 0;
			size_t i;

			/* An explicit width can widen the field but never
			 * narrow it below the 16 digits, and it fills with
			 * spaces: a '0' flag cannot make a pointer field
			 * start with a run of zeros before the 0x. */
			if (width > 0 && (size_t)width > len + 2 + zpad)
				spaces = (size_t)width - (len + 2 + zpad);

			if (!left_align)
				for (i = 0; i < spaces; i++)
					st.sink(' ', st.arg);
			st.sink('0', st.arg);
			st.sink('x', st.arg);
			for (i = 0; i < zpad; i++)
				st.sink('0', st.arg);
			for (i = 0; i < len; i++)
				st.sink(buf[i], st.arg);
			if (left_align)
				for (i = 0; i < spaces; i++)
					st.sink(' ', st.arg);
			break;
		}
		case '%':
			st.sink('%', st.arg);
			break;
		case '\0':
			/* Trailing '%': emit it rather than reading past the end. */
			st.sink('%', st.arg);
			continue;
		default:
			/* Unknown conversion: emit it verbatim so a typo in a
			 * format string is visible in the output instead of
			 * silently vanishing. */
			st.sink('%', st.arg);
			st.sink(*p, st.arg);
			break;
		}
		if (*p)
			p++;
	}

	va_end(st.ap);
}

/* ------------------------------------------------------------- buffer sink -- */

struct buffer_sink {
	char *buf;
	size_t size;
	size_t written;
};

static void buffer_putc(char c, void *arg)
{
	struct buffer_sink *bs = arg;

	/* Always NUL-terminate. One byte is reserved for the terminator even when
	 * the buffer is full, so ksnprintf output is always a valid C string. */
	if (bs->written + 1 < bs->size)
		bs->buf[bs->written] = c;
	bs->written++;
}

void kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	struct buffer_sink bs = { .buf = buf, .size = size, .written = 0 };

	if (size == 0)
		return;
	kvprintf(buffer_putc, &bs, fmt, ap);
	buf[bs.written < size ? bs.written : size - 1] = '\0';
}

ksize_t ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	kvsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return strlen(buf);
}

ksize_t kformat(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	kvsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return strlen(buf);
}
