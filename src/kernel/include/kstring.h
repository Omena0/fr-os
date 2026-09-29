/*
 * kstring.h — freestanding string and memory primitives.
 *
 * The kernel links with -nostdlib, so there is no libc. These are the only
 * implementations, and they are tuned for the host CPU rather than being
 * portable C: the AVX2 path below is selected at build time because the kernel
 * targets one specific CPU and the presence of AVX2 is confirmed by CPUID
 * before anything calls it.
 *
 * The compiler can also synthesise calls to these (for struct assignment,
 * array initialisation, and so on), so they must stay externally visible with
 * exactly these names.
 */
#ifndef KSTRING_H
#define KSTRING_H

#include <types.h>

void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *haystack, const char *needle);

/* Bounded copy with an explicit size, returning bytes written or -1 if the
 * source did not fit. Used for all copies into fixed-size kernel buffers. */
ksize_t strlcpy(char *dst, const char *src, size_t size);
ksize_t strlcat(char *dst, const char *src, size_t size);

/* Case-insensitive comparison, ASCII only. */
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);

/* Number parsing. Return the number of characters consumed, or 0 on
 * malformed input; *out receives the value only when the return is non-zero. */
ksize_t kstrtoul(const char *s, unsigned long *out, int base);
ksize_t kstrtoull(const char *s, unsigned long long *out, int base);

/* Zeroing a large region. Implemented with non-temporal stores above a
 * threshold, because a page that is about to be handed to another CPU has no
 * business sitting dirty in cache. */
void bzero(void *dst, size_t n);

#endif /* KSTRING_H */
