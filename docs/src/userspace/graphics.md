# Userspace graphics

The freestanding libc includes a small 32-bit framebuffer drawing API in
`<gfx.h>`. It keeps a software backbuffer in the process, clips drawing
operations to the display, and sends a complete frame to the kernel with
`gfx_present()`. The kernel validates the user buffer and copies each row to
the bootloader-selected framebuffer, respecting its pitch and channel layout.

```c
#include <gfx.h>

int main(void) {
    struct gfx_canvas screen;
    if (gfx_open(&screen) < 0)
        return 1;

    uint32_t dark = gfx_rgb(&screen, 20, 30, 50);
    uint32_t orange = gfx_rgb(&screen, 245, 165, 45);
    gfx_clear(&screen, dark);
    gfx_fill_triangle(&screen, 100, 300, 500, 350, 280, 80, orange);
    gfx_present(&screen);
    gfx_close(&screen);
    return 0;
}
```

The API also provides clipped pixel writes, Bresenham lines, and filled
rectangles. The `triangle` initrd program is a small complete example; at the
init prompt, run `run triangle`.

Keyboard input continues to use standard input. PS/2 arrow keys are delivered
as the terminal sequences `ESC [ A`, `ESC [ B`, `ESC [ C`, and `ESC [ D`, so a
game can read movement input with the same interface as a terminal program.
