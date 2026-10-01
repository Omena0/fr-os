/*
 * stdio.h — buffered stdio.
 *
 * The buffering model is the classic one: each FILE owns a buffer and an
 * fd. A write goes into the buffer and is flushed to the fd when the buffer
 * fills, when a newline is seen on a line-buffered stream, or on fflush.
 * A read pulls a buffer's worth from the fd at a time. stderr is unbuffered
 * by default; stdout is line-buffered when it refers to a tty.
 */
#ifndef STDIO_H
#define STDIO_H

#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

#define _IOFBF  0
#define _IOLBF  1
#define _IONBF  2

#ifndef BUFSIZ
#define BUFSIZ 4096
#endif

#define EOF       (-1)
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

#define FREAD   0x01
#define FWRITE  0x02
#define FAPPEND 0x04

typedef struct _IO_FILE FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

extern FILE *fopen(const char *path, const char *mode);
extern int fclose(FILE *stream);
extern int fflush(FILE *stream);
extern int fputc(int c, FILE *stream);
extern int putc(int c, FILE *stream);
extern int putchar(int c);
extern int fputs(const char *s, FILE *stream);
extern int puts(const char *s);
extern int ferror(FILE *stream);
extern int feof(FILE *stream);
extern void clearerr(FILE *stream);

extern size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
extern size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
extern char *fgets(char *s, int size, FILE *stream);
extern int getline(char **lineptr, size_t *n, FILE *stream);
extern int getc(FILE *stream);
extern int getchar(void);
extern int ungetc(int c, FILE *stream);
extern void setbuf(FILE *stream, char *buf);
extern int setvbuf(FILE *stream, char *buf, int mode, size_t size);

extern int fprintf(FILE *stream, const char *format, ...);
extern int printf(const char *format, ...);
extern int vfprintf(FILE *stream, const char *format, va_list ap);
extern int vprintf(const char *format, va_list ap);

extern int snprintf(char *str, size_t size, const char *format, ...);
extern int sprintf(char *str, const char *format, ...);
extern int vsnprintf(char *str, size_t size, const char *format, va_list ap);
extern int vsprintf(char *str, const char *format, va_list ap);

extern int perror(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* STDIO_H */