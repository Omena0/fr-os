/*
 * ctype.h — ASCII character classification and case conversion.
 *
 * The values are the ASCII ones, not locale-dependent. There is no locale in
 * this libc; returning a class for a byte above 0x7f would be pretending there
 * is one.
 */
#ifndef CTYPE_H
#define CTYPE_H

#ifdef __cplusplus
extern "C" {
#endif

int isalnum(int c);
int isalpha(int c);
int isblank(int c);
int iscntrl(int c);
int isdigit(int c);
int isgraph(int c);
int islower(int c);
int isprint(int c);
int ispunct(int c);
int isspace(int c);
int isupper(int c);
int isxdigit(int c);

int tolower(int c);
int toupper(int c);

int isascii(int c);
int toascii(int c);

#ifdef __cplusplus
}
#endif

#endif /* CTYPE_H */
