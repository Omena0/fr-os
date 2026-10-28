/*
 * ps2kbd.c — PS/2 keyboard driver for the 8042 controller.
 *
 * The 8042 PS/2 controller sits at ports 0x60 (data), 0x64 (status/command).
 * IRQ1 fires when a scancode is available in the output buffer.
 *
 * This driver initializes the controller, installs the IRQ1 handler which
 * reads scancodes and translates them to ASCII, and delivers chars via a
 * registered callback.
 */

/* 8042 controller ports. There is no ps2_8042 helper in io.h — only the raw
 * inb/outb pair — so the three registers and the status bits they share are
 * named here. */
#define PS2_DATA     0x60
#define PS2_STATUS   0x64
#define PS2_CMD      0x64

#define PS2_STATUS_OBF  0x01   /* output buffer full: a byte is waiting */
#define PS2_STATUS_IBF  0x02   /* input buffer full: the controller is busy */
#define PS2_STATUS_SYS  0x04   /* the byte came from the aux port, not the kbd */

#define PS2_CMD_READ_CONFIG   0x20
#define PS2_CMD_WRITE_CONFIG  0x60
#define PS2_CMD_ENABLE_KBD    0xAE

/* Keyboard commands sent to the data port. */
#define KBD_CMD_ENABLE_SCAN   0xF4

/* Configuration byte bits. */
#define PS2_CFG_KBD_INT   0x01   /* raise IRQ1 for keyboard bytes */
#define PS2_CFG_KBD_CLOCK 0x10   /* 1 = keyboard clock held low, silent */
#define PS2_CFG_TRANSLATE 0x40   /* translate set 2 into set 1 scancodes */

#define PS2_WAIT_US    1000
#define PIC1_DATA_PORT 0x21
#define PIC1_IRQ_KEYBOARD_BIT 0x02

#include <drivers/ps2kbd.h>
#include <io.h>
#include <klog.h>
#include <cpu_features.h>
#include <types.h>

KLOG_SUBSYSTEM("ps2kbd");

/* --- 8042 controller I/O helpers --- */

static u64 ps2_us_to_tsc(u64 us)
{
    extern struct cpu_features cpu_features;
    if (!cpu_features.tsc_khz)
        return us * 3000;
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

static void ps2_expect_ack(uint8_t cmd, uint8_t want)
{
    uint8_t resp;

    if (!ps2_wait(PS2_STATUS_OBF, true))
        return;
    resp = inb(PS2_DATA);

    if (resp != want)
        klog(KLOG_WARN, "ps2kbd: 8042 command 0x%02x answered 0x%02x, "
             "expected 0x%02x\n", (unsigned)cmd, (unsigned)resp,
             (unsigned)want);
}

/* --- scancode translation --- */

/* Make codes 0x00-0x3F, set 1, unshifted. Zero = no ASCII. */
static const char scancode_base[0x40] = {
    [0x01] = 0x1B,
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
    [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=',
    [0x0E] = '\b',
    [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p',
    [0x1A] = '[', [0x1B] = ']',
    [0x1C] = '\n',
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

#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36
#define SC_LCTRL  0x1D

/* Modifier state, shared with tty.c for Ctrl+A-Z handling. */
bool shift_held;
bool ctrl_held;

static ps2kbd_callback_t keyboard_callback;

/* One scancode from port 0x60. Called from IRQ1 with interrupts off. */
static void keyboard_scancode(uint8_t sc)
{
    bool make = (sc & 0x80) == 0;
    uint8_t code = make ? sc : (uint8_t)(sc & 0x7F);
    char c;

    if (sc == 0xFA || sc == 0xFE || sc == 0xFF || (sc >= 0xF0 && sc <= 0xFD))
        return;

    if (sc == 0xE1)
        return;
    if (sc == 0xE0)
        return;

    if (code == SC_LSHIFT || code == SC_RSHIFT) {
        shift_held = make;
        return;
    }
    if (code == SC_LCTRL) {
        ctrl_held = make;
        return;
    }

    if (!make)
        return;

    /* Translate. */
    if (code >= ARRAY_SIZE(scancode_base))
        return;
    c = scancode_base[code];
    if (c == 0)
        return;

    /* Ctrl+letter: convert to Ctrl code. */
    if (ctrl_held && c >= 'a' && c <= 'z') {
        if (keyboard_callback)
            keyboard_callback((char)(c & 0x1F));
        return;
    }
    if (ctrl_held && c >= 'A' && c <= 'Z') {
        if (keyboard_callback)
            keyboard_callback((char)(c & 0x1F));
        return;
    }

    if (shift_held) {
        char s = 0;
        if (code < ARRAY_SIZE(scancode_shifted))
            s = scancode_shifted[code];
        if (s != 0) {
            c = s;
        } else if (c >= 'a' && c <= 'z') {
            c = (char)(c - ('a' - 'A'));
        }
    }

    if (keyboard_callback)
        keyboard_callback(c);
}

/* IRQ1 handler. Drains the 8042 output buffer to avoid losing keystrokes. */
void ps2kbd_irq(struct interrupt_frame *frame)
{
    uint8_t status;
    unsigned guard;

    UNUSED(frame);

    for (guard = 0; guard < 32; guard++) {
        status = inb(PS2_STATUS);

        if (!(status & PS2_STATUS_OBF))
            return;

        /* Mouse byte — leave it for the mouse driver. */
        if (status & PS2_STATUS_SYS)
            return;

        keyboard_scancode(inb(PS2_DATA));
        status = inb(PS2_STATUS);
    }
}

/* Initialize the PS/2 keyboard controller, install IRQ1 handler, and register
 * the callback for keystroke delivery. */
void ps2kbd_init(ps2kbd_callback_t cb)
{
    u64 flags = irq_save();
    uint8_t config;

    keyboard_callback = cb;

    /* Drain pre-existing bytes. */
    {
        unsigned int guard = 1024;
        while ((inb(PS2_STATUS) & PS2_STATUS_OBF) && guard--)
            (void)inb(PS2_DATA);
    }

    /* Enable keyboard interface (no response). */
    outb(PS2_CMD, PS2_CMD_ENABLE_KBD);

    /* Enable IRQ1 + scancode translation. */
    config = PS2_CFG_KBD_INT | PS2_CFG_TRANSLATE;
    config &= (uint8_t)~PS2_CFG_KBD_CLOCK;
    if (ps2_write_port(PS2_CMD, PS2_CMD_WRITE_CONFIG))
        ps2_write_port(PS2_DATA, config);

    /* Enable scanning. */
    if (ps2_write_port(PS2_DATA, KBD_CMD_ENABLE_SCAN))
        ps2_expect_ack(KBD_CMD_ENABLE_SCAN, 0xFA);

    /* Flush any response from the enable command. */
    {
        unsigned int guard = 1024;
        while ((inb(PS2_STATUS) & PS2_STATUS_OBF) && guard--)
            (void)inb(PS2_DATA);
    }

    /* Install the IRQ1 handler before unmasking. */
    idt_set_handler(VECTOR_IRQ_KEYBOARD, ps2kbd_irq, IST_NONE, 0);

    /* Unmask keyboard line in PIC. */
    config = inb(PIC1_DATA_PORT);
    config &= (uint8_t)~PIC1_IRQ_KEYBOARD_BIT;
    outb(PIC1_DATA_PORT, config);

    irq_restore(flags);
}
