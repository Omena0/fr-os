/*
 * tty.c — the kernel side of a process's terminal, and the PS/2 keyboard that
 * feeds it.
 *
 * Two rings, and neither of them is a scheduler. The output ring decouples a
 * process's write() from the console: a process must never block because the
 * UART is slow, and the console must never be entered with the ring lock held.
 * The input ring decouples the input interrupts from read(): both the keyboard
 * and COM1 push a byte, tty_read() takes it out, and a byte arriving while a
 * reader is asleep wakes that reader through the scheduler rather than making
 * the reader come and fetch it.
 *
 * Two input sources feed the one ring. The keyboard is on IRQ1 and COM1 on
 * IRQ4, and both are drained completely in their handlers. COM1 is also polled
 * once per pass of tty_read(), which is a fallback for a UART whose receive
 * interrupt does not arrive rather than a replacement for it -- see
 * tty_poll_serial().
 */
#include <console.h>
#include <interrupt.h>
#include <io.h>
#include <klog.h>
#include <sched.h>
#include <spinlock.h>
#include <tty.h>
#include <types.h>
#include <cpu_features.h>
#include <drivers/serial.h>

KLOG_SUBSYSTEM("tty");

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
	size_t space;
	size_t n;

	/*
	 * A ring with no storage is not a ring that should be pushed into, and
	 * `r->buf[r->head]` on a null buf is a write to address 0 -- which is
	 * indistinguishable, from the fault that reports it, from a wild store
	 * anywhere else. Refusing here and saying so is the difference between a
	 * located fault and a mystery. tty_init() checks the same invariant when
	 * it attaches the storage, so reaching this means something detached it
	 * afterwards.
	 */
	if (!r->buf) {
		klog(KLOG_FATAL, "tty: ring push with no storage attached\n");
		return 0;
	}

	space = TTY_RING_CAP - r->count;
	n = MIN(count, space);

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
 * The task blocked in tty_read(), if any.
 *
 * One pointer rather than a queue, because one tty has one reader: two processes
 * reading the same terminal is a race for bytes, not a queue, and a queue here
 * would only decide who lost it. A second reader arriving while one is blocked
 * overwrites this and the first is never woken -- which is a caller bug (both
 * are blocked in read() on the same descriptor) and not something a tty can
 * make correct.
 *
 * Guarded by in_lock, and that is the whole correctness argument for the
 * wakeup: tty_read() publishes and re-checks the ring under in_lock, and the
 * interrupt handler pushes and reads this pointer under the same in_lock. The
 * two therefore cannot interleave, which is what makes the publish-then-recheck
 * order in tty_read() sufficient rather than merely hopeful.
 */
static struct task *tty_waiter;

/*
 * Push one byte of input, and wake the reader waiting for it.
 *
 * Called from the keyboard interrupt and from the COM1 receive interrupt, both
 * of which run with interrupts already off, so the only contention is with a
 * reader on another CPU.
 *
 * The wakeup happens outside in_lock. sched_wake() takes the run queue's lock
 * and may switch tasks outright, and holding a ring lock across a context
 * switch is the lock nesting this file exists to avoid -- tty_drain() releases
 * out_lock before touching the console for exactly this reason. Taking the
 * waiter under the lock and calling sched_wake() after the unlock is safe
 * because the waiter has already been cleared: a second interrupt arriving in
 * the gap finds no waiter and leaves the task alone, rather than waking it
 * twice.
 *
 * A full input ring drops the new byte rather than the oldest. The old byte is
 * the one a program is most likely to be waiting on, and a program that is
 * overwritten loses data it had already consumed; the newest is the one no
 * reader has committed to yet.
 */
static void tty_in_push(char c)
{
	struct task *wake = NULL;
	u64 flags = spinlock_irqsave(&in_lock);

	/*
	 * No logging from here. This runs on every keystroke inside an interrupt
	 * handler, and reporting a full ring through the console would recurse
	 * into the 8042 handshakes on a controller that is by definition not
	 * keeping up. tty_read_dropped() is how the loss is found.
	 */
	if (ring_push(&in_ring, (const u8 *)&c, 1) == 0)
		in_dropped++;

	if (tty_waiter) {
		wake = tty_waiter;
		tty_waiter = NULL;
	}

	spinlock_unlock_irqrestore(&in_lock, flags);

	if (wake)
		sched_wake(wake);

	console_putc(c);
}

/*
 * Copy out of the input ring, waiting for a byte first if `block`.
 *
 * Waiting is a sleep, not a spin and not a halt: the reader records itself as
 * the tty's waiter, calls sched_block_current(), and an input interrupt calls
 * sched_wake() on it. Nothing polls here while a person takes to press a key,
 * and the CPU is not held by a halted core either -- the idle task is, which is
 * what it is for.
 *
 * The wakeup and the ring that triggers it must be published together, and
 * tty_waiter is guarded by in_lock for exactly that reason. See tty_read() for
 * the ordering and the three cases it has to survive.
 */
/*
 * Move anything the serial port has received into the input ring.
 *
 * COM1 is the console's primary channel -- run.sh's own header is a page about
 * why -- and it can receive the entire time. Nothing read it. `serial_getc()`
 * exists, works, and had no callers: the only consumer of COM1 was the transmit
 * side, so the console could print to the terminal and could not be typed at.
 * That is the gap this closes.
 *
 * This is a missing feature rather than a workaround for the PS/2 problem
 * alongside it. The two input paths are independent -- a keyboard and a serial
 * line are both legitimate sources for the same ring -- and a console with two
 * working channels is better than one with two broken ones.
 *
 * Kept, and kept polled, as the second consumer of the receive FIFO rather than
 * as a replacement for the interrupt. Two reasons, and both are load-bearing.
 *
 * The fallback: an interrupt-driven input path is only as good as the device
 * behind it. This one is the console's own port, and a UART that does not
 * interrupt -- or an 8250 whose IER is still zero because serial_init() writes
 * 0x00 there -- would otherwise make the terminal permanently untypable, with
 * nothing in the log to say why. Polled, that machine reads its input.
 *
 * And the cost is bounded: this runs once per pass of tty_read()'s loop, so a
 * reader that is not blocked pays one LSR read and nothing else.
 *
 * It cannot double-consume. serial_getc() and com1_irq() both drain the same
 * receive FIFO, and a byte leaves the FIFO when the RBR is read: whichever of
 * the two reads it first has it, and the other sees the data-ready bit clear.
 * There is no window in which both hold the same byte.
 */
static void tty_poll_serial(void)
{
	char c;

	while (serial_getc(&c))
		tty_in_push((uint8_t)c);
}

/* ------------------------------------------------- COM1 receive interrupt --- */

/*
 * Interrupt enable register. Offset 1 from the base, bit 0 = "received data
 * available".
 *
 * Named here rather than taken from drivers/serial.h because serial.h exposes
 * the base address and the line status bits and nothing else -- serial.c keeps
 * the IER bit definitions to itself, which is defensible for a header that
 * documents output. The consequence is that nobody outside serial.c can turn
 * the UART's interrupts on, and serial_init() writes 0x00 there: the receiver
 * is off from boot until something here sets it. Reported to whoever owns the
 * driver; until then this is where the bit is set, and it is set exactly once,
 * in tty_init().
 */
#define UART_IER          1
#define UART_IER_RX_AVAILABLE 0x01

/*
 * Bytes the receive interrupt handler will take in one pass.
 *
 * A 16550 receive FIFO holds 16. Four times that is comfortably more than the
 * device can hold, so the bound cannot be reached with bytes still in the FIFO
 * -- and it is there anyway because a poll with no bound is a hang, which is
 * this file's own rule for every other device poll in it.
 *
 * The bound is not what makes a burst arrive whole; draining to the data-ready
 * bit going clear is. The line is level-sensitive on the FIFO trigger level, so
 * a device that still had bytes left would re-raise it anyway.
 */
#define COM1_FIFO_CAP     16
#define COM1_DRAIN_LIMIT  (COM1_FIFO_CAP * 4)

/*
 * Master 8259 data port, shared with the keyboard's unmask below. The remap in
 * idt.c left both lines masked; IRQ1 is bit 1 of that byte and IRQ4 is bit 4.
 */
#define PIC1_DATA_PORT       0x21
#define PIC1_IRQ_KEYBOARD_BIT 0x02
#define PIC1_IRQ_COM1_BIT    0x10

/*
 * The pins. All four are about the same failure: an interrupt that is never
 * delivered because a number was wrong, which looks exactly like a device that
 * does not work.
 *
 * VECTOR_IRQ_COM1 was 37 for a while -- one higher than the remap delivers --
 * and while it was, every IRQ in the kernel landed on the wrong vector. Nothing
 * inside the kernel can see that: the handler is installed on the number it is
 * named, the CPU arrives on a different one, and the two only disagree in a
 * register dump nobody takes. This assert is the check that would have caught
 * it, evaluated by the compiler instead of by a boot.
 */
_Static_assert(VECTOR_IRQ_COM1 == VECTOR_IRQ_BASE + 4,
	       "COM1 is IRQ4 and the remap delivers VECTOR_IRQ_BASE + irq; a "
	       "handler on any other vector is never entered");
_Static_assert(VECTOR_IRQ_COM1 < VECTOR_STUB_MAX,
	       "interrupt_entry.S generates no stub at or past VECTOR_STUB_MAX, "
	       "so this vector would be absorbed by the default handler");
_Static_assert(VECTOR_IRQ_COM1 != VECTOR_IRQ_KEYBOARD,
	       "two drivers on one vector is the race idt_set_handler() warns "
	       "about");
_Static_assert(PIC1_IRQ_COM1_BIT == (1u << 4),
	       "IRQ4 is bit 4 of the master's interrupt mask register");
_Static_assert(UART_IER_RX_AVAILABLE == 1,
	       "bit 0 of the UART's interrupt enable register is the receiver");
_Static_assert(COM1_DRAIN_LIMIT >= COM1_FIFO_CAP,
	       "the drain bound must exceed what the receive FIFO can hold, or a "
	       "burst is truncated with bytes still in the device");

/*
 * The register offsets this file touches are not assertable against each other
 * and are deliberately not pretended to be. Only UART_IER is named here; the
 * receive buffer and line status offsets live inside serial.c and are reached
 * through serial_getc(), so a constant that would "check" them would be checking
 * this file's copy of a number rather than the driver's.
 */

/*
 * COM1 receive interrupt.
 *
 * Drains the FIFO completely, in a loop, and that loop is not a refinement.
 *
 * The receive interrupt is level-sensitive on the trigger level rather than
 * latched per byte: reading the line status register does not acknowledge
 * anything, and it is the FIFO dropping below the trigger level that ends the
 * interrupt. So a handler that takes one byte and returns has, at that
 * moment, a FIFO still above the trigger with an interrupt line still asserted
 * -- and the PIC has already been told, by the EOI that interrupt_dispatch()
 * issues on the way out, that IRQ4 has been serviced. Whether the next byte is
 * ever delivered then depends on the device re-asserting a line the
 * acknowledgement just told it was cleared. Taking the whole burst in one pass
 * is what removes the question.
 *
 * The same reasoning is why this cannot be a single serial_getc() call: the
 * loop is the handler.
 *
 * Nothing is logged from here. This runs in interrupt context with the console
 * lock reachable from the interrupted code, and tty_in_push() already declines
 * to log for the same reason. tty_read_dropped() and the boot line from
 * tty_init() are how the path reports itself.
 */
static void com1_irq(struct interrupt_frame *frame)
{
	unsigned int drained = 0;
	char c;

	(void)frame;

	while (drained < COM1_DRAIN_LIMIT && serial_getc(&c)) {
		tty_in_push((uint8_t)c);
		drained++;
	}
}

size_t tty_read(char *buf, size_t count, bool block)
{
	if (!buf || count == 0)
		return 0;

	for (;;) {
		u64 flags;
		struct task *me;
		size_t n;

		/* Serial first, so a byte that arrived while the ring was empty is
		 * seen before deciding the ring is empty at all. This is the
		 * fallback for a UART whose receive interrupt does not fire, and it
		 * shares the FIFO with the COM1 handler rather than duplicating it:
		 * a byte leaves the receive FIFO when the RBR is read, so whichever
		 * of the two gets there first has it and the other cannot push the
		 * same byte twice. */
		tty_poll_serial();

		me = block ? current_task() : NULL;

		flags = spinlock_irqsave(&in_lock);
		n = ring_pop(&in_ring, (u8 *)buf, count);

		/*
		 * Publish, then look again -- both under in_lock, which is what
		 * makes the pair atomic with respect to the interrupt handler.
		 *
		 * The gap this closes is real and is not hypothetical. Between the
		 * ring_pop() above finding nothing and this task being recorded as
		 * the waiter, an interrupt can arrive: the handler pushes the byte
		 * and reads tty_waiter, finds NULL, and wakes nobody. The byte is
		 * in the ring and this task is about to sleep on an empty ring, so
		 * nothing wakes it until the *next* byte arrives -- one keystroke
		 * late, forever if there is only ever one keystroke.
		 *
		 * Ordering it the other way is not available: the waiter cannot be
		 * published before the ring is found empty, because then a reader
		 * that finds a full ring would have to undo the publication, and
		 * the window would simply move.
		 *
		 * So the second ring_pop() runs after the publication and before
		 * the unlock, and both it and the handler's push-and-take-waiter
		 * hold in_lock. Exactly one of three things happens:
		 *
		 *   - the handler completes first: the byte is in the ring, this
		 *     pop finds it, and the task returns the data instead of
		 *     sleeping;
		 *   - this task completes first: the pop finds nothing, the waiter
		 *     is cleared, and the handler then sees no waiter -- but the
		 *     byte it pushed is still in the ring, so the loop's next pass
		 *     pops it;
		 *   - a reader on another CPU does it first: same as the second
		 *     case, and tty_waiter is cleared so the handler does not wake
		 *     a task that has already been served.
		 */
		if (n == 0 && me) {
			tty_waiter = me;
			n = ring_pop(&in_ring, (u8 *)buf, count);
			if (n != 0) {
				tty_waiter = NULL;
				spinlock_unlock_irqrestore(&in_lock, flags);
				return n;
			}
			/*
			 * Still empty. Keep tty_waiter set across the unlock and
			 * the block so an interrupt that delivers a byte after
			 * the second ring_pop sees a valid waiter and wakes us.
			 * Clearing it before sched_block_current() is the race
			 * that leaves a task blocked forever on the first byte.
			 */
			spinlock_unlock_irqrestore(&in_lock, flags);
			sched_block_current();
			tty_waiter = NULL;
			continue;
		}
		spinlock_unlock_irqrestore(&in_lock, flags);

		if (n != 0)
			return n;

		/* Nothing buffered and no wait requested. Reporting 0 is how a
		 * caller tells an idle terminal from a closed one, and it is what
		 * makes a poll loop terminate.
		 *
		 * `me == NULL` with block requested means current_task() gave
		 * nothing, and sched_block_current() would then be a no-op: the
		 * loop would spin here forever with no other task to run and
		 * nothing to wake it. Reporting 0 is the honest answer for a
		 * caller that is not a task. */
		if (!me)
			return 0;

		/*
		 * Nothing buffered and a wait was requested: give the CPU up.
		 *
		 * This used to `sti; hlt;` -- enable interrupts, halt, wait for
		 * something to wake the core. That is wrong in two separate ways,
		 * and both showed up.
		 *
		 * A terminal read that halts the whole processor is not a process
		 * blocking on input, it is the machine stopping: nothing else runs,
		 * nothing else can make progress, and the only thing that ever ends
		 * it is a device that may never speak. Yielding is what "block"
		 * means, and the scheduler already knows how.
		 *
		 * And enabling interrupts here is much worse than wasteful. Every
		 * syscall arrives with IF clear -- SYSCALL's SFMASK clears it -- so
		 * this was the *first* time a timer tick could land while a process
		 * was inside the kernel's syscall path. The kernel has never been
		 * tested that way and does not survive it: with interrupts enabled
		 * in a blocking read, runs died at `ret` in this function with a
		 * *user* address as the stack pointer, and elsewhere jumped to
		 * linear 0x400. Both are the interrupt/return path, not this loop.
		 *
		 * Yielding leaves interrupts off, exactly as every other blocking
		 * path in the kernel does, and lets the idle task run -- which is
		 * what keeps the timer ticking, since the idle task is what a
		 * halted CPU was pretending to be.
		 *
		 * Note what is deliberately NOT here, given that the two faults
		 * above both involved interrupt state. A syscall arrives with IF
		 * clear, and this function does not turn it back on: the idle task
		 * is the thing that re-enables interrupts around its hlt, and it
		 * does that for the whole machine regardless of who blocked last.
		 * Setting IF here would be the same enabling that killed the runs
		 * above, and it would also be redundant -- there is no state this
		 * function can leave behind that the idle loop does not already
		 * establish for itself.
		 *
		 * MEASURED, and it is the boundary of what is known about the
		 * wakeup below. With this file's half complete -- handler on vector
		 * 36, IRQ4 unmasked, guest read-back of the master's mask showing
		 * 0xec, and the UART's receive line visibly asserted (pic0 irr bit
		 * 4) -- com1_irq() still never runs. The machine takes exactly one
		 * timer tick and then delivers nothing further: over 26 s at
		 * 100 Hz, one `pit: tick` line (tick 100 never arrives), pic0
		 * reading isr=01 in 16 of 16 samples, the CPU in the idle halt in
		 * 40 of 40 samples. An 8259 blocks equal-or-lower-priority requests
		 * while an in-service bit is set, and IRQ4 is below IRQ0, so the
		 * COM1 handler is waiting on the timer path clearing, not on
		 * anything in this file. Reported to whoever owns idt.c.
		 */
		sched_block_current();
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
 * Every 8042 handshake here is a poll, and a poll with no bound is a hang: a
 * controller that is absent, wedged, or a machine with no PS/2 at all answers
 * nothing and never will. Every wait below is therefore a wait that can fail,
 * and the caller reports the failure and carries on with the terminal half
 * working rather than the machine stopped.
 */
#define PS2_SPIN_LIMIT (1u << 20)

/*
 * How long a 8042 handshake is waited for, in microseconds.
 *
 * This used to be a spin count -- 1 << 20 iterations, each with a port write in
 * `io_wait()` -- which is not a bound on anything. Under emulation a port write
 * is not free, and one timed-out wait was measured at **7.2 seconds**, with the
 * kernel stalled between `syscall entry installed` and the first `tty:` line.
 * Everything after it looked like a hang, and a boot that appears to freeze in
 * tty_init is indistinguishable from one that froze for a reason worth looking
 * at.
 *
 * A deadline in real time is what the caller actually means by "bounded", and it
 * behaves the same on every machine and under every accelerator. A millisecond is
 * far longer than any real 8042 takes to answer -- the parts are specified in
 * microseconds -- and far shorter than anyone can notice if it expires.
 */
#define PS2_WAIT_US 1000

/* Microseconds to TSC ticks, from the frequency cpu_features_init() measured.
 *
 * Falls back to a plausible constant when the frequency is unknown rather than
 * returning 0: a zero-length deadline would turn every wait into an immediate
 * failure, which looks like a missing controller rather than a missing
 * measurement. */
static u64 ps2_us_to_tsc(u64 us)
{
	if (!cpu_features.tsc_khz)
		return us * 3000;	/* ~3 GHz, the common case */
	return us * cpu_features.tsc_khz / 1000;
}

static bool ps2_wait(uint8_t mask, bool set)
{
	u64 deadline = rdtsc() + (u64)ps2_us_to_tsc(PS2_WAIT_US);

	for (;;) {
		uint8_t status = inb(PS2_STATUS);

		if (!!(status & mask) == set)
			return true;
		if (rdtsc() >= deadline)
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
 * 0xFA that acknowledges a command the kernel just sent.
 *
 * Takes the command byte so the failure can name it. "8042 answered 0x00 where
 * an ack was expected" identifies the controller's mood and nothing else: there
 * are several steps in the sequence and no way to tell which of them produced
 * it, which is how an unacknowledged enable-scanning -- the step that decides
 * whether a keystroke can raise an interrupt at all -- can look like a problem
 * with the keyboard rather than with the sequence.
 */
static void ps2_expect_ack(uint8_t cmd, uint8_t want)
{
	uint8_t resp = ps2_read_data();

	if (resp != want)
		klog(KLOG_WARN, "tty: 8042 command 0x%02x answered 0x%02x, "
		     "expected 0x%02x\n", (unsigned)cmd, (unsigned)resp,
		     (unsigned)want);
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

/* Master 8259 data port and the keyboard's line bit are defined with the COM1
 * ones above, so that both unmasked lines are named in the same place. */

static void keyboard_irq(struct interrupt_frame *frame)
{
	uint8_t status = inb(PS2_STATUS);

	/*
	 * Drain the output buffer, not one byte of it.
	 *
	 * Reading the status port clears the controller's interrupt line, and a
	 * new one is only generated when a byte arrives *after* that read. So a
	 * handler that takes one byte and returns discards the line while more
	 * bytes are still sitting there, and the rest of the burst is never
	 * delivered -- there is nothing left to interrupt for. Measured: five
	 * keystrokes injected into the guest produced one IRQ, and one character.
	 *
	 * Bounded by the same OBF bit that gates the loop, so it terminates as
	 * soon as the controller is empty, and it cannot spin on a controller
	 * that never clears OBF for longer than one pass is bounded by the
	 * scancode queue draining.
	 */
	for (unsigned guard = 0; guard < 32; guard++) {
		if (!(status & PS2_STATUS_OBF))
			return;

		/* The byte is from the mouse, which shares the controller. It has
		 * its own interrupt line and no driver yet; taking its byte here
		 * would steal it from whichever driver claims IRQ12. Leave it for
		 * the mouse's own interrupt rather than returning, or the byte
		 * stays in the buffer and the loop spins to its bound. */
		if (status & PS2_STATUS_SYS)
			return;

		keyboard_scancode(inb(PS2_DATA));

		/* Re-read: the byte we just took may have been the last, and the
		 * status must be sampled again to know that. */
		status = inb(PS2_STATUS);
	}
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

	/*
	 * Drain the controller before the first command, and do not run its self
	 * test.
	 *
	 * The self test was here, and it is the step everything downstream was
	 * getting wrong. The controller answers 0xAA with 0x55 and then 0xAA; some
	 * implementations -- QEMU among them -- answer with 0x55 alone. Consuming
	 * "the second byte" on the assumption that it is always there reads a reply
	 * that belongs to a *later* command, and then that command's real reply is
	 * never seen. Both readings were tried here and neither is reliable:
	 * trusting one byte leaves a stray 0xAA to be mistaken for the next reply,
	 * and trusting two swallows the next reply instead. The observed result was
	 * "8042 command 0xae answered 0x00" -- the enable-interface command,
	 * unanswered, which is why no key ever arrived.
	 *
	 * The self test is diagnostic, not setup. A controller that is absent leaves
	 * the port unreadable, which the bounded waits below already turn into a
	 * reported failure rather than a hang. Draining first gives a known-empty
	 * register, which is the property the sequence actually needs.
	 */	ps2_flush();

	if (ps2_write_port(PS2_CMD, PS2_CMD_ENABLE_KBD))
		/* 0xAE (enable keyboard interface) does not generate a response. */
		;

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
		ps2_expect_ack(KBD_CMD_ENABLE_SCAN, 0xFA);

	ps2_flush();

	spinlock_init(&out_lock);
	spinlock_init(&in_lock);
	out_ring.buf = out_storage;
	in_ring.buf  = in_storage;

	/*
	 * Read the storage back and complain here if it did not stick.
	 *
	 * The rings are declared with `.buf = NULL` and get their storage here, so
	 * a store that is lost, reordered past the calls below, or aimed at a
	 * different object leaves a ring that looks initialised and is not. Every
	 * later symptom is a write to address 0 from ring_push() on some process's
	 * first write -- which says nothing about where the value went, and has
	 * been seen to happen on roughly one boot in four and not at all on others.
	 *
	 * Checking it here turns that into a located failure at the point where the
	 * invariant is established, which is the only place it can be checked. It
	 * costs one load and one compare, once, before interrupts are re-enabled.
	 */
	if (out_ring.buf != out_storage || in_ring.buf != in_storage) {
		klog(KLOG_FATAL,
		     "tty: ring storage did not attach (out %p expected %p, "
		     "in %p expected %p)\n",
		     (void *)out_ring.buf, (void *)out_storage,
		     (void *)in_ring.buf, (void *)in_storage);
		return;
	}

	/*
	 * Handler before the line is unmasked, in that order and not the other
	 * way round. An unmasked line whose vector is still the default handler
	 * fires on every keystroke into a function that only counts, and worse,
	 * it re-arms the line from inside a handler that has already been
	 * entered by the same interrupt.
	 */
	idt_set_handler(VECTOR_IRQ_KEYBOARD, keyboard_irq, IST_NONE, 0);
	idt_set_handler(VECTOR_IRQ_COM1, com1_irq, IST_NONE, 0);

	/*
	 * The UART's own half of the same rule. serial_init() programs the
	 * interrupt enable register to 0x00, so the receiver has been raising
	 * nothing since boot and no amount of unmasking IRQ4 would produce an
	 * interrupt. Read-modify-write rather than a plain assignment: bit 1 is
	 * the transmit-empty interrupt, which belongs to whoever wants it, and
	 * silently clearing it here would make this file responsible for a
	 * decision it did not make.
	 *
	 * Still before the PIC unmask below, so the line cannot become visible
	 * before there is something behind it.
	 */
	outb(COM1 + UART_IER,
	     (uint8_t)(inb(COM1 + UART_IER) | UART_IER_RX_AVAILABLE));

	config = inb(PIC1_DATA_PORT);
	config &= (uint8_t)~(PIC1_IRQ_KEYBOARD_BIT | PIC1_IRQ_COM1_BIT);
	outb(PIC1_DATA_PORT, config);

	/*
	 * Read both lines back. This is a pin and not politeness: the two
	 * unmasked bits are the entire difference between a tty that can be
	 * typed at and one that cannot, and the failure mode is completely
	 * silent -- the poll still works, so the console keeps printing, the
	 * kernel looks healthy, and a person types at a terminal that is not
	 * listening. OCPW1 reads back the mask register, so this says what the
	 * device is actually using rather than what was written to it.
	 */
	config = inb(PIC1_DATA_PORT);
	if (config & (PIC1_IRQ_KEYBOARD_BIT | PIC1_IRQ_COM1_BIT)) {
		klog(KLOG_FATAL, "tty: PIC1 unmask did not take, mask is %#x "
		     "(keyboard %#x, COM1 %#x)\n", (unsigned)config,
		     (unsigned)!!(config & PIC1_IRQ_KEYBOARD_BIT),
		     (unsigned)!!(config & PIC1_IRQ_COM1_BIT));
		return;
	}

	if (!(inb(COM1 + UART_IER) & UART_IER_RX_AVAILABLE))
		klog(KLOG_FATAL, "tty: COM1 receive interrupt enable did not "
		     "take, IER is %#x\n", (unsigned)inb(COM1 + UART_IER));


	irq_restore(flags);

	klog(KLOG_INFO, "tty: rings %u bytes each, 8042 up, keyboard on "
	     "vector %u, COM1 receive on vector %u\n", (unsigned)TTY_RING_CAP,
	     VECTOR_IRQ_KEYBOARD, VECTOR_IRQ_COM1);
}
