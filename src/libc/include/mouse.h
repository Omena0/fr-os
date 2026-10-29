/*
 * mouse.h — read the pointing device.
 *
 * The kernel tracks a single absolute cursor. A program reads a snapshot; it
 * does not move the pointer, so several programs can observe it without
 * fighting over it.
 */
#ifndef MOUSE_H
#define MOUSE_H

#include <stdbool.h>
#include <uapi/mouse.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Button bits in struct mouse_state::buttons. */
#define MOUSE_BUTTON_LEFT    0x01
#define MOUSE_BUTTON_RIGHT   0x02
#define MOUSE_BUTTON_MIDDLE  0x04

/*
 * Fill *state with the current pointer position and buttons.
 *
 * Returns 1 if a device has reported at least once, 0 if the position is only
 * the initial centre (no mouse, or none moved yet), and -1 on error with errno
 * set. A position of (0,0) is a legitimate reading, so the return value, not
 * the coordinates, says whether the device exists.
 */
int mouse_get(struct mouse_state *state);

/* Convenience wrappers for programs that only want one coordinate system. */
int mouse_get_pixels(int *x, int *y, unsigned *buttons);
int mouse_get_cell(int *col, int *row, unsigned *buttons);

#ifdef __cplusplus
}
#endif

#endif /* MOUSE_H */