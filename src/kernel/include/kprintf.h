/*
 * kprintf.h — formatted kernel output.
 *
 * Two layers, deliberately separated:
 *
 *   kvprintf() formats into a caller-supplied sink function. Everything else
 *   in this file is built on it, so adding an output path (framebuffer,
 *   trace buffer, panic dump) means writing one small adapter rather than a
 *   second formatter.
 *
 *   kprintf()/kpanicf() write to the console. They are safe to call from any
 *   context, including interrupt handlers, because the console serialises
 *   writes with a spinlock and never allocates.
 *
 * No floating point. The kernel has no FPU state save/restore on every path, and
 * a %f in a log message is not worth the context-switch cost of enabling the FPU
 * for one frame.
 */
#ifndef KPRINTF_H
#define KPRINTF_H

#include <types.h>
#include <stdarg.h>

/* The sink receives one character at a time. Return value is ignored. */
typedef void (*kvprintf_sink_t)(char c, void *arg);

/*
 * Format into `sink`. Supports:
 *   %d %i   signed decimal, with width, zero pad, and '-' left justify
 *   %u      unsigned decimal
 *   %x %X   unsigned hex
 *   %o      unsigned octal
 *   %b      unsigned binary
 *   %c      character
 *   %s      NUL-terminated string, with an optional precision (max chars)
 *   %p      pointer, always 0x-prefixed and 16 hex digits minimum
 *   %%      literal percent
 *
 * Length modifiers: l, ll, z, h, hh. Width and precision may be given as
 * literals or through '*'.
 *
 * Padding follows C99 7.21.6.1, because a log that mis-pads a fixed column is
 * worse than no log: '-' left-justifies, '0' zero-pads (but is ignored when a
 * precision is given, and a '-' overrides it), '+' and ' ' force a sign on a
 * non-negative value, and a precision is a minimum digit count for numbers and
 * a maximum length for strings. There is no '#' alternate form, so a hex field
 * that wants a prefix has to spell it out: "0x%016lx", not "%#lx".
 */
void kvprintf(kvprintf_sink_t sink, void *arg, const char *fmt, va_list ap);

/* Same, but varargs. */
void kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
ksize_t ksnprintf(char *buf, size_t size, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

/* Convenience formatting for small fixed fields (register dumps, panic output). */
ksize_t kformat(char *buf, size_t size, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
#endif /* KPRINTF_H */
