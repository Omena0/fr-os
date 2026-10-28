#include <gfx.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    struct gfx_canvas canvas;
    int x0, y0, x1, y1, x2, y2;
    uint32_t gold, blue, white;

    if (gfx_open(&canvas) < 0) {
        fprintf(stderr, "triangle: graphics unavailable (%d)\n", errno);
        return 1;
    }

    printf("triangle: rendering on a %ux%u framebuffer\n",
           canvas.width, canvas.height);

    /* Claim the screen before rendering. */
    if (gfx_claim_screen() < 0) {
        fprintf(stderr, "triangle: could not claim screen (%d)\n", errno);
        gfx_close(&canvas);
        return 1;
    }

    /* Three deliberately arbitrary, fixed points for a repeatable demo. */
    x0 = (int)(canvas.width * 19 / 100); y0 = (int)(canvas.height * 72 / 100);
    x1 = (int)(canvas.width * 77 / 100); y1 = (int)(canvas.height * 81 / 100);
    x2 = (int)(canvas.width * 56 / 100); y2 = (int)(canvas.height * 18 / 100);

    gold = gfx_rgb(&canvas, 242, 177, 54);
    blue = gfx_rgb(&canvas, 28, 44, 76);
    white = gfx_rgb(&canvas, 250, 248, 240);
    gfx_clear(&canvas, blue);
    gfx_fill_triangle(&canvas, x0, y0, x1, y1, x2, y2, gold);
    gfx_line(&canvas, x0, y0, x1, y1, 3, white);
    gfx_line(&canvas, x1, y1, x2, y2, 3, white);
    gfx_line(&canvas, x2, y2, x0, y0, 3, white);

    if (gfx_present(&canvas) < 0) {
        fprintf(stderr, "triangle: present failed (%d)\n", errno);
        gfx_close(&canvas);
        return 1;
    }
    static const char done[] = "triangle: frame presented\n";
    (void)write(STDOUT_FILENO, done, sizeof(done) - 1);

    /* Wait for Ctrl+C before returning to the shell. */
    {
        char c;
        while (read(STDIN_FILENO, &c, 1) > 0) {
            if (c == 0x03)  /* Ctrl+C */
                break;
        }
    }

    gfx_close(&canvas);
    return 0;
}
