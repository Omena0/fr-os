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
/* Bit 2 is the system/self-test flag (1 after a successful POST), NOT the
 * source of the byte. Auxiliary (mouse) data is flagged by the auxiliary
 * output-buffer-full bit, bit 5. Treating bit 2 as "aux" discarded every
 * keyboard byte on QEMU, where the self-test flag is set. */
#define PS2_STATUS_AUX  0x20   /* output buffer holds data from the aux port */

#define PS2_CMD_READ_CONFIG   0x20
#define PS2_CMD_WRITE_CONFIG  0x60
#define PS2_CMD_ENABLE_KBD    0xAE

/* Keyboard commands sent to the data port. */
#define KBD_CMD_ENABLE_SCAN   0xF4
#define KBD_CMD_SET_TYPEMATIC 0xF3

/*
 * Typematic parameter: bits 6-5 select the delay before a held key repeats,
 * bits 4-0 the repeat period. 0x05 is delay 00 (250 ms) and rate index 5
 * (75 ms, about 13 repeats per second). The keyboard's default is 500 ms,
 * which made held movement in the games feel unresponsive; 250 ms starts
 * repeating quickly without the 33 ms machine-gun rate of index 0.
 *
 * This configures the PS/2 keyboard only. A serial terminal's auto-repeat is
 * the terminal emulator's setting, not the guest's.
 */
#define KBD_TYPEMATIC_VALUE   0x05

/* Configuration byte bits. */
#define PS2_CFG_KBD_INT   0x01   /* raise IRQ1 for keyboard bytes */
#define PS2_CFG_KBD_CLOCK 0x10   /* 1 = keyboard clock held low, silent */
#define PS2_CFG_TRANSLATE 0x40   /* translate set 2 into set 1 scancodes */

#define PS2_WAIT_US    1000
#define PIC1_DATA_PORT 0x21
#define PIC1_IRQ_KEYBOARD_BIT 0x02

#include <drivers/ps2kbd.h>
#include <drivers/ps2mouse.h>
#include <io.h>
#include <klog.h>
#include <cpu_features.h>
#include <wait.h>
#include <types.h>
#include <utf8.h>

KLOG_SUBSYSTEM("ps2kbd");

/* --- 8042 controller I/O helpers --- */

static bool ps2_wait(uint8_t mask, bool set)
{
    /*
     * Bounded by wall-clock time, not by an iteration count: a port access
     * under emulation can be orders of magnitude slower than on the host, so
     * a loop count that is a millisecond on hardware is a multi-second hang
     * here. wait_spin_us() keeps the busy-wait (this runs before the
     * scheduler exists, so it cannot sleep) but owns the deadline arithmetic.
     */
    u64 deadline = rdtsc() + wait_us_to_tsc(PS2_WAIT_US);

    for (;;) {
        uint8_t status = inb(PS2_STATUS);

        if (!!(status & mask) == set)
            return true;
        if ((s64)(rdtsc() - deadline) >= 0)
            return false;
        io_wait();
    }
}

static bool ps2_write_port(uint16_t port, uint8_t value)
{
    if (!ps2_wait(PS2_STATUS_IBF, false)) {
        klog(KLOG_DEBUG, "ps2kbd: port 0x%04x write 0x%02x blocked (IBF stuck)\n",
              (unsigned)port, (unsigned)value);
        return false;
    }
    klog(KLOG_DEBUG, "ps2kbd: outb(0x%04x, 0x%02x)\n",
          (unsigned)port, (unsigned)value);
    outb(port, value);
    return true;
}

static void ps2_expect_ack(uint8_t cmd, uint8_t want)
{
    uint8_t resp;

    if (!ps2_wait(PS2_STATUS_OBF, true)) {
        klog(KLOG_DEBUG, "ps2kbd: 0x%02x timed out waiting for ack\n",
              (unsigned)cmd);
        return;
    }
    resp = inb(PS2_DATA);

    if (resp != want)
        klog(KLOG_WARN, "ps2kbd: 8042 command 0x%02x answered 0x%02x, "
             "expected 0x%02x\n", (unsigned)cmd, (unsigned)resp,
             (unsigned)want);
    else
        klog(KLOG_DEBUG, "ps2kbd: 0x%02x acked 0x%02x\n",
              (unsigned)cmd, (unsigned)resp);
}

/* --- scancode translation --- */

/*
 * The layout is chosen at build time (KBD_LAYOUT in src/config.mk) because a
 * PS/2 keyboard is not self-describing: there is no protocol message that says
 * "I am Finnish". The detector reads the host's X11/vconsole keymap and the
 * kernel compiles in the matching tables.
 *
 * Letters are in the same physical places in both layouts (Finnish is QWERTY,
 * not QWERTZ); what differs is punctuation and the three Nordic letter keys.
 *
 * The Nordic letters (å, ä, ö) and the dead keys are not ASCII and the console
 * is byte-oriented with an 8-bit font, so those keys currently produce
 * nothing rather than a wrong ASCII character. Everything needed for the REPL
 * and the games is ASCII.
 */
#if defined(KBD_LAYOUT_FI)

/* Make codes 0x00-0x3F, set 1, unshifted: Finnish (fi). */
static const u16 scancode_base[0x60] = {
    [0x01] = 0x1B,
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
    [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '+', [0x0D] = '`',          /* +? and the acute dead key */
    [0x0E] = '\b',
    [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p',
    [0x1A] = 0x00E5,         /* å */
    [0x1B] = 0x00A8,         /* dead ¨ */
    [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f',
    [0x22] = 'g', [0x23] = 'h', [0x24] = 'j', [0x25] = 'k',
    [0x26] = 'l',
    [0x27] = 0x00F6,         /* ö */
    [0x28] = 0x00E4,         /* ä */
    [0x29] = 0x00A7,         /* § */
    [0x2B] = '\'',                       /* the ' * key */
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
    [0x30] = 'b', [0x31] = 'n', [0x32] = 'm',
    [0x33] = ',', [0x34] = '.', [0x35] = '-',
    [0x39] = ' ',
    [0x56] = '<',
};

static const u16 scancode_shifted[0x60] = {
    [0x02] = '!', [0x03] = '"', [0x04] = '#', [0x05] = '\xa4',   /* ¤ */
    [0x06] = '%', [0x07] = '&', [0x08] = '/', [0x09] = '(',
    [0x0A] = ')', [0x0B] = '=',
    [0x0C] = '?', [0x0D] = '`',          /* grave */
    [0x1A] = 0x00C5,         /* Å */
    [0x1B] = 0x005E,         /* dead ^ */
    [0x27] = 0x00D6,         /* Ö */
    [0x28] = 0x00C4,         /* Ä */
    [0x29] = 0x00BD,         /* ½ */
    [0x2B] = '*',
    [0x33] = ';', [0x34] = ':', [0x35] = '_',
    [0x56] = '>',
};

static const u16 scancode_altgr[0x60] = {
    [0x03] = '@',
    [0x04] = '$',
    [0x05] = 0x20AC,             /* € */
    [0x07] = '{', [0x08] = '[', [0x09] = ']', [0x0A] = '}',
    [0x0C] = '\\',
    [0x0D] = '|',
    [0x1A] = '~', [0x1B] = '^',
    [0x2B] = 0x0060,             /* altgr + ' -> backtick */
};


#elif defined(KBD_LAYOUT_US)

/* Make codes 0x00-0x3F, set 1, unshifted: US. */
static const u16 scancode_base[0x60] = {
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
    [0x56] = '<',
};

static const u16 scancode_shifted[0x60] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$',
    [0x06] = '%', [0x07] = '^', [0x08] = '&', [0x09] = '*',
    [0x0A] = '(', [0x0B] = ')',
    [0x0C] = '_', [0x0D] = '+',
    [0x1A] = '{', [0x1B] = '}',
    [0x27] = ':', [0x28] = '"', [0x29] = '~',
    [0x2B] = '|',
    [0x33] = '<', [0x34] = '>', [0x35] = '?',
};

static const u16 scancode_altgr[0x60] = { 0 };

#else
#error "KBD_LAYOUT must be US or FI (see src/config.mk)"
#endif

#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36
#define SC_LCTRL  0x1D

/* Modifier state, shared with tty.c for Ctrl+A-Z handling. */
bool shift_held;
bool ctrl_held;
bool altgr_held;
static u32 dead_key;   /* combining accent waiting for a base letter */

/* True after an E0 prefix, so the next code is an extended key (arrows,
 * keypad, etc.) rather than a normal scancode. */
static bool e0_prefix;

static ps2kbd_callback_t keyboard_callback;

/* Last typematic parameter successfully applied, so a caller can query or
 * restore it. */
static u8 typematic_value = KBD_TYPEMATIC_VALUE;

/*
 * Which scancodes are physically down, bit sc for sc < 64. The keyboard sends
 * make and break codes; the break half was previously discarded, which made
 * "is this key held" unanswerable and forced games to rely on the keyboard's
 * typematic repeat. A program that wants its own delay and rate needs the key
 * state, not repeated characters.
 *
 * Scancodes for the keys games use (letters, space, comma, period, digits) are
 * all below 0x40, so one word is enough. Updated from IRQ1 and read by a
 * syscall; a 64-bit aligned load/store is atomic on x86-64.
 */
static u64 key_mask;

/* Deliver a Unicode code point to the tty as UTF-8, one byte per callback. */
static void deliver(u32 cp)
{
    u8 buf[4];
    size_t n = utf8_encode(cp, buf);
    if (keyboard_callback)
        for (size_t i = 0; i < n; i++)
            keyboard_callback((char)buf[i]);
}

/* Combining mark for a dead-key accent, or 0 if not a dead key. */
static u32 dead_mark(u32 cp)
{
    switch (cp) {
    case 0x00A8: return 0x0308;   /* ¨ -> combining diaeresis */
    case 0x00B4: return 0x0301;   /* ´ -> combining acute */
    case 0x005E: return 0x0302;   /* ^  -> combining circumflex */
    case 0x0060: return 0x0300;   /* `  -> combining grave */
    case 0x007E: return 0x0303;   /* ~  -> combining tilde */
    default: return 0;
    }
}

/* Compose a base letter with a combining mark into a precomposed Latin-1
 * character, or return 0 if no precomposed form exists. */
static u32 compose(u32 base, u32 mark)
{
    switch (mark) {
    case 0x0300: /* grave */
        switch (base) {
        case 'A': return 0x00C0; case 'E': return 0x00C8; case 'I': return 0x00CC;
        case 'O': return 0x00D2; case 'U': return 0x00D9;
        case 'a': return 0x00E0; case 'e': return 0x00E8; case 'i': return 0x00EC;
        case 'o': return 0x00F2; case 'u': return 0x00F9;
        default: return 0;
        }
    case 0x0301: /* acute */
        switch (base) {
        case 'A': return 0x00C1; case 'E': return 0x00C9; case 'I': return 0x00CD;
        case 'O': return 0x00D3; case 'U': return 0x00DA; case 'Y': return 0x00DD;
        case 'a': return 0x00E1; case 'e': return 0x00E9; case 'i': return 0x00ED;
        case 'o': return 0x00F3; case 'u': return 0x00FA; case 'y': return 0x00FD;
        default: return 0;
        }
    case 0x0302: /* circumflex */
        switch (base) {
        case 'A': return 0x00C2; case 'E': return 0x00CA; case 'I': return 0x00CE;
        case 'O': return 0x00D4; case 'U': return 0x00DB;
        case 'a': return 0x00E2; case 'e': return 0x00EA; case 'i': return 0x00EE;
        case 'o': return 0x00F4; case 'u': return 0x00FB;
        default: return 0;
        }
    case 0x0303: /* tilde */
        switch (base) {
        case 'A': return 0x00C3; case 'N': return 0x00D1; case 'O': return 0x00D5;
        case 'a': return 0x00E3; case 'n': return 0x00F1; case 'o': return 0x00F5;
        default: return 0;
        }
    case 0x0308: /* diaeresis */
        switch (base) {
        case 'A': return 0x00C4; case 'E': return 0x00CB; case 'I': return 0x00CF;
        case 'O': return 0x00D6; case 'U': return 0x00DC; case 'Y': return 0x00FF;
        case 'a': return 0x00E4; case 'e': return 0x00EB; case 'i': return 0x00EF;
        case 'o': return 0x00F6; case 'u': return 0x00FC; case 'y': return 0x00FF;
        default: return 0;
        }
    default:
        return 0;
    }
}

/* The spacing (standalone) accent corresponding to a combining mark, so a
 * dead key pressed without a composable base letter still yields a visible
 * accent instead of an invalid combining mark. Returns 0 if none. */
static u32 spacing_accent(u32 mark)
{
    switch (mark) {
    case 0x0300: return 0x0060;   /* grave  ` */
    case 0x0301: return 0x00B4;   /* acute  ´ */
    case 0x0302: return 0x005E;   /* circumflex ^ */
    case 0x0303: return 0x007E;   /* tilde  ~ */
    case 0x0308: return 0x00A8;   /* diaeresis ¨ */
    default: return 0;
    }
}

/* One scancode from port 0x60. Called from IRQ1 with interrupts off. */
static void keyboard_scancode(uint8_t sc)
{
    bool make = (sc & 0x80) == 0;
    uint8_t code = make ? sc : (uint8_t)(sc & 0x7F);
    u32 cp;

    klog(KLOG_DEBUG, "ps2kbd: scancode 0x%02x (%s, code 0x%02x)\n",
          (unsigned)sc, make ? "make" : "break", (unsigned)code);

    if (sc == 0xFA || sc == 0xFE || sc == 0xFF || (sc >= 0xF0 && sc <= 0xFD)) {
        klog(KLOG_DEBUG, "ps2kbd: special scancode 0x%02x dropped\n",
              (unsigned)sc);
        return;
    }

    if (sc == 0xE1) {
        klog(KLOG_DEBUG, "ps2kbd: 0xE1 prefix dropped\n");
        return;
    }
    if (sc == 0xE0) {
        e0_prefix = true;
        return;
    }
    /* Track press/release for the scancodes we can hold. E0-prefixed keys have
     * their own path below and are not needed for text input. */
    if (code < 64) {
        if (make)
            key_mask |= 1ULL << code;
        else
            key_mask &= ~(1ULL << code);
    }

    if (e0_prefix) {
        /* Extended keys. Emit the same escape sequences a serial terminal
         * sends, so the tty/game input path does not need a second protocol.
         * Only make codes act; the break code ends the key. */
        const char *seq = NULL;

        e0_prefix = false;
        if (code == 0x38) {   /* right Alt = AltGr */
            altgr_held = make;
            klog(KLOG_DEBUG, "ps2kbd: altgr %s\n", make ? "pressed" : "released");
            return;
        }
        if (!make)
            return;
        switch (code) {
        case 0x48: seq = "\033[A"; break;   /* up */
        case 0x50: seq = "\033[B"; break;   /* down */
        case 0x4D: seq = "\033[C"; break;   /* right */
        case 0x4B: seq = "\033[D"; break;   /* left */
        case 0x1C:                          /* keypad Enter */
            if (keyboard_callback)
                keyboard_callback('\n');
            return;
        default:
            return;
        }
        if (keyboard_callback)
            for (const char *p = seq; *p; p++)
                keyboard_callback(*p);
        return;
    }

    if (code == SC_LSHIFT || code == SC_RSHIFT) {
        shift_held = make;
        klog(KLOG_DEBUG, "ps2kbd: shift %s\n", make ? "pressed" : "released");
        return;
    }
    if (code == SC_LCTRL) {
        ctrl_held = make;
        klog(KLOG_DEBUG, "ps2kbd: ctrl %s\n", make ? "pressed" : "released");
        return;
    }

    /*
     * Ctrl+Backspace. A serial terminal usually sends ^W for this (and
     * "delete word" is ^W in the line editor anyway), so map it to the same
     * byte rather than delivering a plain backspace and losing the modifier.
     */
    if (ctrl_held && code == 0x0E) {
        if (make && keyboard_callback)
            keyboard_callback(0x17);
        return;
    }

    if (!make) {
        klog(KLOG_DEBUG, "ps2kbd: break for 0x%02x dropped\n", (unsigned)code);
        return;
    }

    /* Translate. */
    if (code >= ARRAY_SIZE(scancode_base)) {
        klog(KLOG_DEBUG, "ps2kbd: code 0x%02x out of range\n",
              (unsigned)code);
        return;
    }
    cp = scancode_base[code];
    if (cp == 0) {
        klog(KLOG_DEBUG, "ps2kbd: code 0x%02x unmapped\n",
              (unsigned)code);
        return;
    }

    /* Ctrl+letter: convert to Ctrl code. */
    if (ctrl_held && cp >= 'a' && cp <= 'z') {
        if (keyboard_callback) {
            klog(KLOG_DEBUG, "ps2kbd: ctrl+%c -> 0x%02x\n", (char)cp,
                  (unsigned)(cp & 0x1F));
            keyboard_callback((char)(cp & 0x1F));
        }
        return;
    }
    if (ctrl_held && cp >= 'A' && cp <= 'Z') {
        if (keyboard_callback) {
            klog(KLOG_DEBUG, "ps2kbd: ctrl+%c -> 0x%02x\n", (char)cp,
                  (unsigned)(cp & 0x1F));
            keyboard_callback((char)(cp & 0x1F));
        }
        return;
    }

    if (altgr_held) {
        u32 a = 0;
        if (code < ARRAY_SIZE(scancode_altgr))
            a = scancode_altgr[code];
        if (a != 0) cp = a;
    } else if (shift_held) {
        u32 sv = 0;
        if (code < ARRAY_SIZE(scancode_shifted))
            sv = scancode_shifted[code];
        if (sv != 0) cp = sv;
        else if (cp >= 'a' && cp <= 'z') cp = (u16)(cp - ('a' - 'A'));
    }

    /* Dead keys (¨ ´ ^ ` ~) wait for a base letter and compose into a
     * precomposed Latin-1 character (e.g. ´ + a -> á). */
    if (dead_key) {
        u32 composed = 0;
        if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
            cp == 'N' || cp == 'n')
            composed = compose(cp, dead_key);
        if (composed) {
            cp = composed;
        } else {
            /* No precomposed form: emit the standalone spacing accent,
             * then this key normally (e.g. ´ + space -> ´ ). */
            u32 spacing = spacing_accent(dead_key);
            if (spacing)
                deliver(spacing);
        }
        dead_key = 0;
    }
    if (dead_mark(cp)) {
        dead_key = dead_mark(cp);
        klog(KLOG_DEBUG, "ps2kbd: dead key pending\n");
        return;
    }

    if (keyboard_callback) {
        klog(KLOG_DEBUG, "ps2kbd: delivering U+%04x to tty\n", cp);
        deliver(cp);
    } else {
        klog(KLOG_DEBUG, "ps2kbd: U+%04x no callback registered\n", cp);
    }
}

/* IRQ1 handler. Drains the 8042 output buffer to avoid losing keystrokes. */
void ps2kbd_irq(struct interrupt_frame *frame)
{
    uint8_t status;
    unsigned guard;

    UNUSED(frame);

    klog(KLOG_DEBUG, "ps2kbd: IRQ1 fired\n");
    for (guard = 0; guard < 32; guard++) {
        status = inb(PS2_STATUS);

        if (!(status & PS2_STATUS_OBF)) {
            klog(KLOG_DEBUG, "ps2kbd: drained %u bytes, OBF clear\n", guard);
            return;
        }

        /* A mouse byte. IRQ1 and IRQ12 can both fire for the same output
         * buffer, and the mouse IRQ may be the one that does not run; hand the
         * byte to the mouse driver instead of dropping it. */
        if (status & PS2_STATUS_AUX) {
            u8 aux = inb(PS2_DATA);

            ps2mouse_handle_byte(aux);
            status = inb(PS2_STATUS);
            continue;
        }

        keyboard_scancode(inb(PS2_DATA));
        status = inb(PS2_STATUS);
    }
    klog(KLOG_WARN, "ps2kbd: drained 32 bytes without OBF clearing\n");
}

/*
 * Change the keyboard's typematic delay/rate. Returns the previous value.
 *
 * This is a runtime setting, not a build-time one: a game wants a shorter
 * delay and a faster rate than an interactive prompt, and must be able to put
 * the old value back when it exits. Keystrokes already waiting in the output
 * buffer are discarded, because the command's ACK and a keystroke both arrive
 * as OBF bytes and reading a key as the ack would desynchronise the sequence.
 */
u8 ps2kbd_set_typematic(u8 value)
{
    u8 previous = typematic_value;
    u64 flags;

    if (value & 0x80)
        return previous;

    flags = irq_save();

    for (unsigned i = 0; i < 64 && (inb(PS2_STATUS) & PS2_STATUS_OBF); i++)
        (void)inb(PS2_DATA);

    if (ps2_write_port(PS2_DATA, KBD_CMD_SET_TYPEMATIC)) {
        if (ps2_wait(PS2_STATUS_OBF, true) && inb(PS2_DATA) == 0xFA) {
            if (ps2_write_port(PS2_DATA, value)) {
                if (ps2_wait(PS2_STATUS_OBF, true) &&
                    inb(PS2_DATA) == 0xFA) {
                    typematic_value = value;
                    klog(KLOG_INFO,
                         "ps2kbd: typematic %#02x -> %#02x\n",
                         (unsigned)previous, (unsigned)value);
                }
            }
        }
    }

    irq_restore(flags);
    return previous;
}

u64 ps2kbd_key_mask(void)
{
    return key_mask;
}

u8 ps2kbd_get_typematic(void)
{
    return typematic_value;
}

/* Initialize the PS/2 keyboard controller, install IRQ1 handler, and register
 * the callback for keystroke delivery. */
void ps2kbd_init(ps2kbd_callback_t cb)
{
    u64 flags = irq_save();
    uint8_t config;

    keyboard_callback = cb;

    klog(KLOG_INFO, "ps2kbd: scancode layout %s\n",
#if defined(KBD_LAYOUT_FI)
         "fi"
#else
         "us"
#endif
         );
    klog(KLOG_DEBUG, "ps2kbd: init start\n");
    /* Drain pre-existing bytes. */
    {
        unsigned int guard = 1024;
        while ((inb(PS2_STATUS) & PS2_STATUS_OBF) && guard--) {
            uint8_t stale = inb(PS2_DATA);
            klog(KLOG_DEBUG, "ps2kbd: drained pre-existing 0x%02x\n",
                  (unsigned)stale);
        }
    }

    /* Enable keyboard interface (no response). */
    klog(KLOG_DEBUG, "ps2kbd: enable kbd\n");
    outb(PS2_CMD, PS2_CMD_ENABLE_KBD);

    /* Enable IRQ1 + scancode translation. */
    config = PS2_CFG_KBD_INT | PS2_CFG_TRANSLATE;
    config &= (uint8_t)~PS2_CFG_KBD_CLOCK;
    if (ps2_write_port(PS2_CMD, PS2_CMD_WRITE_CONFIG))
        ps2_write_port(PS2_DATA, config);

    /* Enable scanning. */
    if (ps2_write_port(PS2_DATA, KBD_CMD_ENABLE_SCAN))
        ps2_expect_ack(KBD_CMD_ENABLE_SCAN, 0xFA);

    /*
     * Set typematic delay/rate: a held key starts repeating after 250 ms and
     * then repeats about 13 times a second. The keyboard acknowledges the
     * command and then the parameter, so both acks are consumed here.
     */
    if (ps2_write_port(PS2_DATA, KBD_CMD_SET_TYPEMATIC)) {
        ps2_expect_ack(KBD_CMD_SET_TYPEMATIC, 0xFA);
        if (ps2_write_port(PS2_DATA, KBD_TYPEMATIC_VALUE))
            ps2_expect_ack(KBD_TYPEMATIC_VALUE, 0xFA);
    }

    /* Flush any response from the enable command. */
    {
        unsigned int guard = 1024;
        while ((inb(PS2_STATUS) & PS2_STATUS_OBF) && guard--) {
            klog(KLOG_DEBUG, "ps2kbd: flushed enable-ack 0x%02x\n",
                  (unsigned)inb(PS2_DATA));
        }
    }

    /* Install the IRQ1 handler before unmasking. */
    idt_set_handler(VECTOR_IRQ_KEYBOARD, ps2kbd_irq, IST_NONE, 0);

    /* Unmask keyboard line in PIC. */
    config = inb(PIC1_DATA_PORT);
    config &= (uint8_t)~PIC1_IRQ_KEYBOARD_BIT;
    outb(PIC1_DATA_PORT, config);

    irq_restore(flags);
}
