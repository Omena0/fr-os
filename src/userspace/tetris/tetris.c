/*
 * tetris.c — a framebuffer Tetris.
 *
 * Guidelines followed, TETR.IO as the reference where it does not turn into a
 * spec implementation:
 *
 *   - SRS rotation with the TETR.IO SRS+ kick table, including 180 kicks.
 *   - T-spin detection by the three-corner rule, with mini distinction, and
 *     all-spin detection for every other piece (last action was a rotation and
 *     the piece is immobile).
 *   - TETR.IO-style scoring: line values, spin values, back-to-back x1.5,
 *     combo, soft/hard drop points, level scaling.
 *
 * Tuning constants are here as #defines so the feel can be changed without
 * reading the game loop.
 *
 * Keys: a/d move, s soft drop, space hard drop, c hold, ,/. rotate CCW/CW,
 * f rotate 180, p pause, q/Esc quit. Arrow left/right/down also work.
 */
#include <errno.h>
#include <fcntl.h>
#include <gfx.h>
#include <kbd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------- tuning ----- */

#define BOARD_W            10
#define BOARD_H            20
#define CELL_PX            28
#define FRAME_MS           16      /* render/poll period */
#define LOCK_DELAY_MS      500
/* Held movement timing (DAS = time before repeat begins, ARR = repeat rate).
 * These are the game's own values, not the keyboard's typematic setting,
 * because the hardware delay cannot go below 250 ms. */
#define DAS_DELAY_MS       167
#define DAS_REPEAT_MS      35
#define SOFT_REPEAT_MS     35
#define LOCK_RESET_LIMIT   15
#define SOFT_DROP_MS       40
#define DAS_MS             140     /* delayed auto shift */
#define ARR_MS             35      /* auto repeat rate */
#define NEXT_COUNT         4
#define LINES_PER_LEVEL    10

/* Held movement repeats at the keyboard's typematic rate. Tetris wants a
 * shorter delay and a faster rate than the prompt; see kbd.h. The previous
 * value is restored when the game exits. */

/* Gravity in milliseconds per cell, indexed by level; the last entry repeats. */
static const int gravity_ms[] = {
    800, 720, 630, 550, 470, 380, 300, 220, 160, 120,
    100,  90,  80,  70,  60,  50,  45,  40,  35,  30,
};
#define GRAVITY_LEVELS ((int)(sizeof(gravity_ms) / sizeof(gravity_ms[0])))

/* -------------------------------------------------------------- pieces ----- */

enum { P_I = 1, P_O, P_T, P_S, P_Z, P_J, P_L, P_COUNT = 8 };

/* One 4x4 mask per rotation. Bit 15 is the top-left cell. */
static const uint16_t shapes[P_COUNT][4] = {
    [P_I] = { 0x0F00, 0x2222, 0x00F0, 0x4444 },
    [P_O] = { 0x6600, 0x6600, 0x6600, 0x6600 },
    [P_T] = { 0x4E00, 0x4640, 0x0E40, 0x4C40 },
    [P_S] = { 0x6C00, 0x4620, 0x06C0, 0x8C40 },
    [P_Z] = { 0xC600, 0x2640, 0x0C60, 0x4C80 },
    [P_J] = { 0x8E00, 0x6440, 0x0E20, 0x44C0 },
    [P_L] = { 0x2E00, 0x4460, 0x0E80, 0xC440 },
};

/* TETR.IO SRS+ kicks as {dx, dy}, with +y up (SRS convention). The tables are
 * indexed [from][to]; transitions with no own table use the JLSTZ set. */
struct kick { int8_t x, y; };

static const struct kick kicks_jlstz[4][4][5] = {
    [0][1] = { {0,0},{-1,0},{-1,1},{0,-2},{-1,-2} },
    [1][0] = { {0,0},{1,0},{1,-1},{0,2},{1,2} },
    [1][2] = { {0,0},{1,0},{1,-1},{0,2},{1,2} },
    [2][1] = { {0,0},{-1,0},{-1,1},{0,-2},{-1,-2} },
    [2][3] = { {0,0},{1,0},{1,1},{0,-2},{1,-2} },
    [3][2] = { {0,0},{-1,0},{-1,-1},{0,2},{-1,2} },
    [3][0] = { {0,0},{-1,0},{-1,-1},{0,2},{-1,2} },
    [0][3] = { {0,0},{1,0},{1,1},{0,-2},{1,-2} },
};

static const struct kick kicks_i[4][4][5] = {
    [0][1] = { {0,0},{-2,0},{1,0},{-2,-1},{1,2} },
    [1][0] = { {0,0},{2,0},{-1,0},{2,1},{-1,-2} },
    [1][2] = { {0,0},{-1,0},{2,0},{-1,2},{2,-1} },
    [2][1] = { {0,0},{1,0},{-2,0},{1,-2},{-2,1} },
    [2][3] = { {0,0},{2,0},{-1,0},{2,1},{-1,-2} },
    [3][2] = { {0,0},{-2,0},{1,0},{-2,-1},{1,2} },
    [3][0] = { {0,0},{1,0},{-2,0},{1,-2},{-2,1} },
    [0][3] = { {0,0},{-1,0},{2,0},{-1,2},{2,-1} },
};

/* 180 kicks (TETR.IO). */
static const struct kick kicks_180[4][4][6] = {
    [0][2] = { {0,0},{0,1},{1,1},{-1,1},{1,0},{-1,0} },
    [2][0] = { {0,0},{0,-1},{1,-1},{-1,-1},{1,0},{-1,0} },
    [1][3] = { {0,0},{1,0},{-1,0},{1,1},{-1,1},{0,-1} },
    [3][1] = { {0,0},{-1,0},{1,0},{-1,-1},{1,-1},{0,1} },
};

/* --------------------------------------------------------------- state ----- */

static struct gfx_canvas canvas;
static uint8_t board[BOARD_H][BOARD_W];      /* 0 empty, else piece id */

static int piece, rot, px, py;
static int hold_piece = 0;
static bool hold_used;
static int next_queue[NEXT_COUNT];

static int score, lines, level, combo;
static bool back_to_back;
static bool game_over, paused, quit_game;
static bool dirty = true;

/* Held-key state for DAS. ps2_mode latches once the keyboard reports a held
 * key: from then on actions come from the bitmap, and character events are
 * drained, so hardware typematic repeats cannot look like fresh presses. */
static uint64_t keys_prev;
static bool ps2_mode;
static int das_dir, das_timer, arr_timer, soft_timer;

static int gravity_timer, lock_timer, lock_resets;
static bool grounded;
static bool last_was_rotation;

static uint32_t background, cell_edge, piece_colours[P_COUNT], ghost_colour;
static int board_x = 40, board_y = 40;
static int panel_x;

/* --------------------------------------------------------------- glyphs ---- */

/* 3x5 font, rows top to bottom, bit 2 is the left pixel. */
static const uint8_t font3x5[][5] = {
    ['A'] = {0x2,0x5,0x7,0x5,0x5}, ['B'] = {0x6,0x5,0x6,0x5,0x6},
    ['C'] = {0x3,0x4,0x4,0x4,0x3}, ['D'] = {0x6,0x5,0x5,0x5,0x6},
    ['E'] = {0x7,0x4,0x6,0x4,0x7}, ['F'] = {0x7,0x4,0x6,0x4,0x4},
    ['G'] = {0x3,0x4,0x5,0x5,0x3}, ['H'] = {0x5,0x5,0x7,0x5,0x5},
    ['I'] = {0x7,0x2,0x2,0x2,0x7}, ['J'] = {0x1,0x1,0x1,0x5,0x2},
    ['K'] = {0x5,0x5,0x6,0x5,0x5}, ['L'] = {0x4,0x4,0x4,0x4,0x7},
    ['M'] = {0x5,0x7,0x7,0x5,0x5}, ['N'] = {0x5,0x7,0x7,0x7,0x5},
    ['O'] = {0x2,0x5,0x5,0x5,0x2}, ['P'] = {0x6,0x5,0x6,0x4,0x4},
    ['Q'] = {0x2,0x5,0x5,0x6,0x3}, ['R'] = {0x6,0x5,0x6,0x5,0x5},
    ['S'] = {0x3,0x4,0x2,0x1,0x6}, ['T'] = {0x7,0x2,0x2,0x2,0x2},
    ['U'] = {0x5,0x5,0x5,0x5,0x3}, ['V'] = {0x5,0x5,0x5,0x5,0x2},
    ['W'] = {0x5,0x5,0x7,0x7,0x5}, ['X'] = {0x5,0x5,0x2,0x5,0x5},
    ['Y'] = {0x5,0x5,0x2,0x2,0x2}, ['Z'] = {0x7,0x1,0x2,0x4,0x7},
    ['0'] = {0x7,0x5,0x5,0x5,0x7}, ['1'] = {0x2,0x6,0x2,0x2,0x7},
    ['2'] = {0x6,0x1,0x2,0x4,0x7}, ['3'] = {0x6,0x1,0x2,0x1,0x6},
    ['4'] = {0x5,0x5,0x7,0x1,0x1}, ['5'] = {0x7,0x4,0x6,0x1,0x6},
    ['6'] = {0x3,0x4,0x7,0x5,0x7}, ['7'] = {0x7,0x1,0x2,0x2,0x2},
    ['8'] = {0x7,0x5,0x7,0x5,0x7}, ['9'] = {0x7,0x5,0x7,0x1,0x6},
    ['-'] = {0x0,0x0,0x7,0x0,0x0}, [':'] = {0x0,0x2,0x0,0x2,0x0},
    ['.'] = {0x0,0x0,0x0,0x0,0x2}, [' '] = {0,0,0,0,0},
};

static void draw_glyph(int x, int y, char c, int scale, uint32_t colour)
{
    const uint8_t *g;

    c = (char)((c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c);
    if ((unsigned char)c >= sizeof(font3x5) / sizeof(font3x5[0]))
        return;
    g = font3x5[(unsigned char)c];
    for (int row = 0; row < 5; row++)
        for (int col = 0; col < 3; col++)
            if (g[row] & (1u << (2 - col)))
                gfx_fill_rect(&canvas, x + col * scale, y + row * scale,
                              scale, scale, colour);
}

static int text_width(const char *s, int scale)
{
    return (int)strlen(s) * 4 * scale;
}

static void draw_text(int x, int y, const char *s, int scale, uint32_t colour)
{
    for (; *s; s++, x += 4 * scale)
        draw_glyph(x, y, *s, scale, colour);
}

static void draw_number(int x, int y, int value, int scale, uint32_t colour)
{
    char buf[16];

    snprintf(buf, sizeof(buf), "%d", value);
    draw_text(x, y, buf, scale, colour);
}

/* --------------------------------------------------------------- board ----- */

static bool occupied(int x, int y)
{
    if (x < 0 || x >= BOARD_W)
        return true;
    if (y >= BOARD_H)
        return true;
    if (y < 0)
        return false;
    return board[y][x] != 0;
}

static bool piece_cells(int type, int r, int x, int y, int *out_x, int *out_y)
{
    uint16_t mask = shapes[type][r & 3];
    int n = 0;

    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++)
            if (mask & (1u << (15 - (row * 4 + col)))) {
                out_x[n] = x + col;
                out_y[n] = y + row;
                n++;
            }
    return n > 0;
}

static bool collides(int type, int r, int x, int y)
{
    int cx[4], cy[4];

    piece_cells(type, r, x, y, cx, cy);
    for (int i = 0; i < 4; i++)
        if (occupied(cx[i], cy[i]))
            return true;
    return false;
}

/* True if the piece cannot move in any direction (TETR.IO all-spin test). */
static bool immobile(void)
{
    return collides(piece, rot, px - 1, py) &&
           collides(piece, rot, px + 1, py) &&
           collides(piece, rot, px, py - 1) &&
           collides(piece, rot, px, py + 1);
}

/* Number of filled corners around the piece's 3x3 box, and whether the two
 * corners on the side it points to are both filled (for mini detection). */
static int t_corners(bool *front_pair)
{
    int cx = px + 1, cy = py + 1;
    bool tl = occupied(cx - 1, cy - 1);
    bool tr = occupied(cx + 1, cy - 1);
    bool bl = occupied(cx - 1, cy + 1);
    bool br = occupied(cx + 1, cy + 1);

    if (front_pair) {
        switch (rot) {
        case 0: *front_pair = tl && tr; break;   /* points up */
        case 1: *front_pair = tr && br; break;   /* right */
        case 2: *front_pair = bl && br; break;   /* down */
        default: *front_pair = tl && bl; break;  /* left */
        }
    }
    return tl + tr + bl + br;
}

/* --------------------------------------------------------------- scoring --- */

static int line_value(int n)
{
    static const int value[5] = { 0, 100, 300, 500, 800 };

    return value[n > 4 ? 4 : n];
}

static int spin_value(int n, bool mini, bool is_t)
{
    if (!is_t)
        return n == 0 ? 400 : (n == 1 ? 800 : (n == 2 ? 1200 : 1600));
    if (mini)
        return n == 0 ? 100 : (n == 1 ? 200 : 400);
    return n == 0 ? 400 : (n == 1 ? 800 : (n == 2 ? 1200 : 1600));
}

/* --------------------------------------------------------------- input ----- */

/*
 * Read one actionable byte. Escape sequences (arrow keys, function keys) are
 * consumed and ignored: a serial terminal sends arrows as ESC [ A-D, and
 * treating the ESC as "quit" made an accidental arrow key end the game.
 */
static int read_key(void)
{
    unsigned char c;
    ssize_t n = read(0, &c, 1);

    if (n != 1)
        return -1;
    if (c == 0x1b) {
        unsigned char junk;

        for (int i = 0; i < 2; i++)
            if (read(0, &junk, 1) != 1)
                break;
        return -1;
    }
    return (int)c;
}

/* --------------------------------------------------------------- render ---- */

static void draw_cell(int bx, int by, uint32_t colour)
{
    gfx_fill_rect(&canvas, board_x + bx * CELL_PX, board_y + by * CELL_PX,
                  CELL_PX - 1, CELL_PX - 1, colour);
    gfx_fill_rect(&canvas, board_x + bx * CELL_PX, board_y + by * CELL_PX,
                  CELL_PX - 1, 2, cell_edge);
}

static void draw_piece_preview(int type, int x, int y, int cell)
{
    uint16_t mask = shapes[type][0];

    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++)
            if (mask & (1u << (15 - (row * 4 + col))))
                gfx_fill_rect(&canvas, x + col * cell, y + row * cell,
                              cell - 1, cell - 1, piece_colours[type]);
}

static void render(void)
{
    gfx_clear(&canvas, background);

    /* Playfield */
    gfx_fill_rect(&canvas, board_x - 4, board_y - 4,
                  BOARD_W * CELL_PX + 8, BOARD_H * CELL_PX + 8, cell_edge);

    for (int y = 0; y < BOARD_H; y++)
        for (int x = 0; x < BOARD_W; x++)
            if (board[y][x])
                draw_cell(x, y, piece_colours[board[y][x]]);
            else
                gfx_fill_rect(&canvas, board_x + x * CELL_PX,
                              board_y + y * CELL_PX, CELL_PX - 1, CELL_PX - 1,
                              background);

    /* Ghost */
    if (!game_over) {
        int gy = py;

        while (!collides(piece, rot, px, gy + 1))
            gy++;
        int cx[4], cy[4];

        piece_cells(piece, rot, px, gy, cx, cy);
        for (int i = 0; i < 4; i++)
            if (cy[i] >= 0)
                gfx_fill_rect(&canvas, board_x + cx[i] * CELL_PX,
                              board_y + cy[i] * CELL_PX, CELL_PX - 1,
                              CELL_PX - 1, ghost_colour);

        piece_cells(piece, rot, px, py, cx, cy);
        for (int i = 0; i < 4; i++)
            if (cy[i] >= 0)
                draw_cell(cx[i], cy[i], piece_colours[piece]);
    }

    /* Panel */
    int cell = 18;
    int y = board_y;

    draw_text(panel_x, y, "HOLD", 2, 0xFFFFFF);
    y += 26;
    gfx_fill_rect(&canvas, panel_x - 4, y - 4, 4 * cell + 8, 3 * cell + 8,
                  cell_edge);
    if (hold_piece)
        draw_piece_preview(hold_piece, panel_x + cell / 2, y, cell);
    y += 3 * cell + 24;

    draw_text(panel_x, y, "NEXT", 2, 0xFFFFFF);
    y += 26;
    for (int i = 0; i < NEXT_COUNT; i++) {
        gfx_fill_rect(&canvas, panel_x - 4, y - 4, 4 * cell + 8,
                      3 * cell + 8, cell_edge);
        draw_piece_preview(next_queue[i], panel_x + cell / 2, y, cell);
        y += 3 * cell + 18;
    }

    y = board_y + BOARD_H * CELL_PX - 90;
    draw_text(panel_x, y, "SCORE", 2, 0xAAAAAA);
    draw_number(panel_x, y + 18, score, 2, 0xFFFFFF);
    y += 54;
    draw_text(panel_x, y, "LEVEL", 2, 0xAAAAAA);
    draw_number(panel_x, y + 18, level, 2, 0xFFFFFF);
    y += 54;
    draw_text(panel_x, y, "LINES", 2, 0xAAAAAA);
    draw_number(panel_x, y + 18, lines, 2, 0xFFFFFF);

    if (paused)
        draw_text(board_x + 30, board_y + 260, "PAUSED", 5, 0xFFFFFF);
    if (game_over) {
        draw_text(board_x + 20, board_y + 240, "GAMEOVER", 4, 0xFF4040);
        draw_text(board_x + 30, board_y + 290, "Q QUITS", 3, 0xFFFFFF);
    }

    gfx_present(&canvas);
}

/* ------------------------------------------------------------ game logic --- */

static uint32_t rng_state = 0x12345678;

static uint32_t rng_next(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static int bag[7];
static int bag_left;

static int next_piece(void)
{
    if (bag_left == 0) {
        for (int i = 0; i < 7; i++)
            bag[i] = i + 1;
        for (int i = 6; i > 0; i--) {
            int j = (int)(rng_next() % (uint32_t)(i + 1));
            int t = bag[i];

            bag[i] = bag[j];
            bag[j] = t;
        }
        bag_left = 7;
    }
    return bag[--bag_left];
}

static void refill_next(void)
{
    for (int i = 0; i < NEXT_COUNT; i++)
        if (!next_queue[i])
            next_queue[i] = next_piece();
}

static bool spawn_piece(int type)
{
    piece = type;
    rot = 0;
    px = 3;
    py = type == P_I ? -1 : 0;

    if (collides(piece, rot, px, py))
        return false;

    last_was_rotation = false;
    grounded = false;
    lock_timer = 0;
    lock_resets = 0;
    gravity_timer = 0;
    return true;
}

static void set_grounded(void)
{
    bool now = collides(piece, rot, px, py + 1);

    if (now && !grounded)
        lock_timer = 0;
    grounded = now;
}

static bool move_piece(int dx, int dy)
{
    if (collides(piece, rot, px + dx, py + dy))
        return false;
    px += dx;
    py += dy;
    last_was_rotation = false;
    set_grounded();
    return true;
}

/* Try a rotation with the TETR.IO kick table. Returns the kick index used, or
 * -1 on failure; a 180 is reported as index 5. */
static int rotate_piece(int delta)
{
    int to = (rot + delta) & 3;
    const struct kick *list = NULL;
    int steps = 5;

    if (delta == 2) {
        list = &kicks_180[rot][to][0];
        steps = 6;
    } else if (piece == P_I) {
        list = &kicks_i[rot][to][0];
    } else if (piece != P_O) {
        list = &kicks_jlstz[rot][to][0];
    }

    if (piece == P_O) {
        /* O never changes shape; rotate in place without a kick search. */
        rot = to;
        last_was_rotation = true;
        return 0;
    }

    for (int i = 0; i < steps; i++) {
        int kx = list[i].x;
        int ky = list[i].y;

        if (!collides(piece, to, px + kx, py - ky)) {
            px += kx;
            py -= ky;
            rot = to;
            last_was_rotation = true;
            if (grounded && lock_resets < LOCK_RESET_LIMIT) {
                lock_timer = 0;
                lock_resets++;
            }
            set_grounded();
            return delta == 2 ? 5 : i;
        }
    }
    return -1;
}

static void lock_piece(void)
{
    int cx[4], cy[4];
    bool is_spin = last_was_rotation && immobile();
    bool is_t = piece == P_T;
    bool mini = false;

    piece_cells(piece, rot, px, py, cx, cy);
    for (int i = 0; i < 4; i++)
        if (cy[i] >= 0)
            board[cy[i]][cx[i]] = (uint8_t)piece;

    if (is_spin && is_t) {
        bool front_pair = false;
        int corners = t_corners(&front_pair);

        if (corners < 3)
            is_spin = false;
        else
            mini = !front_pair;
    }

    /* Clear lines. */
    int cleared = 0;

    for (int y = BOARD_H - 1; y >= 0; y--) {
        bool full = true;

        for (int x = 0; x < BOARD_W; x++)
            if (!board[y][x]) {
                full = false;
                break;
            }
        if (!full)
            continue;
        cleared++;
        for (int yy = y; yy > 0; yy--)
            memcpy(board[yy], board[yy - 1], BOARD_W);
        memset(board[0], 0, BOARD_W);
        y++;      /* recheck this row after the shift */
    }

    int gained = 0;

    if (cleared > 0 || is_spin) {
        int base = is_spin ? spin_value(cleared, mini, is_t)
                           : line_value(cleared);
        bool b2b_clear = cleared == 4 || (is_spin && cleared > 0);

        if (b2b_clear && back_to_back)
            base = base * 3 / 2;
        if (b2b_clear)
            back_to_back = true;
        else if (cleared > 0)
            back_to_back = false;

        if (cleared > 0) {
            combo++;
            if (combo > 1)
                base += 50 * (combo - 1);
        } else {
            combo = 0;
        }

        gained = base * (level + 1);
        score += gained;
        lines += cleared;
        level = lines / LINES_PER_LEVEL;
    }

    hold_used = false;
    refill_next();

    if (!spawn_piece(next_queue[0])) {
        game_over = true;
        return;
    }
    for (int i = 0; i < NEXT_COUNT - 1; i++)
        next_queue[i] = next_queue[i + 1];
    next_queue[NEXT_COUNT - 1] = 0;
    refill_next();
}

static void hard_drop(void)
{
    int dropped = 0;

    while (!collides(piece, rot, px, py + 1)) {
        py++;
        dropped++;
    }
    score += dropped * 2;
    lock_piece();
}

static void hold_piece_action(void)
{
    if (hold_used || game_over)
        return;

    int old = hold_piece;

    hold_piece = piece;
    if (old) {
        if (!spawn_piece(old))
            game_over = true;
    } else {
        if (!spawn_piece(next_queue[0]))
            game_over = true;
        else {
            for (int i = 0; i < NEXT_COUNT - 1; i++)
                next_queue[i] = next_queue[i + 1];
            next_queue[NEXT_COUNT - 1] = 0;
            refill_next();
        }
    }
    hold_used = true;
}

static int gravity_interval(void)
{
    int lv = level;

    if (lv >= GRAVITY_LEVELS)
        lv = GRAVITY_LEVELS - 1;
    return gravity_ms[lv];
}

/* ---------------------------------------------------------------- main ----- */

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(void)
{
    int old_flags;
    long last = now_ms();

    if (gfx_open(&canvas) < 0) {
        fprintf(stderr, "tetris: graphics unavailable (%d)\n", errno);
        return 1;
    }
    if (gfx_claim_screen() < 0) {
        fprintf(stderr, "tetris: could not claim screen (%d)\n", errno);
        gfx_close(&canvas);
        return 1;
    }

    background = gfx_rgb(&canvas, 14, 16, 22);
    cell_edge = gfx_rgb(&canvas, 55, 62, 78);
    ghost_colour = gfx_rgb(&canvas, 40, 46, 60);
    piece_colours[P_I] = gfx_rgb(&canvas, 60, 210, 220);
    piece_colours[P_O] = gfx_rgb(&canvas, 240, 210, 60);
    piece_colours[P_T] = gfx_rgb(&canvas, 180, 80, 220);
    piece_colours[P_S] = gfx_rgb(&canvas, 80, 210, 90);
    piece_colours[P_Z] = gfx_rgb(&canvas, 230, 70, 70);
    piece_colours[P_J] = gfx_rgb(&canvas, 70, 110, 230);
    piece_colours[P_L] = gfx_rgb(&canvas, 240, 150, 50);

    board_x = 40;
    board_y = (int)(canvas.height - BOARD_H * CELL_PX) / 2;
    panel_x = board_x + BOARD_W * CELL_PX + 40;

    rng_state ^= (uint32_t)now_ms();

    old_flags = fcntl(0, F_GETFL, 0);
    if (old_flags >= 0)
        fcntl(0, F_SETFL, old_flags | O_NONBLOCK);

    memset(board, 0, sizeof(board));
    refill_next();
    if (!spawn_piece(next_queue[0]))
        game_over = true;
    else {
        for (int i = 0; i < NEXT_COUNT - 1; i++)
            next_queue[i] = next_queue[i + 1];
        next_queue[NEXT_COUNT - 1] = 0;
        refill_next();
    }

    render();

    while (!quit_game) {
        long now = now_ms();
        int elapsed = (int)(now - last);

        last = now;

        if (!paused && !game_over) {
            uint64_t keys = 0;

            if (kbd_state(&keys) < 0)
                keys = 0;

            bool just_entered = false;

            if (keys && !ps2_mode) {
                /* First PS/2 press: this frame still goes through the
                 * character path, which already carries the press. Consume
                 * the mask edge so it is not acted on twice. */
                ps2_mode = true;
                keys_prev = keys;
                just_entered = true;
            }

            if (ps2_mode && !just_entered) {
                uint64_t edges;

                /* Actions come from the bitmap; discard the character events
                 * the make codes also produced. */
                while (read_key() >= 0)
                    ;

                edges = keys & ~keys_prev;

                if (edges & (1ULL << KBD_SC_A)) {
                    move_piece(-1, 0);
                    das_dir = -1;
                    das_timer = arr_timer = 0;
                }
                if (edges & (1ULL << KBD_SC_D)) {
                    move_piece(1, 0);
                    das_dir = 1;
                    das_timer = arr_timer = 0;
                }
                if (edges & (1ULL << KBD_SC_S)) {
                    if (move_piece(0, 1))
                        score += 1;
                    soft_timer = 0;
                }
                if (edges & (1ULL << KBD_SC_SPACE))
                    hard_drop();
                if (edges & (1ULL << KBD_SC_C))
                    hold_piece_action();
                if (edges & (1ULL << KBD_SC_COMMA))
                    rotate_piece(-1);
                if (edges & (1ULL << KBD_SC_DOT))
                    rotate_piece(1);
                if (edges & (1ULL << 0x21))          /* f */
                    rotate_piece(2);
                if (edges & (1ULL << 0x19))          /* p */
                    paused = !paused;
                if (edges & (1ULL << 0x10))          /* q */
                    quit_game = true;

                if (keys & (1ULL << KBD_SC_A)) {
                    if (das_dir == -1) {
                        das_timer += elapsed;
                        if (das_timer >= DAS_DELAY_MS) {
                            arr_timer += elapsed;
                            while (arr_timer >= DAS_REPEAT_MS) {
                                move_piece(-1, 0);
                                arr_timer -= DAS_REPEAT_MS;
                                dirty = true;
                            }
                        }
                    }
                } else if (das_dir == -1) {
                    das_dir = 0;
                }
                if (keys & (1ULL << KBD_SC_D)) {
                    if (das_dir == 1) {
                        das_timer += elapsed;
                        if (das_timer >= DAS_DELAY_MS) {
                            arr_timer += elapsed;
                            while (arr_timer >= DAS_REPEAT_MS) {
                                move_piece(1, 0);
                                arr_timer -= DAS_REPEAT_MS;
                                dirty = true;
                            }
                        }
                    }
                } else if (das_dir == 1) {
                    das_dir = 0;
                }
                if (keys & (1ULL << KBD_SC_S)) {
                    soft_timer += elapsed;
                    if (soft_timer >= SOFT_REPEAT_MS) {
                        soft_timer = 0;
                        if (move_piece(0, 1))
                            score += 1;
                        dirty = true;
                    }
                }

                if (edges)
                    dirty = true;
                keys_prev = keys;
            } else {
                /* Serial fallback: no key-up information, so each character
                 * is one action and the terminal's own repeat drives holds. */
                int c;

                while ((c = read_key()) >= 0) {
                    switch (c) {
                    case 'a': case 'A': move_piece(-1, 0); break;
                    case 'd': case 'D': move_piece(1, 0); break;
                    case 's': case 'S':
                        if (move_piece(0, 1))
                            score += 1;
                        gravity_timer = 0;
                        break;
                    case ' ': hard_drop(); break;
                    case 'c': case 'C': hold_piece_action(); break;
                    case ',': rotate_piece(-1); break;
                    case '.': rotate_piece(1); break;
                    case 'f': case 'F': rotate_piece(2); break;
                    case 'p': case 'P': paused = !paused; break;
                    case 'q': case 'Q': case 0x03: quit_game = true; break;
                    default: break;
                    }
                    dirty = true;
                }
            }

            /*
             * The transition frame took the character path, which moved once
             * but does not own DAS state. Seed the repeat state from the same
             * press so the hold actually repeats from here on.
             */
            if (just_entered) {
                if (keys & (1ULL << KBD_SC_A)) {
                    das_dir = -1;
                    das_timer = arr_timer = 0;
                } else if (keys & (1ULL << KBD_SC_D)) {
                    das_dir = 1;
                    das_timer = arr_timer = 0;
                }
                if (keys & (1ULL << KBD_SC_S))
                    soft_timer = 0;
            }

            /* Gravity and lock. */
            gravity_timer += elapsed;
            if (gravity_timer >= gravity_interval()) {
                gravity_timer = 0;
                if (move_piece(0, 1)) {
                    dirty = true;
                }
            }
            set_grounded();
            if (grounded) {
                lock_timer += elapsed;
                if (lock_timer >= LOCK_DELAY_MS) {
                    lock_piece();
                    dirty = true;
                }
            } else {
                lock_timer = 0;
            }
        } else if (game_over) {
            int c;

            while ((c = read_key()) >= 0)
                if (c == 'q' || c == 'Q' || c == 0x03)
                    quit_game = true;
        } else {
            int c;

            while ((c = read_key()) >= 0) {
                if (c == 'p' || c == 'P')
                    paused = false;
                else if (c == 'q' || c == 'Q' || c == 0x03)
                    quit_game = true;
            }
        }

        if (dirty) {
            render();
            dirty = false;
        }

        struct timespec ts = { 0, FRAME_MS * 1000000L };
        nanosleep(&ts, NULL);
    }

    gfx_claim_screen_release();
    gfx_close(&canvas);
    if (old_flags >= 0)
        fcntl(0, F_SETFL, old_flags);
    printf("tetris: score %d, lines %d, level %d\n", score, lines, level);
    return 0;
}