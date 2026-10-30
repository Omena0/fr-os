/*
 * lineedit.c — a small raw-terminal line editor.
 *
 * The terminal has no line discipline: read() returns raw bytes. This editor
 * owns echo, a block cursor, insertion in the middle, Backspace, Ctrl+W /
 * Ctrl+Backspace word delete, Left/Right, Home/End and Delete.
 *
 * The block cursor is an inverse-video character rather than the hardware
 * cursor because the framebuffer console has no visible cursor. The real
 * terminal cursor is hidden while editing so the serial and framebuffer
 * backends look the same.
 *
 * Kept separate from init so a future shell, debugger or other interactive
 * program can link the same editor.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "lineedit.h"

static void line_draw_plain(const char *prompt, const char *line, size_t len)
{
    fputs("\r", stdout);
    fputs(prompt, stdout);
    fwrite(line, 1, len, stdout);
    fputs("\033[K", stdout);
    fflush(stdout);
}

static void line_redraw(const char *prompt, const char *line, size_t len,
                        size_t pos)
{
    fputs("\r", stdout);
    fputs(prompt, stdout);
    for (size_t i = 0; i < len; i++) {
        if (i == pos)
            printf("\033[7m%c\033[0m", line[i]);
        else
            putchar(line[i]);
    }
    if (pos == len)
        fputs("\033[7m \033[0m", stdout);
    fputs("\033[K", stdout);
    fflush(stdout);
}

static void line_delete(char *line, size_t *len, size_t *pos,
                        size_t from, size_t to)
{
    if (to > *len)
        to = *len;
    if (from >= to)
        return;
    memmove(line + from, line + to, *len - to);
    *len -= to - from;
    line[*len] = '\0';
    if (*pos >= to)
        *pos -= to - from;
    else if (*pos > from)
        *pos = from;
}

static int read_byte(void)
{
    unsigned char c;

    return read(0, &c, 1) == 1 ? (int)c : -1;
}

/* Consume a CSI sequence (after ESC [). Returns false on EOF. */
static bool handle_csi(char *line, size_t *len, size_t *pos,
                       const char *prompt)
{
    int params[4] = { 0, 0, 0, 0 };
    int np = 0;
    int v = 0;
    bool have = false;

    for (;;) {
        int c = read_byte();

        if (c < 0)
            return false;
        if (c >= '0' && c <= '9') {
            v = v * 10 + (c - '0');
            have = true;
            continue;
        }
        if (c == ';') {
            if (np < 4)
                params[np++] = have ? v : 0;
            v = 0;
            have = false;
            continue;
        }
        if (np < 4)
            params[np++] = have ? v : 0;

        if (c == '~') {
            int p = params[0];

            if (p == 3 && *pos < *len) {            /* Delete */
                line_delete(line, len, pos, *pos, *pos + 1);
                line_redraw(prompt, line, *len, *pos);
            } else if (p == 1) {                    /* Home */
                *pos = 0;
                line_redraw(prompt, line, *len, *pos);
            } else if (p == 4) {                    /* End */
                *pos = *len;
                line_redraw(prompt, line, *len, *pos);
            } else if ((p == 127 || p == 8) && np >= 2 && params[1] == 5) {
                /* Ctrl+Backspace in modifyOtherKeys form. */
                size_t from = *pos;

                while (from > 0 && line[from - 1] == ' ')
                    from--;
                while (from > 0 && line[from - 1] != ' ')
                    from--;
                line_delete(line, len, pos, from, *pos);
                line_redraw(prompt, line, *len, *pos);
            }
        } else if (c == 'A' || c == 'B') {
            /* No history. */
        } else if (c == 'C') {
            if (*pos < *len) {
                (*pos)++;
                line_redraw(prompt, line, *len, *pos);
            }
        } else if (c == 'D') {
            if (*pos > 0) {
                (*pos)--;
                line_redraw(prompt, line, *len, *pos);
            }
        } else if (c == 'H') {
            *pos = 0;
            line_redraw(prompt, line, *len, *pos);
        } else if (c == 'F') {
            *pos = *len;
            line_redraw(prompt, line, *len, *pos);
        }
        return true;
    }
}

int line_edit(const char *prompt, char *line, size_t max)
{
    size_t len = 0, pos = 0;

    line[0] = '\0';
    fputs("\033[?25l", stdout);
    line_redraw(prompt, line, len, pos);

    for (;;) {
        int c = read_byte();

        if (c < 0) {
            line_draw_plain(prompt, line, len);
            fputs("\033[0m\033[?25h\n", stdout);
            fflush(stdout);
            return -1;
        }
        if (c == '\r' || c == '\n') {
            line_draw_plain(prompt, line, len);
            fputs("\033[0m\033[?25h\n", stdout);
            fflush(stdout);
            return (int)len;
        }
        if (c == '\b' || c == 0x7f) {                /* Backspace */
            if (pos > 0) {
                size_t from = pos - 1;
                /* Back up over continuation bytes (0x80-0xBF) to find
                 * the start of the UTF-8 character (the lead byte, which
                 * is anything else). Deleting byte by byte would leave a
                 * dangling lead byte that renders as U+FFFD. */
                while (from > 0 && (line[from] & 0xC0) == 0x80)
                    from--;
                line_delete(line, &len, &pos, from, pos);
                line_redraw(prompt, line, len, pos);
            }
            continue;
        }
        if (c == 0x17) {                             /* Ctrl+W / Ctrl+Backspace */
            size_t from = pos;

            while (from > 0 && line[from - 1] == ' ')
                from--;
            while (from > 0 && line[from - 1] != ' ')
                from--;
            line_delete(line, &len, &pos, from, pos);
            line_redraw(prompt, line, len, pos);
            continue;
        }
        if (c == 0x1b) {
            int c1 = read_byte();

            if (c1 < 0) {
                fputs("\033[0m\033[?25h\n", stdout);
                return -1;
            }
            if (c1 == '[') {
                if (!handle_csi(line, &len, &pos, prompt)) {
                    fputs("\033[0m\033[?25h\n", stdout);
                    return -1;
                }
            } else if (c1 == 'O') {
                int c2 = read_byte();

                if (c2 == 'C' && pos < len) {
                    pos++;
                    line_redraw(prompt, line, len, pos);
                } else if (c2 == 'D' && pos > 0) {
                    pos--;
                    line_redraw(prompt, line, len, pos);
                } else if (c2 == 'H') {
                    pos = 0;
                    line_redraw(prompt, line, len, pos);
                } else if (c2 == 'F') {
                    pos = len;
                    line_redraw(prompt, line, len, pos);
                }
            }
            continue;
        }
        if (c < 0x20)
            continue;
        if (len + 1 >= max)
            continue;
        memmove(line + pos + 1, line + pos, len - pos);
        line[pos] = (char)c;
        len++;
        pos++;
        line[len] = '\0';
        line_redraw(prompt, line, len, pos);
    }
}
