/*
 * tty.h — the console tty: a ring-buffered line between the serial/VGA console
 * and user processes' stdin and stdout.
 *
 * These prototypes used to live in a local block at the top of tty.c, with a
 * comment explaining that there was no tty.h to put them in. That had a cost
 * beyond tidiness: tty_init() was declared nowhere, so nothing outside tty.c
 * could see it, and **kmain never called it**. The rings are initialised with
 * `.buf = NULL` and their storage is attached in tty_init(), so the first write
 * from a user process dereferenced a null pointer and the kernel died on it --
 * at `r->buf[r->head] = src[i]` in ring_push(), with CR2 = 0.
 *
 * The symptom pointed nowhere near the cause: the fault was a ring-0 page
 * fault at address 0 in the tty, reached from a perfectly ordinary write() from
 * PID 1, and "the tty was never initialised" was not something the call site
 * had any way to say.
 */
#ifndef TTY_H
#define TTY_H

#include <types.h>
#include <uapi/syscall.h>

/*
 * Screen ownership APIs. Call tty_screen_claim() when a process wants exclusive
 * access to the screen (e.g. a fullscreen graphics program), and
 * tty_screen_release() when it no longer owns it. The console skips VGA and
 * framebuffer writes while the screen is owned, but serial output always
 * passes through.
 *
 * The claim is exclusive: a second claim while the screen is owned is a
 * no-op and the second caller does not gain ownership. Release is always
 * allowed.
 */
/*
 * Claim or release ownership of the screen. While owned, console VGA and
 * framebuffer output is skipped so the owning program's rendering is not
 * overwritten. Serial output always passes through.
 *
 * The claim is exclusive: a second claim while the screen is owned by someone
 * else is rejected. Release is always allowed.
 *
 * Returns 0 on success, or -EPERM if the screen is already owned by another
 * process.
 */
int tty_screen_claim(void);
void tty_screen_release(void);
bool tty_screen_owned(void);
u32 tty_screen_owner_pid(void);

/*
 * Attach the ring storage, bring up the PS/2 keyboard driver, and unmask COM1.
 *
 * Must be called from kmain after idt_init() -- it calls idt_set_handler(), and
 * it deliberately installs the keyboard handler *before* unmasking the line, so
 * a keystroke cannot arrive with only the default handler behind it -- and
 * before any process is created, since process creation hands PID 1 file
 * descriptors that point at these rings.
 */
void tty_init(void);

/* Block until at least one byte is available, unless `block` is false. */
size_t tty_read(char *buf, size_t count, bool block);

/*
 * The console's termios. The flags are stored and reported; the console does
 * not yet act on ICANON, ECHO or ISIG, so reads are raw and nothing is echoed.
 * tty_termios_set() with flush_input discards unread input (TCSETSF).
 */
void tty_termios_get(struct termios *t);
void tty_termios_set(const struct termios *t, bool flush_input);

/* Never blocks. Returns via tty_write_dropped() how much did not fit. */
void tty_write(const char *buf, size_t count);

/* Bytes dropped since the last call, reported once per burst. */
u64 tty_write_dropped(void);
u64 tty_read_dropped(void);

/* Bytes a read() would return right now: buffered input in raw mode, a
 * committed line (or EOF) in canonical mode. */
size_t tty_readable(void);

/* Push one byte into the tty input ring. Called from interrupt context. */
void tty_in_push(char c);

#endif /* TTY_H */

