#include <gfx.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>

extern long __syscall1(long number, long a1);
extern long __syscall2(long number, long a1, long a2);
extern long __syscall0(long number);

static int syscall_status(long ret)
{
    if (ret < 0) {
        __errno = (int)-ret;
        return -1;
    }
    return (int)ret;
}

int gfx_open(struct gfx_canvas *canvas)
{
    uint64_t bytes;
    long ret;

    if (!canvas) {
        __errno = EINVAL;
        return -1;
    }
    memset(canvas, 0, sizeof(*canvas));
    ret = __syscall1(SYS_gfx_info, (long)&canvas->display);
    if (ret < 0)
        return syscall_status(ret);
    if (canvas->display.bpp != 32 || !canvas->display.width ||
        !canvas->display.height ||
        canvas->display.width > UINT32_MAX / canvas->display.height / 4) {
        __errno = ENODEV;
        return -1;
    }
    bytes = (uint64_t)canvas->display.width * canvas->display.height * 4;
    if (bytes > SIZE_MAX) {
        __errno = ENOMEM;
        return -1;
    }
    canvas->width = canvas->display.width;
    canvas->height = canvas->display.height;
    canvas->pixels = malloc((size_t)bytes);
    if (!canvas->pixels) {
        __errno = ENOMEM;
        return -1;
    }
    return 0;
}

void gfx_close(struct gfx_canvas *canvas)
{
    if (!canvas)
        return;
    free(canvas->pixels);
    memset(canvas, 0, sizeof(*canvas));
}

uint32_t gfx_rgb(const struct gfx_canvas *canvas, uint8_t r, uint8_t g, uint8_t b)
{
    if (!canvas)
        return 0;
    return ((uint32_t)r << canvas->display.red_shift) |
           ((uint32_t)g << canvas->display.green_shift) |
           ((uint32_t)b << canvas->display.blue_shift);
}

void gfx_clear(struct gfx_canvas *canvas, uint32_t color)
{
    if (!canvas || !canvas->pixels)
        return;
    size_t count = (size_t)canvas->width * canvas->height;
    for (size_t i = 0; i < count; i++)
        canvas->pixels[i] = color;
}

void gfx_pixel(struct gfx_canvas *canvas, int x, int y, uint32_t color)
{
    if (!canvas || !canvas->pixels || x < 0 || y < 0 ||
        (uint32_t)x >= canvas->width || (uint32_t)y >= canvas->height)
        return;
    canvas->pixels[(size_t)y * canvas->width + (uint32_t)x] = color;
}

void gfx_line(struct gfx_canvas *canvas, int x0, int y0, int x1, int y1,
               int thickness, uint32_t color)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    if (thickness < 1)
        thickness = 1;

    for (;;) {
        for (int ty = 0; ty < thickness; ty++)
            for (int tx = 0; tx < thickness; tx++)
                gfx_pixel(canvas, x0 + tx - thickness / 2,
                          y0 + ty - thickness / 2, color);
        if (x0 == x1 && y0 == y1)
            break;
        int twice = 2 * err;
        if (twice >= dy) {
            err += dy;
            x0 += sx;
        }
        if (twice <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void gfx_fill_rect(struct gfx_canvas *canvas, int x, int y, int width, int height,
                   uint32_t color)
{
    int64_t x1, y1;

    if (!canvas || !canvas->pixels || width <= 0 || height <= 0)
        return;
    x1 = (int64_t)x + width;
    y1 = (int64_t)y + height;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > canvas->width) x1 = canvas->width;
    if (y1 > canvas->height) y1 = canvas->height;
    for (int yy = y; yy < y1; yy++)
        for (int xx = x; xx < x1; xx++)
            canvas->pixels[(size_t)yy * canvas->width + (uint32_t)xx] = color;
}

static int64_t edge(int ax, int ay, int bx, int by, int px, int py)
{
    return ((int64_t)px - ax) * ((int64_t)by - ay) -
           ((int64_t)py - ay) * ((int64_t)bx - ax);
}

void gfx_fill_triangle(struct gfx_canvas *canvas, int x0, int y0, int x1, int y1,
                       int x2, int y2, uint32_t color)
{
    if (!canvas || !canvas->pixels)
        return;
    int area_sign = edge(x0, y0, x1, y1, x2, y2) < 0 ? -1 : 1;
    if (edge(x0, y0, x1, y1, x2, y2) == 0)
        return;
    int min_x = x0 < x1 ? x0 : x1;
    int max_x = x0 > x1 ? x0 : x1;
    int min_y = y0 < y1 ? y0 : y1;
    int max_y = y0 > y1 ? y0 : y1;
    if (x2 < min_x) min_x = x2;
    if (x2 > max_x) max_x = x2;
    if (y2 < min_y) min_y = y2;
    if (y2 > max_y) max_y = y2;
    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x >= (int)canvas->width) max_x = (int)canvas->width - 1;
    if (max_y >= (int)canvas->height) max_y = (int)canvas->height - 1;

    for (int y = min_y; y <= max_y; y++) {
        for (int x = min_x; x <= max_x; x++) {
            int64_t a = edge(x0, y0, x1, y1, x, y) * area_sign;
            int64_t b = edge(x1, y1, x2, y2, x, y) * area_sign;
            int64_t c = edge(x2, y2, x0, y0, x, y) * area_sign;
            if (a >= 0 && b >= 0 && c >= 0)
                canvas->pixels[(size_t)y * canvas->width + (uint32_t)x] = color;
        }
    }
}

int gfx_present(struct gfx_canvas *canvas)
{
    uint64_t bytes;
    long ret;

    if (!canvas || !canvas->pixels) {
        __errno = EINVAL;
        return -1;
    }
    bytes = (uint64_t)canvas->width * canvas->height * 4;
    ret = __syscall2(SYS_gfx_present, (long)canvas->pixels, (long)bytes);
    return syscall_status(ret);
}

int gfx_claim_screen(void)
{
    long ret;

    ret = __syscall1(SYS_screen_claim, 0);
    if (ret < 0)
        return syscall_status(ret);
    return 0;
}

void gfx_claim_screen_release(void)
{
    long ret = __syscall0(SYS_screen_release);
    (void)syscall_status(ret);
}
