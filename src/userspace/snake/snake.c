/*
 * snake.c — a small terminal Snake.
 *
 * Usage: snake [width] [height] [apples]
 *   width, height   playfield cells (default 20 x 15)
 *   apples          apples on the board at once (default 3)
 *
 * The board is clamped to the terminal reported by TIOCGWINSZ, so the border
 * and score line always fit. Input is polled non-blocking while the game
 * animates: the tty delivers serial keys and PS/2 keys into the same ring, and
 * arrow keys arrive as the usual ESC [ A/B/C/D sequences. WASD works too.
 *
 * Win condition: fill the whole board with snake. Apples are just the food;
 * eating one grows the snake by a cell and a new one is placed in a free cell
 * until none are left.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

enum { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT };

struct point {
    int x, y;
};

static int board_w = 20;
static int board_h = 15;
static int apple_count = 3;

static const int dx[4] = { 0, 0, -1, 1 };
static const int dy[4] = { -1, 1, 0, 0 };

static void render(const struct point *body, int len,
                   const struct point *apples, int n_apples,
                   int eaten, int tick_ms)
{
    fputs("\033[H\033[0m", stdout);

    fputs("\033[34m+", stdout);
    for (int x = 0; x < board_w; x++)
        putchar('-');
    fputs("+\033[0m\n", stdout);

    for (int y = 0; y < board_h; y++) {
        fputs("\033[34m|\033[0m", stdout);
        for (int x = 0; x < board_w; x++) {
            const char *colour = NULL;
            char ch = ' ';

            if (body[0].x == x && body[0].y == y) {
                ch = '@';
                colour = "\033[1;32m";
            } else {
                bool is_apple = false;

                for (int i = 0; i < n_apples; i++)
                    if (apples[i].x == x && apples[i].y == y) {
                        ch = 'o';
                        colour = "\033[1;31m";
                        is_apple = true;
                        break;
                    }
                if (is_apple) {
                    /* already chosen */
                } else {
                    for (int i = 1; i < len; i++)
                        if (body[i].x == x && body[i].y == y) {
                            ch = '#';
                            colour = "\033[32m";
                            break;
                        }
                }
            }
            if (colour)
                printf("%s%c\033[0m", colour, ch);
            else
                putchar(' ');
        }
        fputs("\033[34m|\033[0m\n", stdout);
    }

    fputs("\033[34m+", stdout);
    for (int x = 0; x < board_w; x++)
        putchar('-');
    fputs("+\033[0m\n", stdout);

    printf("\033[33m board %dx%d  on board %d  eaten %d  length %d/%d  "
           "tick %dms  [WASD/arrows, q]\033[0m\033[K\n",
           board_w, board_h, n_apples, eaten, len, board_w * board_h,
           tick_ms);
    fflush(stdout);
}

/*
 * Place one apple in a free cell. A cell is free if neither the snake nor an
 * existing apple occupies it. Returns 0 when the board has no free cell.
 */
static int spawn_apple(struct point *out, const struct point *body, int len,
                       const struct point *apples, int n_apples)
{
    int area = board_w * board_h;
    unsigned char *used = malloc((size_t)area);
    struct point *free_cells = malloc(sizeof(*free_cells) * (size_t)area);
    int nfree = 0;

    if (!used || !free_cells) {
        free(used);
        free(free_cells);
        return 0;
    }
    memset(used, 0, (size_t)area);
    for (int i = 0; i < len; i++)
        used[body[i].y * board_w + body[i].x] = 1;
    for (int i = 0; i < n_apples; i++)
        used[apples[i].y * board_w + apples[i].x] = 1;

    for (int y = 0; y < board_h; y++)
        for (int x = 0; x < board_w; x++)
            if (!used[y * board_w + x]) {
                free_cells[nfree].x = x;
                free_cells[nfree].y = y;
                nfree++;
            }

    if (nfree > 0)
        *out = free_cells[rand() % nfree];

    free(used);
    free(free_cells);
    return nfree > 0;
}

/*
 * Consume pending input and return the last direction seen, or -1. Bytes are
 * buffered across reads so an arrow sequence split between two reads is not
 * mistaken for a lone ESC.
 */
static int read_direction(int *quit)
{
    static unsigned char inbuf[32];
    static int inlen;
    char buf[64];
    ssize_t n;
    int dir = -1;

    while ((n = read(0, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n && inlen < (int)sizeof(inbuf); i++)
            inbuf[inlen++] = (unsigned char)buf[i];
    }

    int pos = 0;

    while (pos < inlen) {
        unsigned char c = inbuf[pos];

        if (c == 0x1b) {
            if (inlen - pos < 3)
                break;              /* wait for the rest of the sequence */
            if (inbuf[pos + 1] == '[' || inbuf[pos + 1] == 'O') {
                switch (inbuf[pos + 2]) {
                case 'A': dir = DIR_UP; break;
                case 'B': dir = DIR_DOWN; break;
                case 'C': dir = DIR_RIGHT; break;
                case 'D': dir = DIR_LEFT; break;
                default: break;
                }
                pos += 3;
                continue;
            }
            pos++;
            continue;
        }
        switch (c) {
        case 'w': case 'W': dir = DIR_UP; break;
        case 's': case 'S': dir = DIR_DOWN; break;
        case 'a': case 'A': dir = DIR_LEFT; break;
        case 'd': case 'D': dir = DIR_RIGHT; break;
        case 'q': case 'Q': *quit = 1; break;
        default: break;
        }
        pos++;
    }

    if (pos > 0) {
        memmove(inbuf, inbuf + pos, (size_t)(inlen - pos));
        inlen -= pos;
    }
    return dir;
}

static int opposite(int a, int b)
{
    return (a == DIR_UP && b == DIR_DOWN) ||
           (a == DIR_DOWN && b == DIR_UP) ||
           (a == DIR_LEFT && b == DIR_RIGHT) ||
           (a == DIR_RIGHT && b == DIR_LEFT);
}

int main(int argc, char **argv)
{
    int req_w = 20, req_h = 15, req_apples = 3;
    int max_w = 78, max_h = 21;
    struct winsize ws;

    if (argc > 1)
        req_w = atoi(argv[1]);
    if (argc > 2)
        req_h = atoi(argv[2]);
    if (argc > 3)
        req_apples = atoi(argv[3]);

    if (ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row) {
        max_w = (int)ws.ws_col - 2;
        max_h = (int)ws.ws_row - 4;
    }
    if (req_w < 5)
        req_w = 5;
    if (req_h < 5)
        req_h = 5;
    if (req_w > max_w)
        req_w = max_w;
    if (req_h > max_h)
        req_h = max_h;
    board_w = req_w;
    board_h = req_h;

    int area = board_w * board_h;
    if (req_apples < 1)
        req_apples = 1;
    if (req_apples > area - 3)
        req_apples = area - 3;
    apple_count = req_apples;

    struct point *body = malloc(sizeof(*body) * (size_t)area);
    struct point *apples = malloc(sizeof(*apples) * (size_t)apple_count);
    if (!body || !apples) {
        free(body);
        free(apples);
        fprintf(stderr, "snake: out of memory\n");
        return 1;
    }

    srand((unsigned)time(NULL) ^ (unsigned)(unsigned long)argv);
    int len = 3;
    int cx = board_w / 2;
    int cy = board_h / 2;
    body[0].x = cx;     body[0].y = cy;
    body[1].x = cx - 1; body[1].y = cy;
    body[2].x = cx - 2; body[2].y = cy;

    int n_apples = 0;
    int eaten = 0;
    for (int i = 0; i < apple_count; i++) {
        if (!spawn_apple(&apples[n_apples], body, len, apples, n_apples))
            break;
        n_apples++;
    }

    /*
     * Animate while polling: a blocking read would freeze the snake. The old
     * flags are restored before returning, because fd 0 is shared with the
     * parent REPL and O_NONBLOCK would otherwise make the prompt spin on
     * empty reads.
     */
    int old_flags = fcntl(0, F_GETFL, 0);
    if (old_flags >= 0)
        fcntl(0, F_SETFL, old_flags | O_NONBLOCK);

    fputs("\033[2J\033[?25l", stdout);

    int dir = DIR_RIGHT, next_dir = DIR_RIGHT;
    int won = 0, lost = 0, quit = 0;

    for (;;) {
        int d = read_direction(&quit);
        if (quit)
            break;
        if (d >= 0 && !opposite(d, dir))
            next_dir = d;
        dir = next_dir;

        int nx = body[0].x + dx[dir];
        int ny = body[0].y + dy[dir];

        if (nx < 0 || nx >= board_w || ny < 0 || ny >= board_h) {
            lost = 1;
            break;
        }

        int apple_at = -1;
        for (int i = 0; i < n_apples; i++)
            if (apples[i].x == nx && apples[i].y == ny) {
                apple_at = i;
                break;
            }
        int grow = apple_at >= 0;

        /* The tail moves out of the way unless the snake is growing. */
        int limit = grow ? len : len - 1;
        for (int i = 0; i < limit; i++)
            if (body[i].x == nx && body[i].y == ny) {
                lost = 1;
                break;
            }
        if (lost)
            break;

        int newlen = len + (grow ? 1 : 0);
        for (int i = newlen - 1; i > 0; i--)
            body[i] = body[i - 1];
        body[0].x = nx;
        body[0].y = ny;
        len = newlen;

        if (grow) {
            eaten++;
            apples[apple_at] = apples[--n_apples];

            /* The whole board filled with snake is the win. */
            if (len >= area) {
                won = 1;
                break;
            }
            if (n_apples < apple_count &&
                spawn_apple(&apples[n_apples], body, len, apples, n_apples))
                n_apples++;
        }

        int tick_ms = 160 - eaten;
        if (tick_ms < 60)
            tick_ms = 60;
        render(body, len, apples, n_apples, eaten, tick_ms);

        struct timespec ts = { 0, tick_ms * 1000000L };
        nanosleep(&ts, NULL);
    }

    render(body, len, apples, n_apples, eaten, 60);

    if (won)
        printf("\n\033[1;33m*** YOU WIN! the board is full (%d cells) ***\033[0m",
               len);
    else if (lost)
        printf("\n\033[1;31m*** GAME OVER: length %d/%d ***\033[0m",
               len, board_w * board_h);
    else
        printf("\n\033[0mbye (length %d/%d)", len, board_w * board_h);

    /* Leave the terminal and stdin usable for the REPL. */
    if (old_flags >= 0)
        fcntl(0, F_SETFL, old_flags);
    fputs("\033[0m\033[?25h\n", stdout);
    fflush(stdout);
    free(body);
    free(apples);
    return won ? 0 : (lost ? 1 : 0);
}
