/*
 * ps2mouse.c — PS/2 mouse over the 8042 auxiliary port.
 *
 * The controller multiplexes two devices onto one output buffer. The keyboard
 * driver reads that buffer too (the two IRQ lines can both fire for the same
 * byte), so the byte decoder here is exported and the keyboard IRQ forwards
 * auxiliary bytes instead of discarding them.
 *
 * A standard three-byte packet:
 *   byte 0  YO XO YS XS 1 M R L   (bit 3 is always 1: the sync bit)
 *   byte 1  X movement, 8 bits
 *   byte 2  Y movement, 8 bits
 *
 * Movement is relative and in the device's frame, where +Y is up; the screen's
 * +Y is down. The driver keeps the absolute position, so applications never
 * have to integrate deltas themselves and cannot disagree about where the
 * pointer is.
 */
#include <io.h>
#include <klog.h>
#include <types.h>
#include <stdbool.h>
#include <interrupt.h>
#include <wait.h>
#include <drivers/ps2mouse.h>
#include <drivers/ps2kbd.h>
#include "console.h"

KLOG_SUBSYSTEM("ps2mouse");

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64

#define PS2_STATUS_OBF  0x01
#define PS2_STATUS_IBF  0x02
#define PS2_STATUS_AUX  0x20

#define PS2_CMD_READ_CONFIG  0x20
#define PS2_CMD_WRITE_CONFIG 0x60
#define PS2_CMD_ENABLE_AUX   0xA8
#define PS2_CMD_AUX_PREFIX   0xD4

#define PS2_CFG_AUX_INT   0x02
#define PS2_CFG_AUX_CLOCK 0x20   /* 1 = clock held low, device silent */

#define MOUSE_CMD_SET_DEFAULTS 0xF6
#define MOUSE_CMD_ENABLE       0xF4
#define MOUSE_ACK              0xFA

#define PIC1_DATA 0x21
#define PIC2_DATA 0xA1

#define PS2_WAIT_US 1000

static u16 screen_w = 640, screen_h = 400;
static s32 cursor_x, cursor_y;
static u8 buttons;
static bool present;

static u8 packet[3];
static unsigned packet_len;

static bool ps2_wait(u8 mask, bool set)
{
    /* Same bounded-by-wall-clock rule as the keyboard driver; see wait.h. */
    u64 deadline = rdtsc() + wait_us_to_tsc(PS2_WAIT_US);

    for (;;) {
        u8 status = inb(PS2_STATUS);

        if (!!(status & mask) == set)
            return true;
        if ((s64)(rdtsc() - deadline) >= 0)
            return false;
        io_wait();
    }
}

static bool ps2_write(u16 port, u8 value)
{
    if (!ps2_wait(PS2_STATUS_IBF, false))
        return false;
    outb(port, value);
    return true;
}

/* Send a command byte to the auxiliary device (via the 0xD4 prefix) and wait
 * for its ACK, which arrives in the output buffer marked as auxiliary. */
static bool aux_command(u8 cmd)
{
    if (!ps2_write(PS2_CMD, PS2_CMD_AUX_PREFIX))
        return false;
    if (!ps2_write(PS2_DATA, cmd))
        return false;
    if (!ps2_wait(PS2_STATUS_OBF, true))
        return false;

    u8 ack = inb(PS2_DATA);

    if (ack != MOUSE_ACK) {
        klog(KLOG_WARN, "mouse: command 0x%02x answered 0x%02x\n",
             (unsigned)cmd, (unsigned)ack);
        return false;
    }
    return true;
}

static void clamp_position(void)
{
    if (cursor_x < 0)
        cursor_x = 0;
    if (cursor_y < 0)
        cursor_y = 0;
    if (cursor_x > (s32)screen_w - 1)
        cursor_x = (s32)screen_w - 1;
    if (cursor_y > (s32)screen_h - 1)
        cursor_y = (s32)screen_h - 1;
}

static void apply_packet(void)
{
    u8 flags = packet[0];
    s32 dx = packet[1];
    s32 dy = packet[2];

    /* Sign bits live in the flag byte, and the movement is a 9-bit two's
     * complement value spread across two bytes. */
    if (flags & 0x10)
        dx -= 256;
    if (flags & 0x20)
        dy -= 256;

    /* Overflow means the device outran the sampling; the delta is unusable,
     * but the buttons in this packet are still current. */
    if (!(flags & 0x40))
        cursor_x += dx;
    if (!(flags & 0x80))
        cursor_y -= dy;          /* device +Y is up, screen +Y is down */

    clamp_position();

    buttons = (u8)(flags & 0x07);
    present = true;
}

void ps2mouse_handle_byte(u8 byte)
{
    /* Byte 0 must have the sync bit set. If it does not, this is a desynced
     * stream (or a stray command reply); resync on the next byte that looks
     * like a packet header rather than interpreting garbage as movement. */
    if (packet_len == 0 && !(byte & 0x08))
        return;

    packet[packet_len++] = byte;
    if (packet_len == 3) {
        packet_len = 0;
        apply_packet();
    }
}

void ps2mouse_irq(struct interrupt_frame *frame)
{
    UNUSED(frame);

    for (unsigned guard = 0; guard < 32; guard++) {
        u8 status = inb(PS2_STATUS);

        if (!(status & PS2_STATUS_OBF))
            return;
        if (!(status & PS2_STATUS_AUX)) {
            /* A keyboard byte delivered on the wrong line: leave it for IRQ1
             * rather than consuming it here. */
            return;
        }
        ps2mouse_handle_byte(inb(PS2_DATA));
    }
}

void ps2mouse_init(void)
{
    struct framebuffer_info info;
    u64 flags = irq_save();
    u8 config;

    /* The cursor's frame of reference. Without a framebuffer the console is
     * the 80x25 VGA text screen, which is 640x400 pixels at 8x16 per cell. */
    if (console_framebuffer_get_info(&info) && info.width && info.height) {
        screen_w = (u16)info.width;
        screen_h = (u16)info.height;
    } else {
        screen_w = 640;
        screen_h = 400;
    }
    cursor_x = (s32)screen_w / 2;
    cursor_y = (s32)screen_h / 2;

    /* Drain anything already waiting, so a stale byte is not taken for the
     * first packet header. */
    for (unsigned guard = 0; guard < 64; guard++) {
        if (!(inb(PS2_STATUS) & PS2_STATUS_OBF))
            break;
        (void)inb(PS2_DATA);
    }

    /* Enable the auxiliary port, then turn its interrupt on and its clock off
     * in the configuration byte. Read-modify-write so the keyboard's bits
     * (IRQ1 enable, translation) are preserved. */
    (void)ps2_write(PS2_CMD, PS2_CMD_ENABLE_AUX);
    if (ps2_write(PS2_CMD, PS2_CMD_READ_CONFIG) &&
        ps2_wait(PS2_STATUS_OBF, true)) {
        config = inb(PS2_DATA);
        config |= PS2_CFG_AUX_INT;
        config &= (u8)~PS2_CFG_AUX_CLOCK;
        if (ps2_write(PS2_CMD, PS2_CMD_WRITE_CONFIG))
            (void)ps2_write(PS2_DATA, config);
    }

    if (!aux_command(MOUSE_CMD_SET_DEFAULTS))
        klog(KLOG_WARN, "mouse: no auxiliary device answered\n");
    else if (!aux_command(MOUSE_CMD_ENABLE))
        klog(KLOG_WARN, "mouse: device did not start reporting\n");
    else
        klog(KLOG_INFO, "mouse: PS/2 auxiliary device reporting\n");

    idt_set_handler(VECTOR_IRQ_BASE + 12, ps2mouse_irq, IST_NONE, 0);

    /* IRQ12 is on the slave, so the cascade on the master has to be open as
     * well as the line itself. */
    outb(PIC1_DATA, (u8)(inb(PIC1_DATA) & ~(1u << 2)));
    outb(PIC2_DATA, (u8)(inb(PIC2_DATA) & ~(1u << 4)));

    irq_restore(flags);
}

bool ps2mouse_get_state(struct mouse_state *out)
{
    struct mouse_state state;
    u32 cols, rows;
    u32 cell_w = 8, cell_h = 16;

    if (!out)
        return false;

    /* One reader is enough for a coherent snapshot: the fields are written by
     * the IRQ and read here in process context. Seqlocking the struct would be
     * the rigorous answer; a torn read would only misdraw one frame of a
     * cursor, and the position fields are updated as a pair before buttons. */
    state.size = sizeof(state);
    state.x = cursor_x;
    state.y = cursor_y;
    state.buttons = buttons;
    state.present = present ? 1 : 0;
    state.reserved = 0;

    cols = console_columns();
    rows = console_rows();
    if (!cols)
        cols = 80;
    if (!rows)
        rows = 25;
    console_font_cell(&cell_w, &cell_h);
    if (!cell_w)
        cell_w = 8;
    if (!cell_h)
        cell_h = 16;

    state.pixel_width = screen_w;
    state.pixel_height = screen_h;
    state.char_width = cols;
    state.char_height = rows;
    state.char_x = (s32)(cursor_x / (s32)cell_w);
    state.char_y = (s32)(cursor_y / (s32)cell_h);
    if (state.char_x >= (s32)cols)
        state.char_x = (s32)cols - 1;
    if (state.char_y >= (s32)rows)
        state.char_y = (s32)rows - 1;

    *out = state;
    return present;
}