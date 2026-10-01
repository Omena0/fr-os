/*
 * stdlib.h — the public surface of the allocator and the rest of stdlib.
 *
 * The allocator is a segregated-list design with a per-thread free-list cache
 * and mmap-backed arenas. Anything larger than 128 KiB bypasses the arena
 * entirely and is handed out by mmap directly, so a single huge allocation
 * cannot fragment a shared arena. Freed memory is zeroed so a use-after-free
 * reads as a null pointer rather than stale data.
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
 * aligned_alloc and posix_memalign over-allocate and store the raw pointer in
 * the sizeof(void*) bytes immediately below the address they return.
 *
 * Consequence, and it is a real limitation rather than an oversight: free()
 * cannot accept the aligned address. free() validates the block by reading the
 * header at `ptr - sizeof(struct chunk)`, and for an aligned pointer those
 * bytes are the stored raw pointer, not a header -- so free() refuses it and
 * aborts instead of corrupting the heap. A program that needs aligned memory
 * that free() can reclaim must ask malloc() for `size + alignment +
 * sizeof(void *)` and do the alignment itself.
 *
 * C11 7.22.3.1 and POSIX both say free() shall accept the aligned result;
 * neither is satisfied here. Nothing in the tree uses these two functions, so
 * the gap is documented rather than worked around; the fix belongs in the
 * allocator, which is the only code that can find the raw pointer.
 */
void *aligned_alloc(size_t alignment, size_t size);
int   posix_memalign(void **memptr, size_t alignment, size_t size);

void  exit(int status) __attribute__((noreturn));
void  _Exit(int status) __attribute__((noreturn));
void  abort(void) __attribute__((noreturn));
int   atexit(void (*func)(void));
int   __cxa_atexit(void (*func)(void), void *arg, void *dso_handle);

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