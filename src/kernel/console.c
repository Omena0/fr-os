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
 * A rendered text cell. The framebuffer backend keeps its own attribute per cell
 * because it owns the pixels; there is no shared buffer with the VGA backend
 * when both are active.
 */
struct fb_cell {
	char ch;
	uint8_t attr;
	uint8_t fg_r, fg_g, fg_b;
	uint8_t bg_r, bg_g, bg_b;
};

struct fb_text {
	struct framebuffer_info *info;

	/*
	 * The framebuffer's physical base, as the firmware reported it. Kept as a
	 * physical address and resolved per use for the same reason the VGA
	 * buffer is: it is a low physical address, and the map that reaches it
	 * changes when vmm_switch_to_kernel_pgd() replaces the bootloader's
	 * identity view. VBE framebuffers are usually high (0xFD000000 on
	 * SeaBIOS), so this is not even always inside the 4 GiB either map
	 * covers.
	 */
	phys_addr_t phys;

	uint32_t pitch;
	uint32_t cols, rows;
	uint32_t cursor_x, cursor_y;
	uint64_t scrollback;

	/* Cell buffer and glyph atlas. */
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

extern const uint8_t font8x16[256][16];

/* 3-3-2 RGB palette, indexed 0-7 by attribute bits 0-2. */
static const uint8_t vga_palette_rgb[8][3] = {
	{ 0x00, 0x00, 0x00 },   /* black */
	{ 0xAA, 0x00, 0x00 },   /* red */
	{ 0x00, 0xAA, 0x00 },   /* green */
	{ 0xAA, 0x55, 0x00 },   /* brown */
	{ 0x00, 0x00, 0xAA },   /* blue */
	{ 0xAA, 0x00, 0xAA },   /* magenta */
	{ 0x00, 0xAA, 0xAA },   /* cyan */
	{ 0xAA, 0xAA, 0xAA },   /* light grey */
};

static inline uint8_t attr_fg(uint8_t attr)
{
	return attr & 0x07;
}

static inline uint8_t attr_bg(uint8_t attr)
{
	return (attr >> 4) & 0x07;
}

static inline bool attr_bold(uint8_t attr)
{
	return (attr & 0x08) != 0;
}

/* ------------------------------------------------------------ state -------- */

static spinlock_t console_lock;
static uint32_t cursor_x, cursor_y;
static uint8_t  current_attr = TERM_ATTR_NORMAL;
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
static char esc_params[16];
static uint8_t esc_param_count;

static void console_fb_init(struct framebuffer_info *info);
static void console_fb_putc(char c, uint8_t attr);
static void console_fb_scroll(void);
static void console_fb_clear(void);
static void console_fb_move(uint32_t x, uint32_t y);

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

/*
 * The VGA text buffer, resolved through whichever map is live.
 *
 * 0xB8000 is a fixed physical address, and the way to reach it changes
 * underneath this file: vmm_switch_to_kernel_pgd() replaces the bootloader's
 * identity view of the low 4 GiB. The address is recomputed on every access
 * rather than cached: console_init(NULL) enables this backend before the
 * switch and the first character after it is a store, so a cached value from
 * the wrong regime is a #PF.
 */
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

	/* Copy every line up by one. The 0xB8000 buffer is in write-through
	 * memory, so a volatile byte copy is correct and fast enough. */
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
			    uint8_t bg_r, uint8_t bg_g, uint8_t bg_b)
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

		for (uint32_t gx = 0; gx < fb.font_width; gx++) {
			bool on = (bits & (0x80 >> gx)) != 0;
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

	for (uint32_t i = 0; i < fb.cols * fb.rows; i++) {
		fb.cells[i].ch = ' ';
		fb.cells[i].attr = TERM_ATTR_NORMAL;
	}

	/* Paint the whole background once so no garbage is visible. */
	for (uint32_t r = 0; r < fb.rows; r++) {
		for (uint32_t c = 0; c < fb.cols; c++)
			fb_render_glyph(c, r, ' ', 0xAA, 0xAA, 0xAA, 0, 0, 0);
	}
}

static void console_fb_scroll(void)
{
	if (!fb.cells || fb.cols == 0)
		return;

	memmove(&fb.cells[0], &fb.cells[fb.cols],
		(fb.rows - 1) * fb.cols * sizeof(struct fb_cell));
	for (uint32_t c = 0; c < fb.cols; c++) {
		fb.cells[(fb.rows - 1) * fb.cols + c].ch = ' ';
		fb.cells[(fb.rows - 1) * fb.cols + c].attr = current_attr;
	}

	/* Repaint only the new bottom line rather than the whole screen. */
	for (uint32_t c = 0; c < fb.cols; c++) {
		struct fb_cell *cell = &fb.cells[(fb.rows - 1) * fb.cols + c];
		const uint8_t *p = vga_palette_rgb[attr_fg(cell->attr)];
		uint8_t fg_r = attr_bold(cell->attr) ? 0xFF : p[0];
		uint8_t fg_g = attr_bold(cell->attr) ? 0xFF : p[1];
		uint8_t fg_b = attr_bold(cell->attr) ? 0xFF : p[2];
		const uint8_t *bg = vga_palette_rgb[attr_bg(cell->attr)];

		fb_render_glyph(c, fb.rows - 1, cell->ch, fg_r, fg_g, fg_b,
				bg[0], bg[1], bg[2]);
	}
	fb.scrollback++;
}

static void console_fb_clear(void)
{
	if (!fb.cells || fb.cols == 0)
		return;

	for (uint32_t i = 0; i < fb.cols * fb.rows; i++) {
		fb.cells[i].ch = ' ';
		fb.cells[i].attr = current_attr;
	}
	for (uint32_t r = 0; r < fb.rows; r++)
		for (uint32_t c = 0; c < fb.cols; c++)
			fb_render_glyph(c, r, ' ', 0xAA, 0xAA, 0xAA, 0, 0, 0);
}

static void console_fb_move(uint32_t x, uint32_t y)
{
	if (x < fb.cols)
		fb.cursor_x = x;
	if (y < fb.rows)
		fb.cursor_y = y;
}

static void console_fb_putc(char c, uint8_t attr)
{
	uint32_t x = fb.cursor_x;
	uint32_t y = fb.cursor_y;

	if (fb.cells && fb.cols) {
		struct fb_cell *cell = &fb.cells[y * fb.cols + x];

		cell->ch = c;
		cell->attr = attr;

		const uint8_t *fg = vga_palette_rgb[attr_fg(attr)];
		const uint8_t *bg = vga_palette_rgb[attr_bg(attr)];
		uint8_t fg_r = attr_bold(attr) ? 0xFF : fg[0];
		uint8_t fg_g = attr_bold(attr) ? 0xFF : fg[1];
		uint8_t fg_b = attr_bold(attr) ? 0xFF : fg[2];

		fb_render_glyph(x, y, c, fg_r, fg_g, fg_b, bg[0], bg[1], bg[2]);
		return;
	}

	/* No cell buffer: draw directly. */
	const uint8_t *fg = vga_palette_rgb[attr_fg(attr)];
	const uint8_t *bg = vga_palette_rgb[attr_bg(attr)];
	uint8_t fg_r = attr_bold(attr) ? 0xFF : fg[0];
	uint8_t fg_g = attr_bold(attr) ? 0xFF : fg[1];
	uint8_t fg_b = attr_bold(attr) ? 0xFF : fg[2];

	fb_render_glyph(x, y, c, fg_r, fg_g, fg_b, bg[0], bg[1], bg[2]);
}

/* ------------------------------------------------------------- cursor ------ */

static void console_newline(void)
{
	cursor_x = 0;
	cursor_y++;
	scrollback++;

	/* Parenthesis matters: "cursor_y >= vga_enabled ? A : B" parses as
	 * "(cursor_y >= vga_enabled) ? A : B", which is a scroll on every
	 * single line once the cursor is past column zero. */
	uint32_t rows = vga_enabled ? VGA_ROWS : fb.rows;

	if (cursor_y >= rows) {
		if (vga_enabled)
			vga_scroll();
		if (console_fb_active())
			console_fb_scroll();
		/* Keep the logical grid at the smaller of the two so a character
		 * written once reaches both backends at the same row. */
		uint32_t limit = vga_enabled && console_fb_active() ?
			MIN(VGA_ROWS, fb.rows) :
			(vga_enabled ? VGA_ROWS : fb.rows);
		cursor_y = limit - 1;
	}
	if (console_fb_active())
		console_fb_move(cursor_x, cursor_y);
}

/* ------------------------------------------------------------- escapes ----- */

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
			return true;
		}
		return false;

	case ESC_SEEN:
		if (c == '[') {
			esc_state = ESC_CSI;
			esc_param_count = 0;
			return true;
		}
		/* ESC followed by anything else is a two-byte sequence we do not
		 * implement. Drop it rather than printing the second byte. */
		esc_state = ESC_NONE;
		return true;

	case ESC_CSI:
		if (c >= '0' && c <= '9') {
			if (esc_param_count < sizeof(esc_params))
				esc_params[esc_param_count] = c;
			esc_param_count++;
			return true;
		}
		if (c == ';') {
			/* Only the first parameter is used; a second one would be a
			 * colour request, which this console ignores. */
			if (esc_param_count < sizeof(esc_params))
				esc_params[esc_param_count] = 0;
			esc_param_count++;
			return true;
		}

		esc_state = ESC_NONE;
		esc_params[esc_param_count < sizeof(esc_params) ?
			    esc_param_count : 0] = '\0';

		/* An unparseable parameter is treated as absent rather than
		 * fatal: a malformed escape sequence in a log line should not
		 * stop the line from being printed. */
		unsigned long n_param = 0;

		kstrtoul(esc_params, &n_param, 10);
		uint32_t n = (uint32_t)n_param;

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
		case 'K':       /* erase line */
			for (uint32_t x = cursor_x; x < VGA_COLS; x++)
				vga_putc_at(x, cursor_y, ' ', current_attr);
			return true;
		case 'm':       /* colour: ignore, the console has one attribute */
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
		if (serial_enabled)
			serial_putc_blocking('\n');
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
		if (console_fb_active())
			console_fb_move(cursor_x, cursor_y);
		if (serial_enabled)
			serial_putc_blocking('\b');
		return;
	}

	if (vga_enabled)
		vga_putc_at(cursor_x, cursor_y, c, attr);
	if (console_fb_active())
		console_fb_putc(c, attr);

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
		if (serial_enabled)
			serial_putc_blocking(s[i]);
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

void console_set_attr(uint8_t attr)
{
	current_attr = attr;
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
	 * mode. Setting a graphics mode for the framebuffer would have
	 * repurposed it, so the two backends are mutually exclusive in practice;
	 * enabling both would corrupt the other's memory.
	 *
	 * An assignment, not a set, because this function is called twice: once
	 * with no bootinfo to get serial output up before anything can fail, and
	 * once with it to add the framebuffer. The first call has to enable VGA
	 * because there is nothing else yet, and the second has to be able to
	 * turn it back off again when a framebuffer turns out to be there. */
	vga_enabled = !console_fb_active();

	if (vga_enabled) {
		vga_clear();
		/* Park the cursor just past the end of the buffer so the hardware
		 * cursor is not drawn over the last line of text. */
		outw(0x3D4, 0x200A);
	}

	scrollback = 0;
}
