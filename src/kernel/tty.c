/*
 * tty.c — the kernel side of a process's terminal, and the PS/2 keyboard that
 * feeds it.
 *
 * Two rings, and neither of them is a scheduler. The output ring decouples a
 * process's write() from the console: a process must never block because the
 * UART is slow, and the console must never be entered with the ring lock held.
 * The input ring decouples the keyboard interrupt from read(), which is the
 * arrangement the UART receive path is documented to use as well
 * (src/kernel/drivers/serial.c): the interrupt pushes a byte, and whoever reads
 * takes it out.
 *
 * The blocking read is a halt loop rather than a wait queue. See tty_read()
 * for why that is a deliberate interim choice and not an oversight.
 */
#include <console.h>
#include <interrupt.h>
#include <io.h>
#include <klog.h>
#include <spinlock.h>
#include <types.h>

KLOG_SUBSYSTEM("tty");

/*
 * Public interface, declared here because there is no tty.h to put it in.
 *
 * process.h declares tty_read() and tty_write() for process.c; tty_init() is
 * missing from every header and has to be added to one by whoever owns it. See
 * the report on this file.
 */
void tty_init(void);
size_t tty_read(char *buf, size_t count, bool block);
void tty_write(const char *buf, size_t count);
u64 tty_write_dropped(void);
u64 tty_read_dropped(void);

/* ------------------------------------------------------------- rings -------- */

/*
 * Both rings are the same structure, so they share one set of primitives. The
 * capacity is fixed and generous: the output ring only has to absorb the window
 * between a process's memcpy and the drain that follows it in the same call, and
 * the input ring only has to survive a program that does not read fast enough.
 * Neither is a place where a bigger buffer changes any behaviour other than how
 * often a drop is reported.
 */
#define TTY_RING_CAP 4096

struct tty_ring {
	u8    *buf;
	size_t head;   /* next position written */
	size_t tail;   /* next position read */
	size_t count;  /* bytes currently held */
};

/* Explicit count rather than a head/tail comparison: the ring is empty exactly
 * when the indices coincide, and keeping that invariant in one field means no
 * caller has to know how many modulo operations the indices have been through. */
static struct tty_ring out_ring = { .buf = NULL, .head = 0, .tail = 0, .count = 0 };
static struct tty_ring in_ring  = { .buf = NULL, .head = 0, .tail = 0, .count = 0 };

static u8 out_storage[TTY_RING_CAP];
static u8 in_storage[TTY_RING_CAP];

/*
 * Zero-initialised is a valid lock state ({0, 0} is SPINLOCK_INIT), so the rings
 * are usable before tty_init() runs. That matters: a process can be entered
 * before the keyboard is brought up, and a write() that faults on a half-built
 * terminal is a worse boot failure than a terminal that starts a moment late.
 * tty_init() still initialises them explicitly, so the order dependency is
 * visible rather than implied by .bss.
 */
static spinlock_t out_lock;
static spinlock_t in_lock;

/* Bytes that could not be stored. Counted rather than dropped quietly: a tty
 * that loses output without saying so produces a bug report about "random
 * missing text" that is close to impossible to reproduce. */
static u64 out_dropped;
static u64 in_dropped;

/* Reported through tty_write_dropped() only once per burst; the input counter
 * is not reported from the handler at all, for the reason given at tty_in_push(). */
static bool out_dropped_reported;

/* Amount copied out of the output ring before releasing the lock, so the console
 * is never entered with out_lock held. Small enough to stay on the stack. */
#define TTY_DRAIN_CHUNK 64

/* Copy in, returning how many bytes fit. The remainder is the caller's loss to
 * account for; the ring itself has no notion of a drop. */
static size_t ring_push(struct tty_ring *r, const u8 *src, size_t count)
{
	size_t space = TTY_RING_CAP - r->count;
	size_t n = MIN(count, space);

	for (size_t i = 0; i < n; i++) {
		r->buf[r->head] = src[i];
		if (++r->head == TTY_RING_CAP)
			r->head = 0;
	}
	r->count += n;
	return n;
}

/* Copy out, returning how many bytes were available. */
static size_t ring_pop(struct tty_ring *r, u8 *dst, size_t count)
{
	size_t n = MIN(count, r->count);

	for (size_t i = 0; i < n; i++) {
		dst[i] = r->buf[r->tail];
		if (++r->tail == TTY_RING_CAP)
			r->tail = 0;
	}
	r->count -= n;
	return n;
}

/* ------------------------------------------------------------- output ------- */

/*
 * Move everything in the output ring to the console.
 *
 * One chunk at a time, with the lock released in between, because the console
 * takes its own lock and the two must never nest. The bytes a process wrote are
 * already committed at that point: the drain is not part of write()'s contract
 * and cannot fail, so there is nothing to hold a lock across.
 *
 * Raw, not console_write(): a process writing 0x1B is writing a byte, and
 * interpreting it as a clear-screen would let a program scribble on the kernel
 * log. The kernel's own messages go through kprintf() and are unaffected.
 */
static void tty_drain(void)
{
	u8 chunk[TTY_DRAIN_CHUNK];

	for (;;) {
		u64 flags = spinlock_irqsave(&out_lock);
		size_t n = ring_pop(&out_ring, chunk, TTY_DRAIN_CHUNK);

		spinlock_unlock_irqrestore(&out_lock, flags);

		if (n == 0)
			return;
		console_write_raw((const char *)chunk, n);
	}
}

void tty_write(const char *buf, size_t count)
{
	size_t accepted;
	u64 dropped_now = 0;
	bool first = false;

	if (!buf || count == 0)
		return;

	u64 flags = spinlock_irqsave(&out_lock);

	accepted = ring_push(&out_ring, (const u8 *)buf, count);
	if (accepted < count) {
		dropped_now = count - accepted;
		out_dropped += dropped_now;
		/* Warn on the first drop of a burst only. Every drop reporting
		 * itself would fill the console with the very output that did not
		 * fit, which is a feedback loop. */
		first = !out_dropped_reported;
		out_dropped_reported = true;
	}

	spinlock_unlock_irqrestore(&out_lock, flags);

	if (first)
		klog(KLOG_WARN, "tty: output ring full, %lu byte(s) dropped "
		     "(%lu total)\n", (unsigned long)dropped_now,
		     (unsigned long)out_dropped);

	tty_drain();
}

u64 tty_write_dropped(void)
{
	u64 flags = spinlock_irqsave(&out_lock);
	u64 n = out_dropped;

	spinlock_unlock_irqrestore(&out_lock, flags);
	return n;
}

/* ------------------------------------------------------------- input -------- */

/*
 * Push one byte of input. Called only from the keyboard interrupt, which runs
 * with interrupts already off, so the only contention is with a reader on
 * another CPU.
 *
 * A full input ring drops the new byte rather than the oldest. The old byte is
 * the one a program is most likely to be waiting on, and a program that is
 * overwritten loses data it had already consumed; the newest is the one no
 * reader has committed to yet.
 */
static void tty_in_push(char c)
{
	u64 flags = spinlock_irqsave(&in_lock);

	/*
	 * No logging from here. This runs on every keystroke inside an interrupt
	 * handler, and reporting a full ring through the console would recurse
	 * into the 8042 handshakes on a controller that is by definition not
	 * keeping up. tty_read_dropped() is how the loss is found.
	 */
	if (ring_push(&in_ring, (const u8 *)&c, 1) == 0)
		in_dropped++;

	spinlock_unlock_irqrestore(&in_lock, flags);
}

/*
 * Copy out of the input ring, waiting for a byte first if `block`.
 *
 * Waiting is a halt loop, not a spin. hlt() is not a busy-wait: the CPU stops
 * until the next interrupt, which is exactly the condition being waited for,
 * and it releases the core to whatever else the machine wants to run. A spin
 * here would pin a CPU at 100% for the entire time a person takes to press a
 * key.
 *
 * The one thing that must not happen is halting with interrupts off, because
 * nothing can then wake the core and the machine stops. That is checked rather
 * than assumed: tty_read() is reachable from a syscall path whose interrupt
 * state is not this file's to guarantee, and a hang during early boot is far
 * worse than a read that reports "no input" when there was none to have.
 *
 * When the scheduler's wait-queue path lands, this becomes
 * sched_block_current()/sched_wake() and the reader is a sleep rather than a
 * halted core. The conditional below is the only part that changes: the ring
 * operations and the return value are already what a blocked reader needs.
 */
size_t tty_read(char *buf, size_t count, bool block)
{
	if (!buf || count == 0)
		return 0;

	for (;;) {
		u64 flags = spinlock_irqsave(&in_lock);
		size_t n = ring_pop(&in_ring, (u8 *)buf, count);

		spinlock_unlock_irqrestore(&in_lock, flags);

		if (n != 0)
			return n;

		/* Nothing buffered and no wait requested. Reporting 0 is how a
		 * caller tells an idle terminal from a closed one, and it is what
		 * makes a poll loop terminate. */
		if (!block)
			return 0;

		/* Nothing buffered, a wait was requested, and interrupts are off.
		 * hlt() would stop the core with nothing able to wake it, so the
		 * wait is declined. Returning 0 is the wrong answer for a caller
		 * that asked to block, and stopping the machine is a much worse
		 * one: this is the state a syscall path can be in during early
		 * init, and a terminal that hangs the kernel on a read is not
		 * recoverable from a serial log. */
		if (!irqs_enabled())
			return 0;

		hlt();
	}
}



u64 tty_read_dropped(void)
{
	u64 flags = spinlock_irqsave(&in_lock);
	u64 n = in_dropped;

	spinlock_unlock_irqrestore(&in_lock, flags);
	return n;
}

/* --------------------------------------------------------- PS/2 keyboard ---- */

/*
 * 8042 controller ports. There is no ps2_8042 helper in io.h — only the raw
 * inb/outb pair — so the three registers and the status bits they share are
 * named here.
 */
#define PS2_DATA     0x60
#define PS2_STATUS   0x64
#define PS2_CMD      0x64

#define PS2_STATUS_OBF  0x01   /* output buffer full: a byte is waiting */
#define PS2_STATUS_IBF  0x02   /* input buffer full: the controller is busy */
#define PS2_STATUS_SYS  0x04   /* the byte came from the aux port, not the kbd */

/* 8042 commands. */
#define PS2_CMD_READ_CONFIG   0x20
#define PS2_CMD_WRITE_CONFIG  0x60
#define PS2_CMD_ENABLE_KBD    0xAE
#define PS2_CMD_DISABLE_KBD   0xAD
#define PS2_CMD_SELF_TEST     0xAA

/* Keyboard commands, sent to the data port. */
#define KBD_CMD_ENABLE_SCAN   0xF4
#define KBD_CMD_DISABLE_SCAN  0xF5
#define KBD_CMD_RESET         0xFF

/* Configuration byte bits. */
#define PS2_CFG_KBD_INT   0x01   /* raise IRQ1 for keyboard bytes */
#define PS2_CFG_KBD_CLOCK 0x10   /* 1 = keyboard clock held low, silent */
#define PS2_CFG_TRANSLATE 0x40   /* translate set 2 into set 1 scancodes */

/*
 * A bounded wait on a status bit.
 *
 * Every 8042 handshake here is a poll, and a poll with no bound is a hang: a
 * controller that is absent, wedged, or a machine with no PS/2 at all answers
 * nothing and never will. Every wait below is therefore a wait that can fail,
 * and the caller reports the failure and carries on with the terminal half
 * working rather than the machine stopped.
 */
#define PS2_SPIN_LIMIT (1u << 20)

static bool ps2_wait(uint8_t mask, bool set)
{
	unsigned int spin = PS2_SPIN_LIMIT;

	for (;;) {
		uint8_t status = inb(PS2_STATUS);

		if (!!(status & mask) == set)
			return true;
		if (--spin == 0)
			return false;
		io_wait();
	}
}

static bool ps2_write_port(uint16_t port, uint8_t value)
{
	if (!ps2_wait(PS2_STATUS_IBF, false))
		return false;
	outb(port, value);
	return true;
}

/* Read one byte from the data port, assuming the caller has already established
 * that the output buffer is full. Returns 0 if it is not, so a spurious IRQ
 * cannot turn into a phantom keystroke. */
static uint8_t ps2_read_data(void)
{
	if (!ps2_wait(PS2_STATUS_OBF, true))
		return 0;
	return inb(PS2_DATA);
}

/* Throw away anything the controller already has queued. Done before the line
 * is unmasked, because a byte produced by the BIOS's own probing would
 * otherwise arrive as the first keystroke of the session. */
static void ps2_flush(void)
{
	unsigned int guard = 1024;

	while ((inb(PS2_STATUS) & PS2_STATUS_OBF) && guard--)
		(void)inb(PS2_DATA);
}

/* Consume one controller response, leaving the data register. Used to swallow the
 * 0xFA that acknowledges a command the kernel just sent. */
static void ps2_expect_ack(void)
{
	uint8_t resp = ps2_read_data();

	if (resp != 0xFA)
		klog(KLOG_WARN, "tty: 8042 answered 0x%02x where an ack was "
		     "expected\n", (unsigned)resp);
}

/* ---------------------------------------------- scancode translation -------- */

/* Make codes 0x00-0x3F, set 1, unshifted. A zero entry means the key produces no
 * byte: it is either a modifier, a function key, or part of a sequence this
 * driver does not decode yet. */
static const char scancode_base[0x40] = {
	[0x01] = 0x1B,                    /* Esc */
	[0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
	[0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
	[0x0A] = '9', [0x0B] = '0',
	[0x0C] = '-', [0x0D] = '=',
	[0x0E] = '\b',                    /* Backspace */
	[0x0F] = '\t',                    /* Tab */
	[0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
	[0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
	[0x18] = 'o', [0x19] = 'p',
	[0x1A] = '[', [0x1B] = ']',
	[0x1C] = '\n',                    /* Enter */
	[0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f',
	[0x22] = 'g', [0x23] = 'h', [0x24] = 'j', [0x25] = 'k',
	[0x26] = 'l',
	[0x27] = ';', [0x28] = '\'', [0x29] = '`',
	[0x2B] = '\\',
	[0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
	[0x30] = 'b', [0x31] = 'n', [0x32] = 'm',
	[0x33] = ',', [0x34] = '.', [0x35] = '/',
	[0x39] = ' ',
};

/*
 * The shifted entries that are not simply an upper-case letter. Letters are
 * derived instead of tabled, so this table is only the punctuation that moves,
 * and adding a keyboard layout means editing punctuation rather than 26 letters
 * that are all the same transformation.
 */
static const char scancode_shifted[0x40] = {
	[0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$',
	[0x06] = '%', [0x07] = '^', [0x08] = '&', [0x09] = '*',
	[0x0A] = '(', [0x0B] = ')',
	[0x0C] = '_', [0x0D] = '+',
	[0x1A] = '{', [0x1B] = '}',
	[0x27] = ':', [0x28] = '"', [0x29] = '~',
	[0x2B] = '|',
	[0x33] = '<', [0x34] = '>', [0x35] = '?',
};

/* Make codes with no ASCII of their own. */
#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36

static bool shift_held;

/*
 * True once an 0xE0 prefix has been seen. The prefix itself is not a key, and
 * the byte that follows belongs to the extended block — the arrows, keypad
 * Enter, right Ctrl — most of which reuse make codes that mean letters on the
 * main block. Swallowing only the prefix would make an arrow key type a
 * letter, so the whole two-byte sequence is dropped.
 */
static bool extended_pending;

static char tty_translate(uint8_t make)
{
	char c;

	if (make >= ARRAY_SIZE(scancode_base))
		return 0;

	c = scancode_base[make];
	if (c == 0)
		return 0;

	if (!shift_held)
		return c;

	char s = scancode_shifted[make];
	if (s != 0)
		return s;
	if (c >= 'a' && c <= 'z')
		return (char)(c - ('a' - 'A'));
	return c;
}

/* One scancode from port 0x60. Called from the IRQ handler, with interrupts
 * off, so the decoder state below needs no lock of its own. */
static void keyboard_scancode(uint8_t sc)
{
	bool make = (sc & 0x80) == 0;
	uint8_t code = make ? sc : (uint8_t)(sc & 0x7F);
	char c;

	/* Set 2 IDs, command acknowledgements, resend requests, and errors. None
	 * of them is a keystroke, and treating 0xFA as a key would put a byte
	 * nobody typed into the read stream on every command the kernel issues. */
	if (sc == 0xFA || sc == 0xFE || sc == 0xFF || (sc >= 0xF0 && sc <= 0xFD)) {
		/* A resend means the controller rejected the last command byte.
		 * The commands this driver sends are all self-contained, so the
		 * recovery is to drop the request and let the next one proceed; a
		 * command that genuinely needs repeating will be re-issued by
		 * whatever noticed the failure. */
		return;
	}

	/* Prefix, not a key. 0xE0 introduces the extended block; 0xE1 introduces
	 * the pause sequence, which is two more bytes for a key that produces
	 * nothing. Both are treated the same way: the byte that follows is
	 * consumed with them and never reaches the input ring. */
	if (sc == 0xE0 || sc == 0xE1) {
		extended_pending = true;
		return;
	}

	/* Everything else in 0xE0-0xFF is a controller response or an ID, not a
	 * key. The make codes this driver decodes all live below 0x80. */
	if (sc > 0xE0)
		return;

	if (extended_pending) {
		extended_pending = false;
		return;
	}

	/* Shift is state, not input. It has to be tracked on make and break
	 * because a terminal is entirely shift-driven: nothing is readable while
	 * it is held, and leaving it latched after the key is released turns
	 * every later key into upper case. */
	if (code == SC_LSHIFT || code == SC_RSHIFT) {
		shift_held = make;
		return;
	}

	/* Only make codes produce input. A break code means the key came back up,
	 * and feeding it in would double every keystroke. */
	if (!make)
		return;

	c = tty_translate(code);
	if (c != 0)
		tty_in_push(c);
}

/* Master 8259 data port. The remap in idt.c left both lines masked, and IRQ1 is
 * bit 1 of that byte. */
#define PIC1_DATA_PORT 0x21
#define PIC1_IRQ_KEYBOARD_BIT 0x02

static void keyboard_irq(struct interrupt_frame *frame)
{
	uint8_t status = inb(PS2_STATUS);

	/* Nothing to collect. Reading the data port anyway would return the last
	 * byte the controller sent, turning a spurious interrupt into a phantom
	 * character. */
	if (!(status & PS2_STATUS_OBF))
		return;

	/* The byte is from the mouse, which shares the controller. It has its own
	 * interrupt line and no driver yet; taking its byte here would steal it
	 * from whichever driver claims IRQ12. */
	if (status & PS2_STATUS_SYS)
		return;

	keyboard_scancode(inb(PS2_DATA));
}

/* pic_remap() masks every line, so it must happen exactly once. Called from
 * main.c directly it would be once; called from anywhere else that also brings
 * up devices it would not be. The guard is a bool rather than a "who is first"
 * convention because the second call's only effect is to break the first. */
static bool pic_remapped;

void tty_init(void)
{
	u64 flags = irq_save();
	uint8_t config = 0;

	if (pic_remapped) {
		irq_restore(flags);
		return;
	}
	pic_remapped = true;

	/*
	 * Remap first: the keyboard interrupt arrives on the vector the remap
	 * produces, and until it has run the vector in the IDT is not the one
	 * the controller will raise.
	 */
	pic_remap();

	/* The controller may not be present. Everything below is best-effort
	 * after the self test fails, and each step is allowed to fail, because a
	 * machine with no PS/2 controller still has a perfectly good terminal —
	 * it just cannot be typed at. */
	if (ps2_write_port(PS2_CMD, PS2_CMD_SELF_TEST)) {
		uint8_t result = ps2_read_data();

		if (result != 0x55) {
			klog(KLOG_WARN, "tty: 8042 self test returned 0x%02x; "
			     "keyboard input disabled\n", (unsigned)result);
			irq_restore(flags);
			return;
		}
	} else {
		klog(KLOG_WARN, "tty: 8042 did not answer; keyboard input "
		     "disabled\n");
		irq_restore(flags);
		return;
	}

	/* A reset leaves the controller with scanning off and the config byte
	 * cleared, so both have to be re-established after it. */
	if (ps2_write_port(PS2_DATA, KBD_CMD_RESET))
		ps2_expect_ack();
	if (ps2_write_port(PS2_CMD, PS2_CMD_ENABLE_KBD))
		ps2_expect_ack();

	ps2_flush();

	/*
	 * Interrupt on keyboard bytes, keyboard clock not held low, and scancode
	 * translation left on: this driver decodes set 1, and translation is what
	 * turns the controller's set 2 into it. Setting it explicitly rather than
	 * trusting the BIOS matters because the byte the BIOS left behind is
	 * whatever the last BIOS keystroke left, and the two halves have to
	 * agree on which set is arriving.
	 */
	if (ps2_write_port(PS2_CMD, PS2_CMD_READ_CONFIG))
		config = ps2_read_data();
	config |= PS2_CFG_KBD_INT | PS2_CFG_TRANSLATE;
	config &= (uint8_t)~PS2_CFG_KBD_CLOCK;
	if (ps2_write_port(PS2_CMD, PS2_CMD_WRITE_CONFIG))
		ps2_write_port(PS2_DATA, config);

	/* Scanning on, or no interrupt is ever generated no matter what the
	 * config byte says. */
	if (ps2_write_port(PS2_DATA, KBD_CMD_ENABLE_SCAN))
		ps2_expect_ack();

	ps2_flush();

	spinlock_init(&out_lock);
	spinlock_init(&in_lock);
	out_ring.buf = out_storage;
	in_ring.buf  = in_storage;

	/*
	 * Handler before the line is unmasked, in that order and not the other
	 * way round. An unmasked line whose vector is still the default handler
	 * fires on every keystroke into a function that only counts, and worse,
	 * it re-arms the line from inside a handler that has already been
	 * entered by the same interrupt.
	 */
	idt_set_handler(VECTOR_IRQ_KEYBOARD, keyboard_irq, IST_NONE, 0);

	config = inb(PIC1_DATA_PORT);
	config &= (uint8_t)~PIC1_IRQ_KEYBOARD_BIT;
	outb(PIC1_DATA_PORT, config);

	irq_restore(flags);

	klog(KLOG_INFO, "tty: rings %u bytes each, 8042 up, keyboard on "
	     "vector %u\n", (unsigned)TTY_RING_CAP, VECTOR_IRQ_KEYBOARD);
}
