/*
 * snake.c — terminal Snake.
 *
 *   snake [-w width] [-H height] [-a apples]
 *   snake [width [height [apples]]]        (positional fallback)
 *
 * Defaults: 20 x 15 board, 3 apples on the board. The board is clamped to
 * TIOCGWINSZ. WASD and arrow keys steer; q quits.
 *
 * Win condition: fill the whole board with snake. Apples are food: eating one
 * grows the snake and a replacement is placed in a free cell until none are
 * left.
 *
 * Input is polled non-blocking and queued: a burst of turns between two ticks
 * is not lost, and each tick consumes one turn, so quick direction changes
 * behave in the order they were typed. Bytes are buffered too, so an escape
 * sequence split across two reads still parses.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <time.h>
#include <unistd.h>

enum { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT };
enum { KEY_QUEUE = 32, BYTE_QUEUE = 64 };

struct point {
    int x, y;
};

struct keys {
    unsigned char bytes[BYTE_QUEUE];   /* unparsed input, partial sequences */
    int nbytes;
    int dirs[KEY_QUEUE];               /* parsed directions, FIFO */
    int head, tail;
    int quit;
};

static int board_w = 20;
static int board_h = 15;
static int apple_count = 3;

static const int dx[4] = { 0, 0, -1, 1 };
static const int dy[4] = { -1, 1, 0, 0 };

static void usage(const char *argv0)
{
    printf("usage: %s [-w width] [-H height] [-a apples]\n", argv0);
    printf("       %s [width [height [apples]]]\n", argv0);
    printf("  defaults: 20x15 board, 3 apples; apples are placed together\n");
    printf("  controls: WASD or arrows, q quits; win by filling the board\n");
}

/* strtol with full validation, so bad input is a usage error rather than a
 * silently clamped zero. */
static int parse_int(const char *s, int min, int max, int *out)
{
    char *end;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || end == s || *end != '\0' || v < min || v > max)
        return 0;
    *out = (int)v;
    return 1;
}

static void keys_push_dir(struct keys *k, int dir)
{
    if (k->tail - k->head >= KEY_QUEUE)
        return;                         /* queue full: drop the newest */
    k->dirs[k->tail % KEY_QUEUE] = dir;
    k->tail++;
}

static int keys_pop_dir(struct keys *k)
{
    if (k->head == k->tail)
        return -1;
    int dir = k->dirs[k->head % KEY_QUEUE];

    k->head++;
    return dir;
}

static int char_dir(int c)
{
    switch (tolower((unsigned char)c)) {
    case 'w': return DIR_UP;
    case 's': return DIR_DOWN;
    case 'a': return DIR_LEFT;
    case 'd': return DIR_RIGHT;
    default:  return -1;
    }
}

/* Arrow keys send ESC [ A/B/C/D, where the letters are not the WASD ones. */
static int arrow_dir(int c)
{
    switch (c) {
    case 'A': return DIR_UP;
    case 'B': return DIR_DOWN;
    case 'C': return DIR_RIGHT;
    case 'D': return DIR_LEFT;
    default:  return -1;
    }
}

/* Read everything available, append it to the byte buffer, and parse every
 * complete key. Leftover bytes wait for the next call. */
static void keys_fill(struct keys *k)
{
    char buf[64];
    ssize_t n;

    while ((n = read(0, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n && k->nbytes < BYTE_QUEUE; i++)
            k->bytes[k->nbytes++] = (unsigned char)buf[i];
    }

    int i = 0;

    while (i < k->nbytes) {
        unsigned char c = k->bytes[i];

        if (c == 0x1b) {
            if (k->nbytes - i < 3)
                break;                  /* partial escape sequence */
            if (k->bytes[i + 1] == '[' || k->bytes[i + 1] == 'O') {
                int d = arrow_dir(k->bytes[i + 2]);

                if (d >= 0)
                    keys_push_dir(k, d);
                else if (k->bytes[i + 2] == 'q' ||
                         k->bytes[i + 2] == 'Q')
                    k->quit = 1;
                i += 3;
                continue;
            }
            i++;
            continue;
        }

        if (tolower(c) == 'q') {
            k->quit = 1;
        } else {
            int d = char_dir(c);

            if (d >= 0)
                keys_push_dir(k, d);
        }
        i++;
    }

    if (i > 0) {
        memmove(k->bytes, k->bytes + i, (size_t)(k->nbytes - i));
        k->nbytes -= i;
    }
}

/*
 * Body colour palette, darkest (oldest) to brightest (newest).
 *
 * Cells leave from the tail, so the tail is what a player needs to read: the
 * segments nearest the tail are the ones that disappear next.
 *
 * A segment is one palette entry darker than the one ahead of it, always. The
 * *range* of the palette in use grows with the snake: a 3-cell snake uses the
 * three brightest entries, a 24-cell snake the whole list, and past that the
 * window stays at 24 entries sitting on the tail, with the rest of the body at
 * the brightest entry. That keeps two properties:
 *
 *   - adjacent segments never differ by more than one palette step, so a short
 *     snake is not a violent dark-to-bright jump;
 *   - the step never shrinks either. Stretching a fixed palette over an
 *     arbitrarily long snake is what made the gradient fade to nothing on a
 *     large board; here the contrast per segment is constant however long the
 *     snake gets.
 *
 * The window is anchored at the tail rather than at the head so the detailed
 * ramp is on the cells that are about to vanish.
 */
#define PALETTE_N 24

static const unsigned char palette[PALETTE_N][3] = {
    {  10,  40,  18 },
    {  15,  47,  23 },
    {  19,  54,  28 },
    {  24,  61,  32 },
    {  29,  68,  37 },
    {  33,  75,  42 },
    {  38,  82,  47 },
    {  43,  89,  51 },
    {  48,  96,  56 },
    {  52, 103,  61 },
    {  57, 110,  66 },
    {  62, 117,  71 },
    {  66, 123,  75 },
    {  71, 130,  80 },
    {  76, 137,  85 },
    {  80, 144,  90 },
    {  85, 151,  95 },
    {  90, 158,  99 },
    {  95, 165, 104 },
    {  99, 172, 109 },
    { 104, 179, 114 },
    { 109, 186, 118 },
    { 113, 193, 123 },
    { 118, 200, 128 },
};

/* Palette entry for snake[index], where 0 is the head. */
static int segment_palette(int index, int len)
{
    int from_tail = len - 1 - index;      /* 0 = tail (oldest) */
    int window = len < PALETTE_N ? len : PALETTE_N;
    int base = PALETTE_N - window;        /* darker end of the slice */
    int pos = from_tail < window ? from_tail : window - 1;

    return base + pos;
}

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
            int seg = -1;
            bool apple_here = false;

            for (int i = 0; i < n_apples; i++)
                if (apples[i].x == x && apples[i].y == y) {
                    apple_here = true;
                    break;
                }
            if (!apple_here) {
                for (int i = 0; i < len; i++)
                    if (body[i].x == x && body[i].y == y) {
                        seg = i;
                        break;
                    }
            }

            if (seg >= 0) {
                const unsigned char *c = palette[segment_palette(seg, len)];

                printf("\033[38;2;%d;%d;%dm%c\033[0m",
                       c[0], c[1], c[2], seg == 0 ? '@' : '#');
            } else if (apple_here) {
                fputs("\033[1;31mo\033[0m", stdout);
            } else {
                putchar(' ');
            }
        }
        fputs("\033[34m|\033[0m\n", stdout);
    }

    fputs("\033[34m+", stdout);
    for (int x = 0; x < board_w; x++)
        putchar('-');
    fputs("+\033[0m\n", stdout);

    char score[160];

    snprintf(score, sizeof(score),
             "\033[33m board %dx%d  on board %d  eaten %d  length %d/%d  "
             "tick %dms  [WASD/arrows, q]\033[0m\033[K\n",
             board_w, board_h, n_apples, eaten, len, board_w * board_h,
             tick_ms);
    fputs(score, stdout);
    fflush(stdout);
}

/* Place one apple in a free cell, excluding the snake and the other apples. */
static int spawn_apple(struct point *out, const struct point *body, int len,
                       const struct point *apples, int n_apples)
{
    int area = board_w * board_h;
    unsigned char *used = calloc((size_t)area, 1);
    struct point *free_cells = malloc(sizeof(*free_cells) * (size_t)area);
    int nfree = 0;

    if (!used || !free_cells) {
        free(used);
        free(free_cells);
        return 0;
    }
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

static int opposite(int a, int b)
{
    return (a == DIR_UP && b == DIR_DOWN) ||
           (a == DIR_DOWN && b == DIR_UP) ||
           (a == DIR_LEFT && b == DIR_RIGHT) ||
           (a == DIR_RIGHT && b == DIR_LEFT);
}

int main(int argc, char **argv)
{
    static const struct option longopts[] = {
        { "width",  required_argument, NULL, 'w' },
        { "height", required_argument, NULL, 'H' },
        { "apples", required_argument, NULL, 'a' },
        { "help",   no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    int req_w = 20, req_h = 15, req_apples = 3;
    int have_w = 0, have_h = 0, have_a = 0;
    int max_w = 78, max_h = 21;
    struct winsize ws;
    int opt;

    while ((opt = getopt_long(argc, argv, "w:H:a:h", longopts, NULL)) != -1) {
        switch (opt) {
        case 'w':
            if (!parse_int(optarg, 5, 1000, &req_w)) {
                usage(argv[0]);
                return 2;
            }
            have_w = 1;
            break;
        case 'H':
            if (!parse_int(optarg, 5, 1000, &req_h)) {
                usage(argv[0]);
                return 2;
            }
            have_h = 1;
            break;
        case 'a':
            if (!parse_int(optarg, 1, 1000, &req_apples)) {
                usage(argv[0]);
                return 2;
            }
            have_a = 1;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 2;
        }
    }

    /* Positional fallback, so `run snake 10 8 5` keeps working. */
    int pos = optind;

    if (!have_w && pos < argc) {
        if (!parse_int(argv[pos++], 5, 1000, &req_w)) {
            usage(argv[0]);
            return 2;
        }
        have_w = 1;
    }
    if (!have_h && pos < argc) {
        if (!parse_int(argv[pos++], 5, 1000, &req_h)) {
            usage(argv[0]);
            return 2;
        }
        have_h = 1;
    }
    if (!have_a && pos < argc) {
        if (!parse_int(argv[pos++], 1, 1000, &req_apples)) {
            usage(argv[0]);
            return 2;
        }
        have_a = 1;
    }
    if (pos < argc) {
        usage(argv[0]);
        return 2;
    }

    if (ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row) {
        max_w = (int)ws.ws_col - 2;
        max_h = (int)ws.ws_row - 4;
    }

    board_w = MAX(5, MIN(req_w, max_w));
    board_h = MAX(5, MIN(req_h, max_h));

    int area = board_w * board_h;

    apple_count = MAX(1, MIN(req_apples, area - 3));

    struct point *body = calloc((size_t)area, sizeof(*body));
    struct point *apples = calloc((size_t)apple_count, sizeof(*apples));
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

    /* Animate while polling. fd 0 is shared with the REPL, so restore its
     * flags before returning or the prompt would spin on empty reads. */
    int old_flags = fcntl(0, F_GETFL, 0);
    if (old_flags >= 0)
        fcntl(0, F_SETFL, old_flags | O_NONBLOCK);

    fputs("\033[2J\033[?25l", stdout);

    struct keys keys = { 0 };
    int dir = DIR_RIGHT, next_dir = DIR_RIGHT;
    int won = 0, lost = 0;

    for (;;) {
        keys_fill(&keys);
        if (keys.quit)
            break;

        int d = keys_pop_dir(&keys);

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

            if (len >= area) {
                won = 1;
                break;
            }
            if (n_apples < apple_count &&
                spawn_apple(&apples[n_apples], body, len, apples, n_apples))
                n_apples++;
        }

        /* Gentle ramp: one millisecond faster per two apples, from 150 ms
         * down to a floor of 80 ms. The old curve lost 2 ms per apple and hit
         * 60 ms after 40 apples, which made the mid-game unplayable quickly. */
        int tick_ms = MAX(80, 150 - eaten / 2);

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

    if (old_flags >= 0)
        fcntl(0, F_SETFL, old_flags);
    fputs("\033[0m\033[?25h\n", stdout);
    fflush(stdout);
    free(body);
    free(apples);
    return won ? 0 : (lost ? 1 : 0);
}
