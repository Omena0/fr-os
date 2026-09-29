/*
 * console.h — the kernel's unified text output.
 *
 * Everything the kernel prints goes through here. The console fans out to
 * whatever backends are available, in order of how much they can be trusted:
 *
 *   1. Serial (COM1). Always present, works with interrupts off and after a
 *      panic. The ground truth for what the kernel said.
 *   2. A framebuffer text console, when stage2 programmed a linear framebuffer.
 *      This is what a human reads.
 *   3. The legacy VGA text buffer at 0xB8000, when there is no framebuffer. It
 *      works without any prior mode set, which makes it the right default on a
 *      machine where VESA was unavailable.
 *
 * Writes are serialised with a spinlock. Two CPUs printing concurrently
 * interleave mid-line otherwise, which is not a cosmetic problem: a garbled
 * panic message is the one message nobody can afford to garble.
 *
 * The lock is acquired with interrupts disabled on the calling CPU. A deadlock
 * would otherwise be possible if an interrupt handler on the same CPU tried to
 * print while the interrupted context held the lock.
 */
#ifndef CONSOLE_H
#define CONSOLE_H

#include <types.h>
#include <boot.h>

/* Terminal escape sequences the console interprets. */
#define TERM_ESC_CLEAR      "\033[2J"
#define TERM_ESC_HOME       "\033[H"
#define TERM_ESC_COLOR(n)   "\033[" #n "m"

#define TERM_ATTR_NORMAL    0x07
#define TERM_ATTR_BOLD      0x0F
#define TERM_ATTR_RED       0x4C
#define TERM_ATTR_GREEN     0x4A
#define TERM_ATTR_YELLOW    0x4E
#define TERM_ATTR_BLUE      0x44
#define TERM_ATTR_CYAN      0x4B
#define TERM_ATTR_MAGENTA   0x4D

/*
 * Bring up every available backend.
 *
 * `bi` may be NULL, in which case the VGA text buffer is the only backend and
 * is used without any mode having been set. That path matters: it is what
 * makes the kernel produce visible output on a machine where stage2 could not
 * find a linear framebuffer.
 */
void console_init(struct bootinfo *bi);

/* Write a string. Escapes are interpreted. */
void console_write(const char *s);

/* Write a string without escape interpretation, for dumping raw bytes. */
void console_write_raw(const char *s, size_t n);

void console_putc(char c);

/* Formatted output. */
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kvprintf_to_console(const char *fmt, __builtin_va_list ap);

void console_set_attr(uint8_t attr);
uint8_t console_get_attr(void);

/* Terminal geometry, as seen by the console. */
uint32_t console_columns(void);
uint32_t console_rows(void);

/* Where the console currently is, for the framebuffer/GUI work later. */
void console_clear(void);

/*
 * Emit a single character with a specific foreground colour, bypassing the
 * current attribute. Used by the panic renderer and, later, by the GUI.
 */
void console_putc_attr(char c, uint8_t attr);

/* Scrollback: the number of lines that have scrolled off the top. */
uint64_t console_scrollback(void);

/*
 * Formatted output to the console. Defined in console.c, and routed to every
 * sink console_init() attached: serial, and the framebuffer text console.
 *
 * The format string supports what kvprintf() supports and nothing more. In
 * particular there is no '#' flag, so a hex field that wants a prefix has to
 * spell it out — "0x%016lx", not "%#lx". See kprintf.h.
 */
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* CONSOLE_H */
