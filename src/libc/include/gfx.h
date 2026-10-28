/*
 * gfx.h — a small pixel graphics API for freestanding userspace programs.
 * Colors are packed according to the active display's 32-bit channel layout.
 */
#ifndef GFX_H
#define GFX_H

#include <stdint.h>
#include <uapi/graphics.h>

struct gfx_canvas {
    uint32_t width;
    uint32_t height;
    uint32_t *pixels;
    struct gfx_display_info display;
};

int gfx_open(struct gfx_canvas *canvas);
void gfx_close(struct gfx_canvas *canvas);
uint32_t gfx_rgb(const struct gfx_canvas *canvas, uint8_t r, uint8_t g, uint8_t b);
void gfx_clear(struct gfx_canvas *canvas, uint32_t color);
void gfx_pixel(struct gfx_canvas *canvas, int x, int y, uint32_t color);
void gfx_line(struct gfx_canvas *canvas, int x0, int y0, int x1, int y1,
              uint32_t color);
void gfx_fill_rect(struct gfx_canvas *canvas, int x, int y, int width, int height,
                   uint32_t color);
void gfx_fill_triangle(struct gfx_canvas *canvas, int x0, int y0, int x1, int y1,
                       int x2, int y2, uint32_t color);
int gfx_present(struct gfx_canvas *canvas);

#endif
