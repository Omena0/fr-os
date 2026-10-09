/*
 * stdlib.h — the public surface of the allocator and the rest of stdlib.
 *
 * The allocator is a segregated-list design with a per-thread free-list cache
 * and mmap-backed arenas. Anything larger than 128 KiB bypasses the arena
 * entirely and is handed out by mmap directly, so a single huge allocation
 * cannot fragment a shared arena. Freed memory is poisoned with 0xFE and the
 * chunk's magic is cleared, so a use-after-free reads as poison rather than as
 * stale data and a second free() of the same pointer is refused instead of
 * putting the block on a free list twice.
 */
#ifndef STDLIB_H
#define STDLIB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NULL ((void *)0)

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

#define RAND_MAX 2147483647

void *malloc(size_t size);
void  free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void *reallocarray(void *ptr, size_t nmemb, size_t size);
size_t malloc_usable_size(void *ptr);
int   malloc_trim(size_t pad);

/*
 * aligned_alloc and posix_memalign over-allocate and slide the returned address
 * up to the requested alignment, recording the block malloc() really returned
 * in the words immediately below the address they hand back. free(), realloc()
 * and malloc_usable_size() all recognise such an address and act on the block
 * underneath, so the C11 and POSIX requirement that free() accept the result
 * is met. See the shim comment in src/libc/src/malloc.c for how a plain malloc()
 * pointer is told apart from one of these, which is what keeps the recognition
 * from mistaking a normal block for an aligned one.
 *
 * Two allocator behaviours a caller can observe and cannot change:
 *
 *   - free() of a block over 128 KiB unmaps it, so a *second* free() of the
 *     same large pointer faults on the header read instead of calling abort().
 *     Both end the process; the fault is just the less legible of the two.
 *   - realloc() of an aligned address may return a different address. It has
 *     to: the replacement must land on an aligned address and the old block
 *     must be released through the shim.
 */
void *aligned_alloc(size_t alignment, size_t size);
int   posix_memalign(void **memptr, size_t alignment, size_t size);

void  exit(int status) __attribute__((noreturn));
void  _Exit(int status) __attribute__((noreturn));
void  abort(void) __attribute__((noreturn));
int   atexit(void (*func)(void));
int   __cxa_atexit(void (*func)(void), void *arg, void *dso_handle);

/*
 * The strtoxx family follows C99 7.20.1.4, and that is deliberate: base 0
 * makes "0b101" base 8, so it converts to 0 with endptr after the leading '0'.
 * C23 added a binary prefix and glibc accepts one, so a program ported from
 * there that passes "0b101" gets 0 rather than 5. Adding it would change a
 * result the standard this libc implements defines, so it is not added; a
 * caller that wants base 2 spells it out with base == 2 and a "0" of its own.
 */
int    atoi(const char *nptr);
long   atol(const char *nptr);
long long atoll(const char *nptr);
long long strtoll(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);
long   strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);

int    rand(void);
void   srand(unsigned int seed);

void   qsort(void *base, size_t nmemb, size_t size,
         int (*compar)(const void *, const void *));
void  *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
         int (*compar)(const void *, const void *));

int abs(int j);
long labs(long j);
long long llabs(long long j);

typedef struct { int quot, rem; } div_t;
typedef struct { long quot, rem; } ldiv_t;
typedef struct { long long quot, rem; } lldiv_t;

div_t div(int numer, int denom);
ldiv_t ldiv(long numer, long denom);
lldiv_t lldiv(long long numer, long long denom);

char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
int system(const char *command);

extern char **environ;
extern char **__environ;

#ifdef __cplusplus
}
#endif

#endif /* STDLIB_H */