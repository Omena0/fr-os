/*
 * console.c — unified kernel text output across serial, framebuffer and VGA.
 *
 * The console maintains a cursor in a logical 80x25 grid and renders it to
 * every available backend. Keeping one logical grid rather than letting each
 * backend track its own cursor is what lets the serial log and the screen agree:
 * a backspace erases on screen and in the serial stream, and a clear-screen
 * sequence clears both.
 *
 * Escapes are handled here rather than in the backends so that every backend
 * sees a stream of ordinary characters plus a handful of positioning commands.
 */

#include <console.h>
#include <drivers/serial.h>
#include <kprintf.h>
#include <kstring.h>
#include <percpu.h>
#include <spinlock.h>
#include <io.h>
#include <boot.h>
#include <vmm.h>
#include <tty.h>

/* ------------------------------------------------------------ VGA text ------ */

/*
 * The VGA text buffer. The low address is physical, and the kernel reaches it
 * through vmm_boot_ptr() rather than as a pointer; see vga_mem() below.
 */
#define VGA_TEXT_BASE   0x000B8000ull
#define VGA_COLS        80
#define VGA_ROWS        25

/*
 * The VGA text buffer entry. The low byte is the character, the high byte the
 * attribute: bits 0-3 select one of eight standard colours and the background,
 * bits 4-6 select the foreground.
 */
struct vga_cell {
    uint8_t ch;
    uint8_t attr;
} __packed;

/* Hide the hardware cursor. The text console in this kernel has no readline
 * editing, so a blinking block at the end of the log is just noise. A terminal
 * emulator on the serial side provides the cursor for interactive use. */
#define VGA_CURSOR_DISABLE 0x2000

/* ------------------------------------------------------- framebuffer text --- */

/*
 * A rendered text cell.
 *
 * The framebuffer owns its pixels, so it is not limited to the VGA text
 * attribute's 8 colours. A cell carries the resolved colour source (a palette
 * index or direct RGB) and a style mask, and the renderer turns those into
 * pixels. `attr` is kept as the best-effort VGA attribute so the text-mode
 * backend and any legacy reader still see something sensible.
 */
struct fb_cell {
    char ch;
    uint8_t attr;
    uint16_t flags;
    int16_t fg_idx;     /* 0..255 palette index, -1 = fg_* is RGB */
    int16_t bg_idx;
    uint8_t fg_r, fg_g, fg_b;
    uint8_t bg_r, bg_g, bg_b;
};

struct fb_text {
    struct framebuffer_info *info;

    /* The framebuffer's physical base, resolved per use: the mapping that
     * reaches it changes when the boot page tables are replaced. */
    phys_addr_t phys;

    uint32_t pitch;
    uint32_t cols, rows;
    uint64_t scrollback;

    struct fb_cell *cells;
    const uint8_t (*font)[16];
    uint32_t font_width, font_height;
};

static struct fb_text fb;

/* Where the pixels are right now, which is not always the same address. */
static inline uint8_t *fb_pixels(void)
{
    return (uint8_t *)vmm_boot_ptr(fb.phys);
}

/* Derive rows from the framebuffer height when it is active, rather than
 * hard-coding VGA_ROWS. */
#define FB_TEXT_ROWS() (console_fb_active() ? fb.rows : VGA_ROWS)

extern const uint8_t font8x16[256][16];

/* SGR style bits, shared by the current attribute and every cell. */
#define TA_BOLD      0x0001
#define TA_DIM       0x0002
#define TA_ITALIC    0x0004
#define TA_UNDERLINE 0x0008
#define TA_BLINK     0x0010
#define TA_INVERSE   0x0020
#define TA_HIDDEN    0x0040
#define TA_STRIKE    0x0080
#define TA_OVERLINE  0x0100

/*
 * The 16 ANSI/VGA colours. Indices 0-7 are the normal set, 8-15 the bright
 * set; 90-97 selects the latter directly and bold promotes 30-37 into it.
 */
static const uint8_t ansi_palette16[16][3] = {
    { 0x00, 0x00, 0x00 },   /* 0  black */
    { 0xAA, 0x00, 0x00 },   /* 1  red */
    { 0x00, 0xAA, 0x00 },   /* 2  green */
    { 0xAA, 0x55, 0x00 },   /* 3  brown */
    { 0x00, 0x00, 0xAA },   /* 4  blue */
    { 0xAA, 0x00, 0xAA },   /* 5  magenta */
    { 0x00, 0xAA, 0xAA },   /* 6  cyan */
    { 0xAA, 0xAA, 0xAA },   /* 7  light grey */
    { 0x55, 0x55, 0x55 },   /* 8  dark grey */
    { 0xFF, 0x55, 0x55 },   /* 9  bright red */
    { 0x55, 0xFF, 0x55 },   /* 10 bright green */
    { 0xFF, 0xFF, 0x55 },   /* 11 yellow */
    { 0x55, 0x55, 0xFF },   /* 12 bright blue */
    { 0xFF, 0x55, 0xFF },   /* 13 bright magenta */
    { 0x55, 0xFF, 0xFF },   /* 14 bright cyan */
    { 0xFF, 0xFF, 0xFF },   /* 15 white */
};

/* The attribute a program is drawing with. Palette index -1 means the RGB
 * fields hold the exact colour (256-colour or truecolour SGR). */
struct term_attr {
    int16_t fg_idx, bg_idx;
    uint8_t fg_r, fg_g, fg_b;
    uint8_t bg_r, bg_g, bg_b;
    uint16_t flags;
};

static struct term_attr cur = {
    .fg_idx = 7, .bg_idx = 0, .flags = 0,
};

static uint8_t current_attr = TERM_ATTR_NORMAL;

/* Blink phase, toggled from the timer tick. True = foreground visible. */
static bool blink_on = true;

/* xterm's 256-colour palette. */
static void ansi256_to_rgb(uint8_t n, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (n < 16) {
        *r = ansi_palette16[n][0];
        *g = ansi_palette16[n][1];
        *b = ansi_palette16[n][2];
        return;
    }
    if (n < 232) {
        static const uint8_t ramp[6] = { 0, 95, 135, 175, 215, 255 };
        uint8_t i = (uint8_t)(n - 16);
        *r = ramp[i / 36];
        *g = ramp[(i / 6) % 6];
        *b = ramp[i % 6];
        return;
    }
    *r = *g = *b = (uint8_t)(8 + (n - 232) * 10);
}

/* Resolve a stored colour source to RGB. */
static void term_color(int16_t idx, uint8_t r, uint8_t g, uint8_t b,
                       uint8_t *or_, uint8_t *og, uint8_t *ob)
{
    if (idx >= 0 && idx < 256) {
        ansi256_to_rgb((uint8_t)idx, or_, og, ob);
    } else {
        *or_ = r;
        *og = g;
        *ob = b;
    }
}

/* Compute the VGA attribute that best approximates the current attribute.
 * VGA has 8 foreground colours plus an intensity bit and 8 background colours;
 * bright backgrounds and truecolour have no exact representation. */
static uint8_t term_vga_attr(void)
{
    uint8_t fr, fg, fb, br, bg, bb;
    unsigned fi = 7, bi = 0;
    uint32_t fbest = 0xFFFFFFFFu, bbest = 0xFFFFFFFFu;

    term_color(cur.fg_idx, cur.fg_r, cur.fg_g, cur.fg_b, &fr, &fg, &fb);
    term_color(cur.bg_idx, cur.bg_r, cur.bg_g, cur.bg_b, &br, &bg, &bb);

    for (unsigned i = 0; i < 16; i++) {
        int dr = (int)fr - ansi_palette16[i][0];
        int dg = (int)fg - ansi_palette16[i][1];
        int db = (int)fb - ansi_palette16[i][2];
        uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
        if (d < fbest) { fbest = d; fi = i; }
    }
    for (unsigned i = 0; i < 8; i++) {
        int dr = (int)br - ansi_palette16[i][0];
        int dg = (int)bg - ansi_palette16[i][1];
        int db = (int)bb - ansi_palette16[i][2];
        uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
        if (d < bbest) { bbest = d; bi = i; }
    }

    return (uint8_t)((fi & 0x07) | ((fi >= 8) ? 0x08 : 0) | (bi << 4));
}

/*
 * Resolve a colour pair plus style mask into the two RGB values actually
 * painted. Bold uses the bright half of the palette for indexed colours and
 * lightens truecolour; dim halves it; inverse swaps the pair and hidden
 * paints the foreground in the background colour.
 */
static void term_resolve(int16_t fg_idx, const uint8_t *fg_rgb,
                         int16_t bg_idx, const uint8_t *bg_rgb,
                         uint16_t flags,
                         uint8_t *fr, uint8_t *fg, uint8_t *fb,
                         uint8_t *br, uint8_t *bg, uint8_t *bb)
{
    term_color(fg_idx, fg_rgb[0], fg_rgb[1], fg_rgb[2], fr, fg, fb);
    term_color(bg_idx, bg_rgb[0], bg_rgb[1], bg_rgb[2], br, bg, bb);

    if ((flags & TA_BOLD) && fg_idx >= 0 && fg_idx < 8) {
        *fr = ansi_palette16[fg_idx + 8][0];
        *fg = ansi_palette16[fg_idx + 8][1];
        *fb = ansi_palette16[fg_idx + 8][2];
    } else if (flags & TA_BOLD) {
        *fr = (uint8_t)(*fr + (255 - *fr) / 3);
        *fg = (uint8_t)(*fg + (255 - *fg) / 3);
        *fb = (uint8_t)(*fb + (255 - *fb) / 3);
    }
    if (flags & TA_DIM) {
        *fr = (uint8_t)(*fr / 2);
        *fg = (uint8_t)(*fg / 2);
        *fb = (uint8_t)(*fb / 2);
    }
    if (flags & TA_INVERSE) {
        uint8_t t0 = *fr, t1 = *fg, t2 = *fb;
        *fr = *br; *fg = *bg; *fb = *bb;
        *br = t0; *bg = t1; *bb = t2;
    }
    if (flags & TA_HIDDEN) {
        *fr = *br; *fg = *bg; *fb = *bb;
    }
    if ((flags & TA_BLINK) && !blink_on) {
        *fr = *br; *fg = *bg; *fb = *bb;
    }
}

/* Resolve a stored cell. */
static void fb_cell_colors(const struct fb_cell *cell,
                           uint8_t *fr, uint8_t *fg, uint8_t *fb,
                           uint8_t *br, uint8_t *bg, uint8_t *bb)
{
    uint8_t fgrgb[3] = { cell->fg_r, cell->fg_g, cell->fg_b };
    uint8_t bgrgb[3] = { cell->bg_r, cell->bg_g, cell->bg_b };

    term_resolve(cell->fg_idx, fgrgb, cell->bg_idx, bgrgb, cell->flags,
                 fr, fg, fb, br, bg, bb);
}

/* Copy the current attribute into a cell. */
static void fb_cell_set_attr(struct fb_cell *cell)
{
    cell->attr = current_attr;
    cell->flags = cur.flags;
    cell->fg_idx = cur.fg_idx;
    cell->bg_idx = cur.bg_idx;
    cell->fg_r = cur.fg_r; cell->fg_g = cur.fg_g; cell->fg_b = cur.fg_b;
    cell->bg_r = cur.bg_r; cell->bg_g = cur.bg_g; cell->bg_b = cur.bg_b;
}

/* ------------------------------------------------------------ state -------- */

static spinlock_t console_lock;
static uint32_t cursor_x, cursor_y;
static uint32_t saved_cursor_x, saved_cursor_y;
static uint64_t scrollback;
static bool vga_enabled;
static bool serial_enabled = true;

/*
 * Escape state. A minimal CSI parser is enough for the sequences a console
 * driver needs: cursor positioning and erase. Anything unknown is ignored
 * rather than printed, so a stray escape from a log message does not dump its
 * parameters onto the screen.
 */
enum esc_state {
    ESC_NONE,
    ESC_SEEN,        /* saw ESC */
    ESC_CSI,         /* saw ESC [ , collecting parameters */
};

static enum esc_state esc_state;
static char esc_params[64];
/* CSI '?' prefix: a private-mode sequence such as ?25l. The console has no
 * mode to set, but it must consume the whole sequence instead of treating
 * '?' as the final byte and printing the rest as text. */
static bool esc_private;
static uint8_t esc_param_count;

static void console_fb_init(struct framebuffer_info *info);
static void console_fb_putc(char c, uint8_t attr);
static void console_fb_scroll(void);
static void console_fb_clear(void);
static void console_fb_move(uint32_t x, uint32_t y);
static bool console_fb_active(void);

/* Cursor save/restore (DECSC/DECRC), used by tests that probe absolute cursor
 * moves. Without it a sequence like ESC[1;1H leaves the serial terminal's
 * cursor at the top of the screen, so the next prompt is drawn over earlier
 * output even though the framebuffer console tracked its own cursor. */
static void console_save_cursor(void)
{
    saved_cursor_x = cursor_x;
    saved_cursor_y = cursor_y;
}

static void console_restore_cursor(void)
{
    uint32_t rows = FB_TEXT_ROWS();

    cursor_x = saved_cursor_x;
    cursor_y = saved_cursor_y;
    /* The saved position was valid when it was taken; it can be out of range
     * only if the display changed size in between. Clamp against the display
     * that is active now, not VGA's 25 rows -- the framebuffer shows far more. */
    if (cursor_x >= VGA_COLS)
        cursor_x = VGA_COLS - 1;
    if (cursor_y >= rows)
        cursor_y = rows - 1;
    if (console_fb_active())
        console_fb_move(cursor_x, cursor_y);
}

/* ---------------------------------------------------------------- spin ----- */

/*
 * Acquire and release the console.
 *
 * Interrupts are disabled for the duration because callers include interrupt
 * handlers and panic paths: if an interrupt landed while this CPU held the
 * lock, and the handler also tried to print, the result is a self-deadlock.
 *
 * The saved flag word lives in the caller's frame, not a shared variable: two
 * CPUs can be inside the console at once, and each must restore its own IF
 * state. A shared variable would be correct on UP and wrong on SMP the moment
 * two CPUs overlapped.
 */
static u64 console_acquire(void)
{
    return spinlock_irqsave(&console_lock);
}

static void console_release(u64 flags)
{
    spinlock_unlock_irqrestore(&console_lock, flags);
}

/* ------------------------------------------------------------ rendering ---- */

static inline volatile struct vga_cell *vga_mem(void)
{
    return (volatile struct vga_cell *)vmm_boot_ptr(VGA_TEXT_BASE);
}

static void vga_scroll(void)
{
    volatile struct vga_cell *mem = vga_mem();
    volatile uint8_t *top = (volatile uint8_t *)&mem[0];
    volatile uint8_t *bottom = (volatile uint8_t *)
        &mem[(VGA_ROWS - 1) * VGA_COLS];

    for (volatile uint8_t *p = top; p < bottom; p++)
        *p = *(p + VGA_COLS * 2);

    for (int i = 0; i < VGA_COLS; i++) {
        mem[(VGA_ROWS - 1) * VGA_COLS + i].ch = ' ';
        mem[(VGA_ROWS - 1) * VGA_COLS + i].attr = current_attr;
    }
}

static void vga_clear(void)
{
    volatile struct vga_cell *mem = vga_mem();

    for (int i = 0; i < VGA_ROWS * VGA_COLS; i++) {
        mem[i].ch = ' ';
        mem[i].attr = current_attr;
    }
}

static inline void vga_putc_at(uint32_t x, uint32_t y, char c, uint8_t attr)
{
    volatile struct vga_cell *mem = vga_mem();
    uint32_t i = y * VGA_COLS + x;

    mem[i].ch = (uint8_t)c;
    mem[i].attr = attr;
}

/* ------------------------------------------------------------- backends ---- */

static bool console_fb_active(void)
{
    return fb.phys != 0;
}

bool console_framebuffer_get_info(struct framebuffer_info *info)
{
    u64 flags;

    if (!info)
        return false;
    flags = console_acquire();
    if (!console_fb_active() || !fb.info) {
        console_release(flags);
        return false;
    }
    *info = *fb.info;
    console_release(flags);
    return true;
}

/* Write a chunk from the tightly packed userspace image into the possibly
 * padded hardware rows. Callers copy and validate user data before entering. */
long console_framebuffer_write(uint64_t offset, const void *src, size_t len)
{
    struct framebuffer_info *info;
    const uint8_t *in = src;
    uint64_t total;
    u64 flags;

    if (!src && len)
        return -EINVAL;
    flags = console_acquire();
    if (!console_fb_active() || !fb.info || fb.info->bpp != 32) {
        console_release(flags);
        return -ENODEV;
    }
    info = fb.info;
    total = (uint64_t)info->width * info->height * 4;
    if (offset > total || len > total - offset) {
        console_release(flags);
        return -EINVAL;
    }
    while (len) {
        uint32_t y = (uint32_t)(offset / ((uint64_t)info->width * 4));
        uint32_t xbyte = (uint32_t)(offset % ((uint64_t)info->width * 4));
        size_t chunk = (size_t)info->width * 4 - xbyte;
        if (chunk > len)
            chunk = len;
        memcpy(fb_pixels() + (uint64_t)y * info->pitch + xbyte, in, chunk);
        in += chunk;
        offset += chunk;
        len -= chunk;
    }
    console_release(flags);
    return 0;
}

/*
 * Render one glyph into the framebuffer.
 *
 * For 32bpp the pixel is assembled by shifting the channel components into
 * their reported positions, which is why bootinfo carries byte offsets rather
 * than a fixed ARGB layout: VBE implementations are free to choose, and a
 * framebuffer in BGR order is common enough that hard-coding would be wrong on
 * some machines and needlessly wrong on others.
 *
 * The glyph is 8x16, one byte per row, MSB leftmost, which is the layout of
 * both the VGA font ROM and the PSF format the font was converted from.
 */
static void fb_render_glyph(uint32_t col, uint32_t row, char c,
                uint8_t fg_r, uint8_t fg_g, uint8_t fg_b,
                uint8_t bg_r, uint8_t bg_g, uint8_t bg_b,
                uint16_t flags)
{
    uint32_t px = col * fb.font_width;
    uint32_t py = row * fb.font_height;
    const uint8_t *glyph;
    uint8_t *pixels;

    if (px + fb.font_width > fb.info->width ||
        py + fb.font_height > fb.info->height)
        return;

    /* Resolved once per glyph rather than per pixel: the address is a
     * function of which map is live, so it cannot be hoisted out of the
     * function, but it does not change between the pixels of one glyph. */
    pixels = fb_pixels();
    glyph = fb.font[(uint8_t)c];

    for (uint32_t gy = 0; gy < fb.font_height; gy++) {
        uint8_t bits = glyph[gy];

        /* Italic: lean the glyph right, more at the top. */
        if (flags & TA_ITALIC)
            bits = (uint8_t)(bits >> ((fb.font_height - 1 - gy) / 6));

        /* Rules span the whole cell, not just the glyph's inked columns. */
        bool rule = ((flags & TA_UNDERLINE) && gy == fb.font_height - 1) ||
                    ((flags & TA_STRIKE) && gy == fb.font_height / 2) ||
                    ((flags & TA_OVERLINE) && gy == 0);

        for (uint32_t gx = 0; gx < fb.font_width; gx++) {
            bool on = rule || ((bits & (0x80 >> gx)) != 0);
            uint32_t x = px + gx;
            uint32_t y = py + gy;
            uint8_t r = on ? fg_r : bg_r;
            uint8_t g = on ? fg_g : bg_g;
            uint8_t b = on ? fg_b : bg_b;
            uint32_t value;
            uint8_t *dst;

            if (fb.info->bpp == 32) {
                value = ((uint32_t)r << fb.info->red_shift) |
                    ((uint32_t)g << fb.info->green_shift) |
                    ((uint32_t)b << fb.info->blue_shift);
            } else {
                /* 24bpp: three bytes per pixel, no padding. */
                value = ((uint32_t)r << 16) |
                    ((uint32_t)g << 8) | b;
            }

            dst = pixels + y * fb.pitch + x * (fb.info->bpp / 8);
            if (fb.info->bpp == 32) {
                *(uint32_t *)dst = value;
            } else {
                dst[0] = (uint8_t)(value & 0xFF);
                dst[1] = (uint8_t)((value >> 8) & 0xFF);
                dst[2] = (uint8_t)((value >> 16) & 0xFF);
            }
        }
    }
}

static void console_fb_init(struct framebuffer_info *info)
{
    uint32_t cell_bytes;

    if (!info || info->address == 0 || info->bpp == 0)
        return;

    fb.info = info;
    fb.phys = info->address;
    fb.pitch = info->pitch;
    fb.font = font8x16;
    fb.font_width = 8;
    fb.font_height = 16;

    /*
     * Derive the grid from the reported pitch rather than assuming 32 pixels
     * of padding. Some BIOSes report a pitch wider than width * bpp/8; using
     * the pitch is what makes text land in the right place on those.
     */
    fb.cols = info->width / fb.font_width;
    fb.rows = info->height / fb.font_height;
    if (fb.cols == 0 || fb.rows == 0)
        return;
    if (fb.rows > 200)
        fb.rows = 200;

    cell_bytes = fb.cols * fb.rows * sizeof(struct fb_cell);
    fb.cells = (struct fb_cell *)kmalloc(cell_bytes);
    if (!fb.cells) {
        /* Without the cell buffer there is no scrollback or repaint, but
         * the framebuffer itself still works for a fixed grid. Fall back
         * to clearing and drawing in place. */
        fb.cols = 0;
        fb.rows = 0;
    }

    current_attr = term_vga_attr();
    for (uint32_t i = 0; i < fb.cols * fb.rows; i++) {
        fb.cells[i].ch = ' ';
        fb_cell_set_attr(&fb.cells[i]);
    }

    /* Paint the whole background once so no garbage is visible. */
    {
        uint8_t fr, fg, fb_, br, bg, bb;
        uint8_t fgrgb[3] = { cur.fg_r, cur.fg_g, cur.fg_b };
        uint8_t bgrgb[3] = { cur.bg_r, cur.bg_g, cur.bg_b };
        term_resolve(cur.fg_idx, fgrgb, cur.bg_idx, bgrgb, cur.flags,
                     &fr, &fg, &fb_, &br, &bg, &bb);
        for (uint32_t r = 0; r < fb.rows; r++)
            for (uint32_t c = 0; c < fb.cols; c++)
                fb_render_glyph(c, r, ' ', fr, fg, fb_, br, bg, bb, 0);
    }
}

static void console_fb_scroll(void)
{
    if (!fb.cells || fb.cols == 0)
        return;

    memmove(&fb.cells[0], &fb.cells[fb.cols],
        (fb.rows - 1) * fb.cols * sizeof(struct fb_cell));
    for (uint32_t c = 0; c < fb.cols; c++) {
        struct fb_cell *cell = &fb.cells[(fb.rows - 1) * fb.cols + c];
        cell->ch = ' ';
        fb_cell_set_attr(cell);
    }

    /* Repaint entire screen since every row shifted up. */
    for (uint32_t r = 0; r < fb.rows; r++) {
        for (uint32_t c = 0; c < fb.cols; c++) {
            struct fb_cell *cell = &fb.cells[r * fb.cols + c];
            uint8_t fr, fg, fb_, br, bg, bb;

            fb_cell_colors(cell, &fr, &fg, &fb_, &br, &bg, &bb);
            fb_render_glyph(c, r, cell->ch, fr, fg, fb_, br, bg, bb,
                            cell->flags);
        }
    }
    fb.scrollback++;
    if (cursor_y >= fb.rows)
        cursor_y = fb.rows - 1;
}

static void console_fb_clear(void)
{
    if (!fb.cells || fb.cols == 0)
        return;

    for (uint32_t i = 0; i < fb.cols * fb.rows; i++) {
        fb.cells[i].ch = ' ';
        fb_cell_set_attr(&fb.cells[i]);
    }
    {
        uint8_t fr, fg, fb_, br, bg, bb;
        uint8_t fgrgb[3] = { cur.fg_r, cur.fg_g, cur.fg_b };
        uint8_t bgrgb[3] = { cur.bg_r, cur.bg_g, cur.bg_b };
        term_resolve(cur.fg_idx, fgrgb, cur.bg_idx, bgrgb, cur.flags,
                     &fr, &fg, &fb_, &br, &bg, &bb);
        for (uint32_t r = 0; r < fb.rows; r++)
            for (uint32_t c = 0; c < fb.cols; c++)
                fb_render_glyph(c, r, ' ', fr, fg, fb_, br, bg, bb, 0);
    }
}

static void console_fb_move(uint32_t x, uint32_t y)
{
    cursor_x = x;
    cursor_y = y;
}

static void console_fb_putc(char c, uint8_t attr)
{
    uint32_t x = cursor_x;
    uint32_t y = cursor_y;

    uint8_t fgrgb[3] = { cur.fg_r, cur.fg_g, cur.fg_b };
    uint8_t bgrgb[3] = { cur.bg_r, cur.bg_g, cur.bg_b };
    uint8_t fg_r, fg_g, fg_b, bg_r, bg_g, bg_b;

    term_resolve(cur.fg_idx, fgrgb, cur.bg_idx, bgrgb, cur.flags,
                 &fg_r, &fg_g, &fg_b, &bg_r, &bg_g, &bg_b);

    if (fb.cells && fb.cols) {
        struct fb_cell *cell = &fb.cells[y * fb.cols + x];

        cell->ch = c;
        fb_cell_set_attr(cell);
        fb_render_glyph(x, y, c, fg_r, fg_g, fg_b, bg_r, bg_g, bg_b,
                        cur.flags);
        return;
    }

    /* No cell buffer: draw directly. */
    fb_render_glyph(x, y, c, fg_r, fg_g, fg_b, bg_r, bg_g, bg_b, cur.flags);
}

/* ------------------------------------------------------------- cursor ------ */

static void console_newline(void)
{
    cursor_x = 0;
    cursor_y++;

    uint32_t rows = FB_TEXT_ROWS();

    if (cursor_y >= rows) {
        if (!tty_screen_owned()) {
            if (vga_enabled)
                vga_scroll();
            if (console_fb_active())
                console_fb_scroll();
        }
        cursor_y = rows - 1;
    }
}

/* ------------------------------------------------------------- escapes ----- */

/*
 * Apply one SGR (Select Graphic Rendition) parameter string to the current
 * attribute. The string is the raw parameter bytes collected by the escape
 * parser, terminated with NUL and with ';' separating numbers.
 *
 * Only the codes the console uses are handled; an unknown code is a no-op
 * rather than an error, so a sequence like "\033[0;37m" leaves the unknown
 * 37 alone and resets the attribute.
 */
void console_apply_sgr(const char *params)
{
    int v[32];
    int n = 0;
    const char *p = params;

    /* Collect the numeric parameters first: 38/48 consume one or four more,
     * so the loop below needs to look ahead. */
    while (*p && n < (int)(sizeof(v) / sizeof(v[0]))) {
        unsigned long x = 0;
        while (*p == ';')
            p++;
        if (*p < '0' || *p > '9') {
            if (!*p)
                break;
            /* A non-numeric parameter is not valid SGR; skip it. */
            while (*p && *p != ';')
                p++;
            continue;
        }
        kstrtoul(p, &x, 10);
        v[n++] = (int)x;
        while (*p && *p != ';')
            p++;
    }
    if (n == 0)
        v[n++] = 0;             /* ESC[m == reset */

    for (int i = 0; i < n; i++) {
        int code = v[i];

        switch (code) {
        case 0:                 /* reset */
            cur.fg_idx = 7; cur.bg_idx = 0; cur.flags = 0;
            cur.fg_r = cur.fg_g = cur.fg_b = 0;
            cur.bg_r = cur.bg_g = cur.bg_b = 0;
            break;
        case 1:  cur.flags |= TA_BOLD; break;
        case 2:  cur.flags |= TA_DIM; break;
        case 3:  cur.flags |= TA_ITALIC; break;
        case 4:  cur.flags |= TA_UNDERLINE; break;
        case 5:
        case 6:  cur.flags |= TA_BLINK; break;
        case 7:  cur.flags |= TA_INVERSE; break;
        case 8:  cur.flags |= TA_HIDDEN; break;
        case 9:  cur.flags |= TA_STRIKE; break;
        case 21:
        case 22: cur.flags &= (uint16_t)~(TA_BOLD | TA_DIM); break;
        case 23: cur.flags &= (uint16_t)~TA_ITALIC; break;
        case 24: cur.flags &= (uint16_t)~TA_UNDERLINE; break;
        case 25: cur.flags &= (uint16_t)~TA_BLINK; break;
        case 27: cur.flags &= (uint16_t)~TA_INVERSE; break;
        case 28: cur.flags &= (uint16_t)~TA_HIDDEN; break;
        case 29: cur.flags &= (uint16_t)~TA_STRIKE; break;
        case 53: cur.flags |= TA_OVERLINE; break;
        case 55: cur.flags &= (uint16_t)~TA_OVERLINE; break;

        case 30: case 31: case 32: case 33:
        case 34: case 35: case 36: case 37:
            cur.fg_idx = (int16_t)(code - 30);
            break;
        case 39:                /* default foreground */
            cur.fg_idx = 7;
            break;
        case 90: case 91: case 92: case 93:
        case 94: case 95: case 96: case 97:
            cur.fg_idx = (int16_t)(code - 90 + 8);
            break;

        case 40: case 41: case 42: case 43:
        case 44: case 45: case 46: case 47:
            cur.bg_idx = (int16_t)(code - 40);
            break;
        case 49:                /* default background */
            cur.bg_idx = 0;
            break;
        case 100: case 101: case 102: case 103:
        case 104: case 105: case 106: case 107:
            cur.bg_idx = (int16_t)(code - 100 + 8);
            break;

        case 38:                /* extended foreground colour */
        case 48: {              /* extended background colour */
            bool fg = (code == 38);
            if (i + 2 < n && v[i + 1] == 5) {
                if (fg) cur.fg_idx = (int16_t)(v[i + 2] & 0xFF);
                else    cur.bg_idx = (int16_t)(v[i + 2] & 0xFF);
                i += 2;
            } else if (i + 4 < n && v[i + 1] == 2) {
                uint8_t r = (uint8_t)v[i + 2];
                uint8_t g = (uint8_t)v[i + 3];
                uint8_t b = (uint8_t)v[i + 4];
                if (fg) {
                    cur.fg_idx = -1;
                    cur.fg_r = r; cur.fg_g = g; cur.fg_b = b;
                } else {
                    cur.bg_idx = -1;
                    cur.bg_r = r; cur.bg_g = g; cur.bg_b = b;
                }
                i += 4;
            }
            break;
        }
        default:
            break;
        }
    }

    /* Keep the VGA text-mode attribute in step with the rich one. */
    current_attr = term_vga_attr();
}

/*
 * Feed one character to the escape parser.
 *
 * Returns true if the character was part of an escape sequence and must not
 * also be printed. That distinction has to live in the return value: a CSI is
 * delivered to this function one byte at a time, and every byte of it has
 * already been handed to console_putc_attr by a caller that only looks at the
 * state, which prints a literal "[2J" for console_clear().
 */
static bool handle_escape(char c)
{
    switch (esc_state) {
    case ESC_NONE:
        if (c == 0x1B) {
            esc_state = ESC_SEEN;
            if (serial_enabled)
                serial_putc_blocking(c);
            return true;
        }
        return false;

    case ESC_SEEN:
        if (c == '[') {
            esc_state = ESC_CSI;
            esc_param_count = 0;
            esc_private = false;
            if (serial_enabled)
                serial_putc_blocking(c);
            return true;
        }
        /* DECSC/DECRC in their two-byte form. */
        if (c == '7') {
            esc_state = ESC_NONE;
            console_save_cursor();
            if (serial_enabled)
                serial_putc_blocking(c);
            return true;
        }
        if (c == '8') {
            esc_state = ESC_NONE;
            console_restore_cursor();
            if (serial_enabled)
                serial_putc_blocking(c);
            return true;
        }
        /* ESC followed by anything else is a two-byte sequence we do not
         * implement. Drop it rather than printing the second byte. */
        esc_state = ESC_NONE;
        if (serial_enabled)
            serial_putc_blocking(c);
        return true;

    case ESC_CSI:
        if (c == '?' && esc_param_count == 0) {
            esc_private = true;
            if (serial_enabled)
                serial_putc_blocking(c);
            return true;
        }
        if (c >= '0' && c <= '9') {
            if (esc_param_count < sizeof(esc_params))
                esc_params[esc_param_count] = c;
            esc_param_count++;
            if (serial_enabled)
                serial_putc_blocking(c);
            return true;
        }
        if (c == ';') {
            /* Keep the separator as ';'. It used to be stored as NUL, which
             * made the parameter string end at the first separator: every
             * multi-parameter SGR (1;31, 38;2;r;g;b, ...) was parsed as just
             * its first number. */
            if (esc_param_count < sizeof(esc_params))
                esc_params[esc_param_count] = ';';
            esc_param_count++;
            if (serial_enabled)
                serial_putc_blocking(c);
            return true;
        }

        /* Sequence complete: parse the command */
        esc_state = ESC_NONE;
        esc_params[esc_param_count < sizeof(esc_params) ?
                esc_param_count : 0] = '\0';

        unsigned long n_param = 0;
        kstrtoul(esc_params, &n_param, 10);
        uint32_t n = (uint32_t)n_param;

        /* Mirror the final byte to serial before processing. */
        if (serial_enabled)
            serial_putc_blocking(c);

        /* Private modes (?25l hide cursor, ?25h show it, ...) have no
         * framebuffer equivalent to set; consume and ignore them. */
        if (esc_private) {
            esc_private = false;
            return true;
        }

        switch (c) {
            case 'H':       /* cursor position, 1-based */
                {
                uint32_t row = n ? n - 1 : 0;
                uint32_t col = 0;

                /* A second parameter is the column; only single-parameter
                 * forms appear in kernel output today. */
                col = 0;
                cursor_y = row;
                cursor_x = col;
                if (vga_enabled) {
                    if (cursor_y >= VGA_ROWS)
                        cursor_y = VGA_ROWS - 1;
                    vga_putc_at(cursor_x, cursor_y, ' ', current_attr);
                }
                if (console_fb_active())
                    console_fb_move(cursor_x, cursor_y);
                return true;
                }
            case 'J':       /* erase display */
                if (vga_enabled)
                    vga_clear();
                if (console_fb_active())
                    console_fb_clear();
                cursor_x = 0;
                cursor_y = 0;
                return true;
            case 's':       /* save cursor (SCP) */
                console_save_cursor();
                return true;
            case 'u':       /* restore cursor (RCP) */
                console_restore_cursor();
                return true;
            case 'K':       /* erase line, from the cursor to the end */
                {
                uint32_t saved = cursor_x;
                uint32_t cols = console_fb_active() ? fb.cols : VGA_COLS;

                for (uint32_t x = cursor_x; x < cols; x++) {
                    if (vga_enabled)
                        vga_putc_at(x, cursor_y, ' ', current_attr);
                    if (console_fb_active()) {
                        cursor_x = x;
                        console_fb_putc(' ', current_attr);
                    }
                }
                cursor_x = saved;
                return true;
                }
            case 'm':       /* SGR: set attributes */
                console_apply_sgr(esc_params);
                return true;
            default:
                /* An unknown final byte still ends a sequence, so it is
                 * consumed rather than printed. */
                return true;
        }
    }

    /* Not reachable: every branch above returns. Present so the function has
     * a return on every path the compiler can see. */
    return false;
}

/* ------------------------------------------------------------- public ------ */

/*
 * Toggle the blink phase from the timer tick and repaint only the cells that
 * asked for it. Called in interrupt context, so it takes the console lock
 * with trylock: if the interrupted code is mid-write the phase change is
 * simply deferred to the next tick rather than deadlocking against a lock
 * this CPU already holds.
 */
void console_blink_tick(u64 now_ns)
{
    bool on = ((now_ns / 500000000ull) & 1ull) == 0;

    if (on == blink_on)
        return;
    if (!spinlock_trylock(&console_lock))
        return;

    blink_on = on;
    if (fb.cells && fb.cols) {
        for (uint32_t r = 0; r < fb.rows; r++) {
            for (uint32_t c = 0; c < fb.cols; c++) {
                struct fb_cell *cell = &fb.cells[r * fb.cols + c];
                uint8_t fr, fg, fb_, br, bg, bb;

                if (!(cell->flags & TA_BLINK))
                    continue;
                fb_cell_colors(cell, &fr, &fg, &fb_, &br, &bg, &bb);
                fb_render_glyph(c, r, cell->ch, fr, fg, fb_, br, bg, bb,
                                cell->flags);
            }
        }
    }

    spinlock_release(&console_lock);
}

void console_putc_attr(char c, uint8_t attr)
{
    if (c == '\n') {
        console_newline();
        /*
         * The screen backends get their line break from the cursor
         * movement above, but the serial port only ever sees the bytes
         * that are written to it. Consuming the newline without sending
         * it is what made the entire kernel log arrive as one
         * unbroken line, with the next message appended to the
         * previous one.
         */
        if (serial_enabled) {
            /* A host terminal attached to QEMU's stdio chardev is in raw
             * mode, so LF alone moves down without returning to column 0.
             * Every terminal accepts CRLF whether or not the host is raw,
             * so send the pair. */
            serial_putc_blocking('\r');
            serial_putc_blocking('\n');
        }
        return;
    }
    if (c == '\r') {
        cursor_x = 0;
        if (console_fb_active())
            console_fb_move(0, cursor_y);
        if (serial_enabled)
            serial_putc_blocking('\r');
        return;
    }
    if (c == '\t') {
        /* Tab stops every 8 columns. */
        do {
            console_putc_attr(' ', attr);
        } while (cursor_x % 8 != 0);
        return;
    }
    if (c == '\b') {
        if (cursor_x > 0)
            cursor_x--;
        if (!tty_screen_owned() && console_fb_active())
            console_fb_move(cursor_x, cursor_y);
        if (serial_enabled)
            serial_putc_blocking('\b');
        return;
    }

    if (!tty_screen_owned()) {
        if (vga_enabled)
            vga_putc_at(cursor_x, cursor_y, c, attr);
        if (console_fb_active())
            console_fb_putc(c, attr);
    }

    if (serial_enabled)
        serial_putc_blocking(c);

    cursor_x++;
    uint32_t cols = vga_enabled ? VGA_COLS : fb.cols;
    if (cols && cursor_x >= cols)
        console_newline();
}

void console_putc(char c)
{
    console_putc_attr(c, current_attr);
}

void console_write(const char *s)
{
    u64 flags = console_acquire();

    for (; *s; s++) {
        /* The escape parser owns every byte it recognises. Printing
         * them as well is how console_clear() ends up writing a
         * literal "[2J" onto the screen it just cleared. */
        if (handle_escape(*s))
            continue;
        console_putc_attr(*s, current_attr);
    }

    console_release(flags);
}

void console_write_raw(const char *s, size_t n)
{
    u64 flags = console_acquire();

    for (size_t i = 0; i < n; i++) {
        char c = s[i];

        if (c == '\n') {
            if (serial_enabled) {
                serial_putc_blocking('\r');
                serial_putc_blocking('\n');
            }
            console_newline();
            continue;
        }
        if (c == '\r') {
            cursor_x = 0;
            if (!tty_screen_owned() && console_fb_active())
                console_fb_move(0, cursor_y);
            if (serial_enabled)
                serial_putc_blocking('\r');
            continue;
        }
        if (c == '\t') {
            /* Tab stops every 8 columns. */
            do {
                if (!tty_screen_owned() && vga_enabled)
                    vga_putc_at(cursor_x, cursor_y, ' ', current_attr);
                if (!tty_screen_owned() && console_fb_active())
                    console_fb_putc(' ', current_attr);
                cursor_x++;
                if (vga_enabled && cursor_x >= VGA_COLS)
                    cursor_x = 0;
                else if (console_fb_active() && cursor_x >= fb.cols)
                    cursor_x = 0;
            } while (cursor_x % 8 != 0);
            if (serial_enabled)
                serial_putc_blocking('\t');
            continue;
        }
        if (c == '\b') {
            if (cursor_x > 0)
                cursor_x--;
            if (!tty_screen_owned() && console_fb_active())
                console_fb_move(cursor_x, cursor_y);
            if (serial_enabled)
                serial_putc_blocking('\b');
            continue;
        }

        /*
         * A process's bytes are still sent to the serial port verbatim (the
         * escape parser mirrors what it consumes), but the screen has to
         * interpret them: drawing ESC[38;2;...m literally is what made the
         * ANSI test fill the framebuffer with the sequence text instead of
         * colours. Only the screen is affected; the parser never routes user
         * bytes through kprintf. */
        if (handle_escape(c))
            continue;

        if (!tty_screen_owned()) {
            if (vga_enabled)
                vga_putc_at(cursor_x, cursor_y, c, current_attr);
            if (console_fb_active())
                console_fb_putc(c, current_attr);
        }

        if (serial_enabled)
            serial_putc_blocking(c);

        cursor_x++;
        if (vga_enabled && cursor_x >= VGA_COLS)
            console_newline();
        else if (console_fb_active() && cursor_x >= fb.cols)
            console_newline();
    }

    console_release(flags);
}

void kprintf(const char *fmt, ...)
{
    __builtin_va_list ap;

    __builtin_va_start(ap, fmt);
    kvprintf_to_console(fmt, ap);
    __builtin_va_end(ap);
}

/*
 * Format and hand the finished text to the console.
 *
 * The conversion happens into a stack buffer first and console_write() takes
 * the lock only for the write of that finished text, so the lock is never held
 * across a conversion and two CPUs cannot interleave mid-line. The cost is a
 * bounded one: a line longer than the buffer is truncated at the buffer size
 * rather than overflowing, which is visible in the log instead of fatal.
 */
void kvprintf_to_console(const char *fmt, __builtin_va_list ap)
{
    char stack_buf[512];

    kvsnprintf(stack_buf, sizeof(stack_buf), fmt, ap);
    console_write(stack_buf);
}

/* Derive the rich attribute from a legacy VGA attribute. */
static void term_attr_from_vga(uint8_t attr)
{
    unsigned fg = attr & 0x07;
    unsigned bg = (attr >> 4) & 0x07;

    cur.fg_idx = (int16_t)((attr & 0x08) ? fg + 8 : fg);
    cur.bg_idx = (int16_t)bg;
    cur.flags = 0;
    cur.fg_r = cur.fg_g = cur.fg_b = 0;
    cur.bg_r = cur.bg_g = cur.bg_b = 0;
}

void console_set_attr(uint8_t attr)
{
    current_attr = attr;
    term_attr_from_vga(attr);
}

void console_reset_attr(void)
{
    current_attr = TERM_ATTR_NORMAL;
    term_attr_from_vga(TERM_ATTR_NORMAL);
}

uint8_t console_get_attr(void)
{
    return current_attr;
}

void console_clear(void)
{
    console_write(TERM_ESC_CLEAR TERM_ESC_HOME);
}

uint32_t console_columns(void)
{
    if (console_fb_active())
        return fb.cols;
    return VGA_COLS;
}

uint32_t console_rows(void)
{
    if (console_fb_active())
        return fb.rows;
    return VGA_ROWS;
}

uint64_t console_scrollback(void)
{
    return scrollback;
}

/*
 * Bring up every available backend.
 *
 * `bootinfo` may be NULL, in which case the VGA text buffer is the only
 * backend and is used without any mode having been set. That path matters: it
 * is what makes the kernel produce visible output on a machine where stage2
 * could not find a linear framebuffer.
 */
void console_init(struct bootinfo *bi)
{
    static char cell_store[1];   /* placeholder to keep the struct non-empty */
    (void)cell_store;

    spinlock_init(&console_lock);

    serial_enabled = true;

    if (bi && (bi->flags & BOOT_FLAG_HAS_FRAMEBUFFER) && bi->fb.address) {
        console_fb_init(&bi->fb);
    }

    /* The VGA text buffer is only usable if the firmware left us in a text
     * mode. Setting a graphics mode for the framebuffer has repurposed it. */
    vga_enabled = !console_fb_active();

    if (vga_enabled) {
        vga_clear();
        outw(0x3D4, 0x200A);
    }

    scrollback = 0;
    cursor_x = 0;
    cursor_y = 0;
}
