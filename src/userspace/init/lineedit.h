/* lineedit.h — a small raw-terminal line editor. */
#ifndef LINEEDIT_H
#define LINEEDIT_H

#include <stddef.h>

/*
 * Read and edit one line into `line` (capacity `max`, NUL-terminated).
 * Echoes, draws a block cursor, supports insertion, Backspace,
 * Ctrl+W / Ctrl+Backspace word delete, Left/Right, Home/End and Delete.
 * Returns the line length, or -1 on EOF.
 */
int line_edit(const char *prompt, char *line, size_t max);

#endif /* LINEEDIT_H */
