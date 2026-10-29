/*
 * mouse.c — libc wrapper over SYS_mouse_info.
 *
 * The syscall copies a struct mouse_state into the caller's buffer and returns
 * its size, which is how a future ABI can grow fields without breaking this
 * wrapper: the `size` field is filled in by the kernel and callers only read
 * fields they know about.
 */
#include <errno.h>
#include <mouse.h>
#include <string.h>
#include <sys/syscall.h>

extern long __syscall1(long number, long a1);

static int mouse_query(struct mouse_state *state)
{
    long ret;

    if (!state) {
        __errno = EINVAL;
        return -1;
    }

    memset(state, 0, sizeof(*state));
    ret = __syscall1(SYS_mouse_info, (long)state);
    if (ret < 0) {
        __errno = (int)-ret;
        return -1;
    }

    /* The kernel may know more fields than this libc does; it reports the
     * size it wrote, and anything beyond what we asked for is ignored. */
    if ((size_t)ret < sizeof(*state))
        memset((char *)state + ret, 0, sizeof(*state) - (size_t)ret);

    return state->present ? 1 : 0;
}

int mouse_get(struct mouse_state *state)
{
    return mouse_query(state);
}

int mouse_get_pixels(int *x, int *y, unsigned *buttons)
{
    struct mouse_state state;
    int ret = mouse_query(&state);

    if (ret < 0)
        return -1;
    if (x)
        *x = state.x;
    if (y)
        *y = state.y;
    if (buttons)
        *buttons = state.buttons;
    return ret;
}

int mouse_get_cell(int *col, int *row, unsigned *buttons)
{
    struct mouse_state state;
    int ret = mouse_query(&state);

    if (ret < 0)
        return -1;
    if (col)
        *col = state.char_x;
    if (row)
        *row = state.char_y;
    if (buttons)
        *buttons = state.buttons;
    return ret;
}