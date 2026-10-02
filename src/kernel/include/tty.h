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

/*
 * Attach the ring storage, install the keyboard handler and turn scanning on.
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

/* Never blocks. Returns via tty_write_dropped() how much did not fit. */
void tty_write(const char *buf, size_t count);

/* Bytes dropped since the last call, reported once per burst. */
u64 tty_write_dropped(void);
u64 tty_read_dropped(void);

#endif /* TTY_H */
