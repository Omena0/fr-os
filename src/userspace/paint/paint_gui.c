/*
 * paint_gui.c — paint with the mouse on the framebuffer.
 *
 * The same program as paint.c, but it uses the graphics API instead of the
 * console: pixel resolution instead of character cells, and its own cursor
 * drawn into the framebuffer.
 *
 * The canvas holds the painting only. The cursor is drawn on top each frame and
 * removed by restoring the small patch of canvas it covered, so the cursor
 * never ends up baked into the picture and no full-frame copy is needed.
 *
 * Left button paints, right button cycles the brush colour, middle button
 * clears, q / Esc / Ctrl-C quits.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <gfx.h>
#include <mouse.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <time.h>
#include <unistd.h>

#define CURSOR_SIZE 9          /* odd so it is centred on the pointer */
#define BRUSH_RADIUS 4

static struct gfx_canvas canvas;

/* Patch under the cursor, saved so the cursor can be removed cheaply. */
static uint32_t cursor_patch[CURSOR_SIZE * CURSOR_SIZE];
static int cursor_sx = -1, cursor_sy = -1;

static int brush_x = -1, brush_y = -1;

static uint32_t colours[8];
static int colour_idx;

static void restore_cursor(void)
{
    if (cursor_sx < 0)
        return;
    for (int dy = 0; dy < CURSOR_SIZE; dy++) {
        for (int dx = 0; dx < CURSOR_SIZE; dx++) {
            int x = cursor_sx + dx;
            int y = cursor_sy + dy;

            if (x < 0 || y < 0 || (uint32_t)x >= canvas.width ||
                (uint32_t)y >= canvas.height)
                continue;
            canvas.pixels[(size_t)y * canvas.width + (size_t)x] =
                cursor_patch[dy * CURSOR_SIZE + dx];
        }
    }
}

static void draw_cursor(int cx, int cy)
{
    int left = cx - CURSOR_SIZE / 2;
    int top = cy - CURSOR_SIZE / 2;

    /* Save what the cursor is about to cover. */
    for (int dy = 0; dy < CURSOR_SIZE; dy++) {
        for (int dx = 0; dx < CURSOR_SIZE; dx++) {
            int x = left + dx;
            int y = top + dy;

            cursor_patch[dy * CURSOR_SIZE + dx] =
                (x < 0 || y < 0 || (uint32_t)x >= canvas.width ||
                 (uint32_t)y >= canvas.height)
                    ? 0
                    : canvas.pixels[(size_t)y * canvas.width + (size_t)x];
        }
    }
    cursor_sx = left;
    cursor_sy = top;

    /* A white crosshair with a dark outline: readable on any background. */
    for (int i = 0; i < CURSOR_SIZE; i++) {
        gfx_pixel(&canvas, cx - CURSOR_SIZE / 2 + i, cy,
                  gfx_rgb(&canvas, 20, 20, 20));
        gfx_pixel(&canvas, cx, cy - CURSOR_SIZE / 2 + i,
                  gfx_rgb(&canvas, 20, 20, 20));
    }
    for (int i = 1; i < CURSOR_SIZE - 1; i++) {
        gfx_pixel(&canvas, cx - CURSOR_SIZE / 2 + i, cy,
                  gfx_rgb(&canvas, 250, 250, 250));
        gfx_pixel(&canvas, cx, cy - CURSOR_SIZE / 2 + i,
                  gfx_rgb(&canvas, 250, 250, 250));
    }
    /* Centre dot shows the exact pixel painted. */
    gfx_pixel(&canvas, cx, cy, colours[colour_idx]);
}

static void brush_dab(int x, int y, uint32_t colour)
{
    gfx_fill_circle(&canvas, x, y, BRUSH_RADIUS, colour);
}

/* Interpolate between two pointer samples so a quick flick of the mouse paints
 * a line rather than a row of dots. */
static void brush_line(int x0, int y0, int x1, int y1, uint32_t colour)
{
    int dx = x1 - x0;
    int dy = y1 - y0;
    int steps = MAX(abs(dx), abs(dy));

    if (steps == 0) {
        brush_dab(x1, y1, colour);
        return;
    }
    for (int i = 0; i <= steps; i++) {
        int x = x0 + (dx * i) / steps;
        int y = y0 + (dy * i) / steps;

        brush_dab(x, y, colour);
    }
}

static int poll_keys(void)
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
            colour_idx = (colour_idx + 1) % (int)ARRAY_SIZE(colours);
    }
    return 0;
}

int main(void)
{
    struct mouse_state m;
    int old_flags;
    int button_prev = 0;
    int running = 1;

    if (gfx_open(&canvas) < 0) {
        fprintf(stderr, "paint_gui: graphics unavailable (%d)\n", errno);
        return 1;
    }
    if (mouse_get(&m) < 0) {
        fprintf(stderr, "paint_gui: no pointer device (%d)\n", errno);
        gfx_close(&canvas);
        return 1;
    }
    if (gfx_claim_screen() < 0) {
        fprintf(stderr, "paint_gui: could not claim screen (%d)\n", errno);
        gfx_close(&canvas);
        return 1;
    }

    colours[0] = gfx_rgb(&canvas, 242, 177, 54);   /* gold */
    colours[1] = gfx_rgb(&canvas, 220, 60, 60);    /* red */
    colours[2] = gfx_rgb(&canvas, 60, 200, 90);    /* green */
    colours[3] = gfx_rgb(&canvas, 70, 140, 240);   /* blue */
    colours[4] = gfx_rgb(&canvas, 200, 90, 220);   /* magenta */
    colours[5] = gfx_rgb(&canvas, 60, 210, 210);   /* cyan */
    colours[6] = gfx_rgb(&canvas, 245, 245, 245);  /* white */
    colours[7] = gfx_rgb(&canvas, 30, 30, 30);     /* near-black */

    gfx_clear(&canvas, gfx_rgb(&canvas, 16, 18, 24));
    printf("paint_gui: %ux%u canvas, pointer %d,%d\n",
           canvas.width, canvas.height, m.x, m.y);

    old_flags = fcntl(0, F_GETFL, 0);
    if (old_flags >= 0)
        fcntl(0, F_SETFL, old_flags | O_NONBLOCK);

    while (running) {
        if (mouse_get(&m) < 0)
            break;

        int x = m.x;
        int y = m.y;

        if (x < 0)
            x = 0;
        if (y < 0)
            y = 0;
        if ((uint32_t)x >= canvas.width)
            x = (int)canvas.width - 1;
        if ((uint32_t)y >= canvas.height)
            y = (int)canvas.height - 1;

        /* Undo the previous frame's cursor before anything else draws. */
        restore_cursor();

        if ((m.buttons & MOUSE_BUTTON_RIGHT) &&
            !(button_prev & MOUSE_BUTTON_RIGHT))
            colour_idx = (colour_idx + 1) % (int)ARRAY_SIZE(colours);

        if ((m.buttons & MOUSE_BUTTON_MIDDLE) &&
            !(button_prev & MOUSE_BUTTON_MIDDLE)) {
            gfx_clear(&canvas, gfx_rgb(&canvas, 16, 18, 24));
            brush_x = -1;
        }

        if (m.buttons & MOUSE_BUTTON_LEFT) {
            if (brush_x >= 0)
                brush_line(brush_x, brush_y, x, y, colours[colour_idx]);
            else
                brush_dab(x, y, colours[colour_idx]);
            brush_x = x;
            brush_y = y;
        } else {
            brush_x = -1;
        }

        draw_cursor(x, y);

        if (gfx_present(&canvas) < 0) {
            fprintf(stderr, "paint_gui: present failed (%d)\n", errno);
            break;
        }

        button_prev = m.buttons;

        if (poll_keys())
            running = 0;

        struct timespec ts = { 0, 16000000L };   /* ~60 Hz */
        nanosleep(&ts, NULL);
    }

    gfx_claim_screen_release();
    gfx_close(&canvas);
    if (old_flags >= 0)
        fcntl(0, F_SETFL, old_flags);
    printf("paint_gui: bye\n");
    return 0;
}