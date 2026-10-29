/* Small, stable userspace input ABI. */
#ifndef UAPI_MOUSE_H
#define UAPI_MOUSE_H

#include <stdint.h>

/*
 * A snapshot of the pointing device.
 *
 * The kernel owns the pointer position: it starts centred and every PS/2
 * packet moves it by a relative delta, clamped to the display. Programs read
 * it; nobody writes it, so two programs cannot fight over the cursor.
 *
 * Both coordinate systems describe the same point. Pixel coordinates index the
 * framebuffer. Character coordinates index the text grid the console draws on:
 * the screen is divided into 8x16 cells, which is the VGA text cell and the
 * kernel's framebuffer font, so a program working in characters and one
 * working in pixels agree about where the pointer is.
 */
struct mouse_state {
    uint32_t size;        /* sizeof(struct mouse_state) */
    int32_t x;            /* pixel column, 0 .. pixel_width-1 */
    int32_t y;            /* pixel row, 0 .. pixel_height-1 */
    int32_t char_x;       /* text column, 0 .. char_width-1 */
    int32_t char_y;       /* text row, 0 .. char_height-1 */
    uint32_t pixel_width;
    uint32_t pixel_height;
    uint32_t char_width;
    uint32_t char_height;
    uint8_t buttons;      /* bit 0 left, bit 1 right, bit 2 middle */
    uint8_t present;      /* 1 once the device has reported at least once */
    uint16_t reserved;
};

#endif