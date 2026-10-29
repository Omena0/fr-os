/*
 * ansi.c — Fr Userland ANSI escape sequence smoke test.
 *
 * Prints a labeled color matrix: one row per ANSI SGR colour/attribute test,
 * each cell two characters wide and rendered in that colour, with row labels
 * on the left and column headers across the top so coordinates can be listed
 * against what is actually rendered (VS Code terminal vs QEMU serial).
 *
 * Below the matrix is a small cursor-operation box that exercises SGR styles,
 * \r and \t plus escape cursor moves — without touching the matrix area.
 * Each box prints a specific legible string; legibility = pass.
 *
 * Covered codes:
 *   SGR styles:    0 1 2 3 4 5 7 8 9  (reset,bold,dim,italic,underline,blink,inverse,hidden,strikethrough)
 *   FG colors:     30-37, 39, 90-97   (black..white + default + bright..bright white)
 *   BG colors:     40-47, 49, 100-107 (black..white + default + bright..bright white)
 *   256-colour:    38;5;n / 48;5;n    (grey ramp)
 *   RGB:           38;2;r;g;b / 48;2;r;g;b
 */
#include <stdio.h>
#include <string.h>

#define COL_W 2            /* each cell is two characters wide */
#define COL_COUNT 40       /* 40 cells * 2 chars = 80 columns */
#define ROW_HEADER_W 8     /* width of the row label column */

static void row_label(const char *tag)
{
    printf("%-8s ", tag);
}

static void col_header(int col)
{
    printf("%2d ", col % 100);
}

static void matrix_header(void)
{
    /* Top left blank header cell. */
    printf("%-8s ", "");
    for (int c = 0; c < COL_COUNT; c++)
        col_header(c);
    printf("\n");
}

static void separator_line(void)
{
    printf("%-8s ", "");
    for (int c = 0; c < COL_COUNT; c++)
        printf("---");
    printf("\n");
}

static void print_cell(const char *label, const char *sgr)
{
    printf("\033[%sm%-2s\033[0m ", sgr, label);
}

static void reset_line(void)
{
    printf("\033[0m\n");
}

/* ---------------------------------------------------------------- matrix --- */

static void matrix(void)
{
    const char *reset_cell = "";   /* marker, unused */

    matrix_header();
    separator_line();

    /* Row 0: style codes (A). */
    row_label("A");
    print_cell("00", "0");   /* reset */
    print_cell("01", "1");   /* bold */
    print_cell("02", "2");   /* dim */
    print_cell("03", "3");   /* italic */
    print_cell("04", "4");   /* underline */
    print_cell("05", "5");   /* blink */
    print_cell("07", "7");   /* inverse */
    print_cell("08", "8");   /* hidden */
    print_cell("09", "9");   /* strikethrough */
    reset_line();

    /* Row 1: foreground colors 30-37 + default 39 + bright 90-97 (B). */
    row_label("B");
    print_cell("30", "30");
    print_cell("31", "31");
    print_cell("32", "32");
    print_cell("33", "33");
    print_cell("34", "34");
    print_cell("35", "35");
    print_cell("36", "36");
    print_cell("37", "37");
    print_cell("dF", "39");
    print_cell("90", "90");
    print_cell("91", "91");
    print_cell("92", "92");
    print_cell("93", "93");
    print_cell("94", "94");
    print_cell("95", "95");
    print_cell("96", "96");
    print_cell("97", "97");
    reset_line();

    /* Row 2: background colors 40-47 + default 49 + bright 100-107 (C). */
    row_label("C");
    print_cell("40", "40");
    print_cell("41", "41");
    print_cell("42", "42");
    print_cell("43", "43");
    print_cell("44", "44");
    print_cell("45", "45");
    print_cell("46", "46");
    print_cell("47", "47");
    print_cell("dF", "49");
    print_cell("00", "100");
    print_cell("01", "101");
    print_cell("02", "102");
    print_cell("03", "103");
    print_cell("04", "104");
    print_cell("05", "105");
    print_cell("06", "106");
    print_cell("07", "107");
    reset_line();

    /* Row 3: combined style+fg+bg examples (D). */
    row_label("D");
    print_cell("01", "1;31");
    print_cell("04", "4;34");
    print_cell("07", "7;37;40");
    print_cell("01", "1;97;41");
    print_cell("05", "5;33");
    print_cell("09", "9;36");
    print_cell("ff", "ff");
    print_cell("00", "0");
    print_cell("00", "0");
    print_cell("00", "0");
    print_cell("00", "0");
    print_cell("00", "0");
    print_cell("00", "0");
    print_cell("00", "0");
    print_cell("00", "0");
    print_cell("00", "0");
    reset_line();

    /* Row 4: 256-colour foreground ramp (E). */
    row_label("E");
    for (int i = 0; i < COL_COUNT; i++) {
        char code[16];
        snprintf(code, sizeof(code), "38;5;%d", i % 256);
        char label[4];
        snprintf(label, sizeof(label), "%d", i % 10);
        print_cell(label, code);
    }
    reset_line();

    /* Row 5: 256-colour background ramp (F). */
    row_label("F");
    for (int i = 0; i < COL_COUNT; i++) {
        char code[16];
        snprintf(code, sizeof(code), "48;5;%d", i % 256);
        char label[4];
        snprintf(label, sizeof(label), "%d", i % 10);
        print_cell(label, code);
    }
    reset_line();

    /* Row 6: RGB foreground samples (G). */
    row_label("G");
    print_cell("r1", "38;2;255;0;0");
    print_cell("g1", "38;2;0;255;0");
    print_cell("b1", "38;2;0;0;255");
    print_cell("r2", "38;2;255;128;0");
    print_cell("g2", "38;2;0;255;128");
    print_cell("b2", "38;2;0;128;255");
    print_cell("c1", "38;2;128;0;255");
    print_cell("m1", "38;2;255;0;128");
    print_cell("w1", "38;2;255;255;255");
    print_cell("k1", "38;2;128;128;128");
    reset_line();

    /* Row 7: RGB background samples (H). */
    row_label("H");
    print_cell("r1", "48;2;255;0;0");
    print_cell("g1", "48;2;0;255;0");
    print_cell("b1", "48;2;0;0;255");
    print_cell("r2", "48;2;255;128;0");
    print_cell("g2", "48;2;0;255;128");
    print_cell("b2", "48;2;0;128;255");
    print_cell("c1", "48;2;128;0;255");
    print_cell("m1", "48;2;255;0;128");
    print_cell("w1", "48;2;255;255;255");
    print_cell("k1", "48;2;128;128;128");
    reset_line();
}

/* -------------------------------------------------------------- box test --- */

static void box(void)
{
    printf("\n\n\n\n");
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
    printf("\033[1;1H\033[1mCURSOR_MOVE_BOLD\033[0m\n");

    /* Dim then normal. */
    printf("\033[2mDIM_TEXT\033[22m normal_after_dim\033[0m\n");

    /* Blink then off. */
    printf("\033[5mBLINK_TEXT\033[25m blink_off\033[0m\n");

    /* Inverse then off. */
    printf("\033[7mINVERSE_TEXT\033[27m inverse_off\033[0m\n");

    printf("\n");
    printf("  ANSI test complete. Mark cells that look wrong against their row/col.\n");
}

int main(void)
{
    matrix();
    return 0;
}
