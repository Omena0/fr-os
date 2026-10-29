/*
 * paint.c — paint with the mouse on the text console.
 *
 * The console is a grid of 8x16 cells. The kernel exposes the pointer in both
 * pixel and character coordinates, and this program uses the character pair:
 * one cell per painted surface, which is the resolution the text screen has.
 *
 * Left button paints, right button cycles the palette, middle button clears the
 * canvas, q (or Esc / Ctrl-C) quits. The pointer is drawn as a block cursor in
 * the current palette colour so it is always visible against the canvas.
 *
 * The canvas is one byte per cell in user memory and redraw is dirty-cell only:
 * a full-screen repaint every frame would push a few thousand escape sequences
 * down a serial line thirty times a second.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <mouse.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <time.h>
#include <unistd.h>

/* Background colours (256-colour indices) that read clearly as filled cells. */
static const unsigned char palette[] = { 17, 22, 28, 52, 88, 124, 160, 196, 226, 231 };
#define PALETTE_N ((int)ARRAY_SIZE(palette))

static int cols = 80;
static int rows = 24;              /* last row is the status line */
static unsigned char *cells;       /* painted colour per cell, 0 = unpainted */
static unsigned char *shown;       /* what is currently on screen */

/*
 * A paint cell is two character cells wide: a font cell is 8x16 pixels, so two
 * of them make a square block. Every position the program prints must therefore
 * be scaled by CELL_CHARS, or the cursor lands at half the column its painted
 * block occupies.
 */
#define CELL_CHARS 2

static int cur_col, cur_row;
static int last_col = -1, last_row = -1;
static int colour;
static bool running = true;

static void move_to(int row, int col)
{
    printf("\033[%d;%dH", row + 1, col * CELL_CHARS + 1);
}

static void set_bg(unsigned char idx)
{
    printf("\033[48;5;%dm", idx);
}

static void reset_attr(void)
{
    fputs("\033[0m", stdout);
}

static void draw_cell(int col, int row, unsigned char value)
{
    move_to(row, col);
    if (value) {
        set_bg(value);
        fputs("  ", stdout);
    } else {
        fputs("\033[0m  ", stdout);
    }
    reset_attr();
    shown[row * cols + col] = value;
}

static void draw_cursor(void)
{
    move_to(cur_row, cur_col);
    printf("\033[48;5;%dm\033[30;1m[]\033[0m", palette[colour]);
}

static int cur_px, cur_py;
static int screen_w, screen_h;

static void draw_status(void)
{
    move_to(rows, 0);
    reset_attr();
    /* Both coordinate systems are shown. A discrepancy between the cell the
     * canvas thinks it is painting and the pixel the kernel reports is then
     * visible here instead of only as a misplaced block on screen. */
    printf("\033[K paint  cell %2d,%2d  px %4d,%4d  of %4d,%4d  colour %d/%d"
           "  [LMB paint  RMB colour  MMB clear  q quit]",
           cur_col, cur_row, cur_px, cur_py, screen_w, screen_h,
           colour + 1, PALETTE_N);
    reset_attr();
}

static void paint_at(int col, int row)
{
    if (col < 0 || col >= cols || row < 0 || row >= rows)
        return;
    cells[row * cols + col] = palette[colour];
}

/* Paint every cell between the previous pointer cell and this one, so a quick
 * flick of the mouse does not leave a dotted line. */
static void paint_line(int c0, int r0, int c1, int r1)
{
    int dx = c1 - c0;
    int dy = r1 - r0;
    int steps = MAX(abs(dx), abs(dy));

    if (steps == 0) {
        paint_at(c1, r1);
        return;
    }
    for (int i = 0; i <= steps; i++) {
        int c = c0 + (dx * i) / steps;
        int r = r0 + (dy * i) / steps;

        paint_at(c, r);
    }
}

static int parse_key(int *cycle_wanted)
{
    char buf[16];
    ssize_t n = read(0, buf, sizeof(buf));

    if (n <= 0)
        return 0;

    for (ssize_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)buf[i];

        if (c == 'q' || c == 'Q' || c == 0x03 || c == 0x1b)
            return 1;
        if (c == 'c' || c == 'C')
            *cycle_wanted = 1;
    }
    return 0;
}

static void redraw_all(void)
{
    fputs("\033[2J", stdout);
    memset(shown, 0, (size_t)cols * rows);
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++)
            if (cells[r * cols + c])
                draw_cell(c, r, cells[r * cols + c]);
}

int main(void)
{
    struct mouse_state m;
    int old_flags;
    int button_prev = 0;
    int dirty_colour = 0;

    if (mouse_get(&m) < 0) {
        fprintf(stderr, "paint: no pointer device (%d)\n", errno);
        return 1;
    }

    cols = (int)m.char_width / CELL_CHARS;
    rows = (int)m.char_height - 1;          /* reserve the status row */
    screen_w = (int)m.pixel_width;
    screen_h = (int)m.pixel_height;
    if (cols < 8 || rows < 3) {
        fprintf(stderr, "paint: console too small (%dx%d)\n", cols, rows);
        return 1;
    }

    cells = calloc((size_t)cols * rows, 1);
    shown = calloc((size_t)cols * rows, 1);
    if (!cells || !shown) {
        fprintf(stderr, "paint: out of memory\n");
        free(cells);
        free(shown);
        return 1;
    }

    old_flags = fcntl(0, F_GETFL, 0);
    if (old_flags >= 0)
        fcntl(0, F_SETFL, old_flags | O_NONBLOCK);

    fputs("\033[?25l", stdout);
    redraw_all();

    while (running) {
        if (mouse_get(&m) < 0)
            break;

        int c = m.char_x / CELL_CHARS;
        int r = m.char_y;

        cur_px = m.x;
        cur_py = m.y;

        if (c >= cols)
            c = cols - 1;
        if (r >= rows)
            r = rows - 1;

        /* Right button cycles the colour on the press edge only. */
        if ((m.buttons & MOUSE_BUTTON_RIGHT) &&
            !(button_prev & MOUSE_BUTTON_RIGHT))
            dirty_colour = 1;
        if ((m.buttons & MOUSE_BUTTON_MIDDLE) &&
            !(button_prev & MOUSE_BUTTON_MIDDLE)) {
            memset(cells, 0, (size_t)cols * rows);
            redraw_all();
        }

        if (dirty_colour) {
            colour = (colour + 1) % PALETTE_N;
            dirty_colour = 0;
            draw_status();
        }

        if ((m.buttons & MOUSE_BUTTON_LEFT) && c >= 0 && r >= 0) {
            if (last_col >= 0)
                paint_line(last_col, last_row, c, r);
            else
                paint_at(c, r);
        }

        /* Move the cursor: restore the cell it left, draw the new one. */
        if (c != cur_col || r != cur_row) {
            if (cur_col >= 0 && cur_row >= 0 && cur_col < cols &&
                cur_row < rows) {
                draw_cell(cur_col, cur_row, shown[cur_row * cols + cur_col]);
            }
            cur_col = c;
            cur_row = r;
        }

        /* Push only cells whose value changed. */
        for (int y = 0; y < rows; y++)
            for (int x = 0; x < cols; x++) {
                unsigned char want = cells[y * cols + x];

                if (want != shown[y * cols + x]) {
                    if (x == cur_col && y == cur_row) {
                        shown[y * cols + x] = want;
                    } else {
                        draw_cell(x, y, want);
                    }
                }
            }

        draw_cursor();
        draw_status();
        fflush(stdout);

        last_col = c;
        last_row = r;
        button_prev = m.buttons;

        if (parse_key(&dirty_colour))
            running = false;

        struct timespec ts = { 0, 30000000L };   /* ~33 Hz */
        nanosleep(&ts, NULL);
    }

    fputs("\033[0m\033[?25h\n", stdout);
    if (old_flags >= 0)
        fcntl(0, F_SETFL, old_flags);
    printf("paint: bye\n");
    free(cells);
    free(shown);
    return 0;
}