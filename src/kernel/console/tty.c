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
#include <kstring.h>
#include <sched.h>
#include <spinlock.h>
#include <tty.h>
#include <types.h>
#include <cpu_features.h>
#include <drivers/serial.h>
#include <drivers/ps2kbd.h>
#include <drivers/ps2mouse.h>

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

/*
 * Screen ownership: at most one process owns the screen at a time. When
 * the screen is owned, console VGA and framebuffer output is skipped so
 * the owning program's rendering is not overwritten. Serial output always
 * passes through.
 *
 * The kernel enforces ownership: a process must call SYS_screen_claim to
 * gain ownership before it can successfully call SYS_gfx_present. The
 * claim is released automatically on process exit (via the task destructor
 * in sched.c or when the process explicitly calls SYS_screen_release).
 */

/*
 * Reported through tty_write_dropped() only once per burst; the input counter
 * is not reported from the handler at all, for the reason given at tty_in_push(). */
static bool out_dropped_reported;

/*
 * Screen ownership: at most one process owns the screen at a time. When
 * the screen is owned, console VGA and framebuffer output is skipped so
 * the owning program's rendering is not overwritten. Serial output always
 * passes through.
 *
 * The kernel enforces ownership: a process must call SYS_screen_claim to
 * gain ownership before it can successfully call SYS_gfx_present. The
 * claim is released automatically on process exit (via the task destructor
 * in sched.c or when the process explicitly calls SYS_screen_release).
 */
static u32 screen_owner_pid = 0;

/*
 * Claim or release ownership of the screen. While owned, console VGA and
 * framebuffer output is skipped so the owning program's rendering is not
 * overwritten. Serial output always passes through.
 *
 * The claim is exclusive: a second claim while the screen is owned by someone
 * else is rejected. The kernel rejects it and returns -EPERM from
 * SYS_screen_claim; a process whose present gets -EPERM prints a diagnostic and
 * falls back to scrolling text on the serial console. Release is always allowed.
 */
int tty_screen_claim(void)
{
    struct task *t = current_task();

    if (!t)
        return -EPERM;
    /* Already owned by someone else: reject. Not owned, or owned by us: claim. */
    if (tty_screen_owned() && tty_screen_owner_pid() != t->pid)
        return -EPERM;
    screen_owner_pid = t->pid;
    return 0;
}

void tty_screen_release(void)
{
    screen_owner_pid = 0;
}

bool tty_screen_owned(void)
{
    return screen_owner_pid != 0;
}

u32 tty_screen_owner_pid(void)
{
    return screen_owner_pid;
}

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
void tty_in_push(char c)
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

    /*
     * No echo here. Input echo belongs to whoever owns the line discipline:
     * init's REPL needs to place a cursor, insert in the middle of the line
     * and redraw, and a game polls keys without wanting them printed over its
     * screen. Echoing here made both impossible (and double-echoed the REPL).
     */
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
 * COM1 is the console's primary channel. It is polled, not interrupt-driven:
 * a UART whose receive interrupt does not arrive would otherwise make the
 * terminal permanently untypable with no log line to say why. Polled, that
 * machine reads its input. The cost is bounded: this runs once per pass of
 * tty_read()'s loop, so a reader that is not blocked pays one LSR read.
 *
 * It cannot double-consume: serial_getc() and com1_irq() both drain the same
 * receive FIFO, and a byte leaves the FIFO when the RBR is read. Whichever
 * of the two reads it first has it.
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

/* ------------------------------------------------------- PS/2 keyboard ---- */
/*
 * Keystroke delivery callback for the PS/2 keyboard driver.
 * The driver translates scancodes and calls this with the resulting ASCII char;
 * this wrapper feeds it into the tty input ring, where tty_read() will pick it up
 * and the echo logic in tty_in_push() will display it.
 */
static void keyboard_callback(char c)
{
    tty_in_push(c);
}

/*
 * The console reads raw bytes and echoes nothing, so its defaults say so. Output
 * processing is what the console always does for '\n', so OPOST and ONLCR are
 * reported as set. The control characters are the usual terminal ones.
 */
static struct termios tty_term;

static void tty_termios_defaults(struct termios *t)
{
    memset(t, 0, sizeof(*t));
    t->c_oflag = OPOST | ONLCR;
    t->c_cflag = CS8 | CREAD | CLOCAL;
    t->c_cc[VINTR] = 0x03;
    t->c_cc[VQUIT] = 0x1c;
    t->c_cc[VERASE] = 0x7f;
    t->c_cc[VKILL] = 0x15;
    t->c_cc[VEOF] = 0x04;
    t->c_cc[VMIN] = 1;
    t->c_cc[VTIME] = 0;
}

void tty_termios_get(struct termios *t)
{
    u64 flags = spinlock_irqsave(&in_lock);

    *t = tty_term;
    spinlock_unlock_irqrestore(&in_lock, flags);
}

void tty_termios_set(const struct termios *t, bool flush_input)
{
    u64 flags = spinlock_irqsave(&in_lock);

    tty_term = *t;
    if (flush_input) {
        in_ring.head = 0;
        in_ring.tail = 0;
        in_ring.count = 0;
    }
    spinlock_unlock_irqrestore(&in_lock, flags);
}

void tty_init(void)
{
    u64 flags = irq_save();

    tty_termios_defaults(&tty_term);
    uint8_t config = 0;

    pic_remap();

    /*
     * PS/2 keyboard: let the driver handle the controller, IRQ handler, and
     * PIC unmask. The driver calls our keyboard_callback() to deliver decoded
     * keystrokes into the input ring.
     */
    ps2kbd_init(keyboard_callback);

    /* The mouse shares the 8042 output buffer with the keyboard, so it is
     * initialised after the keyboard driver has written the controller
     * configuration byte it owns. */
    ps2mouse_init();

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
     * ps2kbd_init() already cleared the IRQ1 (keyboard) bit, so only COM1
     * (bit 4) is cleared here.
     *
     * Still before the PIC unmask below, so the line cannot become visible
     * before there is something behind it.
     */
    outb(COM1 + UART_IER,
         (uint8_t)(inb(COM1 + UART_IER) | UART_IER_RX_AVAILABLE));

    config = inb(PIC1_DATA_PORT);
    config &= (uint8_t)~PIC1_IRQ_COM1_BIT;
    outb(PIC1_DATA_PORT, config);

    /*
     * Read back what the mask actually became. Keyboard is handled by
     * ps2kbd_init(); only COM1 has to pass this check. This is a pin and
     * not politeness -- the whole difference between a tty that can be
     * typed at and one that cannot comes down to whether this bit cleared
     * or did not. OCPW1 reads back what the driver is using rather than
     * what was written to it.
     */
    config = inb(PIC1_DATA_PORT);
    if (config & PIC1_IRQ_COM1_BIT) {
        klog(KLOG_FATAL, "tty: COM1 unmask did not take, mask is %#x\n",
             (unsigned)config);
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
