/* Small, stable userspace graphics ABI. */
#ifndef UAPI_GRAPHICS_H
#define UAPI_GRAPHICS_H

#include <stdint.h>

struct gfx_display_info {
    uint32_t size;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t bpp;
    uint8_t red_shift;
    uint8_t green_shift;
    uint8_t blue_shift;
    uint32_t reserved;
};

#endif
