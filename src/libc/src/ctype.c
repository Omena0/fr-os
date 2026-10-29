/*
 * ctype.c — ASCII character classes.
 *
 * Every classification is a range test on the byte value. A negative int (the
 * usual EOF/-1 case) is not a member of any class, and tolower/toupper return
 * it unchanged.
 */
#include <ctype.h>

static int in_range(int c, int lo, int hi)
{
    return c >= lo && c <= hi;
}

int isdigit(int c)
{
    return in_range(c, '0', '9');
}

int isupper(int c)
{
    return in_range(c, 'A', 'Z');
}

int islower(int c)
{
    return in_range(c, 'a', 'z');
}

int isalpha(int c)
{
    return isupper(c) || islower(c);
}

int isalnum(int c)
{
    return isalpha(c) || isdigit(c);
}

int isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' ||
           c == '\f' || c == '\r';
}

int isblank(int c)
{
    return c == ' ' || c == '\t';
}

int iscntrl(int c)
{
    return in_range(c, 0x00, 0x1f) || c == 0x7f;
}

int isprint(int c)
{
    return in_range(c, 0x20, 0x7e);
}

int isgraph(int c)
{
    return in_range(c, 0x21, 0x7e);
}

int ispunct(int c)
{
    return isgraph(c) && !isalnum(c);
}

int isxdigit(int c)
{
    return isdigit(c) || in_range(c, 'a', 'f') || in_range(c, 'A', 'F');
}

int tolower(int c)
{
    return isupper(c) ? c + ('a' - 'A') : c;
}

int toupper(int c)
{
    return islower(c) ? c - ('a' - 'A') : c;
}

int isascii(int c)
{
    return in_range(c, 0x00, 0x7f);
}

int toascii(int c)
{
    return c & 0x7f;
}
