/*
 * ansi2.c — Userland ANSI escape sequence smoke test - cursor and style box.
 *
 * Prints a small cursor-operation box that exercises SGR styles,
 * \r and \t plus escape cursor moves - without touching the matrix area.
 * Each box prints a specific legible string; legibility = pass.
 */
#include <stdio.h>

static void box(void)
{
    printf("\n");
    printf("  CURSOR/STYLE BOX (each line a separate check, legible = pass)\n");
    printf("  -----------------------------------------------------------\n");

    /* Bold + underline across the line. */
    printf("\033[1;4mBOLD+UNDERLINE\033[0m \033[0mplain\n");

    /* Inverse + blink. */
    printf("\033[7;5mINVERSE+BLINK\033[0m \033[0mplain\n");

    /* Strikethrough + italic. */
    printf("\033[9;3mSTRIKE+ITALIC\033[0m \033[0mplain\n");

    /* Hidden text should not print anything visible (only the label). */
    printf("HIDDEN:\033[8mSECRET\033[0m (nothing between colon and newline means pass)\n");

    /* Carriage return: overwrite line start. */
    printf("CARRIAGE_RETURN_START\033[0m");
    printf("\rCARRIAGE_RETURN_OVERWRITE\033[0m\n");

    /* Tab: advance to next tab stop. */
    printf("TAB_BEFORE\033[0m\tTAB_AFTER\033[0m\n");

    /* Cursor move with SGR: move to column 1, bold text. */
    /* Save the cursor, exercise an absolute move, then put it back: a test
     * that leaves the terminal cursor at the top of the screen makes the next
     * prompt overwrite earlier output. The console implements DECSC/DECRC
     * (ESC[s / ESC[u]) so the framebuffer and the serial terminal agree. */
    printf("\033[s\033[1;1H\033[1mCURSOR_MOVE_BOLD\033[0m\033[u");

    /* Dim then normal. */
    printf("\033[2mDIM_TEXT\033[22m normal_after_dim\033[0m\n");

    /* Blink then off. */
    printf("\033[5mBLINK_TEXT\033[25m blink_off\033[0m\n");

    /* Inverse then off. */
    printf("\033[7mINVERSE_TEXT\033[27m inverse_off\033[0m\n");

    printf("\n");
    printf("  ANSI test complete. Mark cells that look wrong against their row/col.\n");
    printf("\n");
}

int main(void)
{
    box();
    return 0;
}