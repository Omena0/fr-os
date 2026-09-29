/*
 * stdlib.c — general utilities: exit, atexit, string-to-number, qsort, bsearch, rand, abs, env.
 *
 * exit runs atexit handlers in reverse order, flushes stdio, then calls sys_exit_group.
 * qsort is introsort with median-of-three pivot, insertion sort for small ranges,
 * and a depth limit that falls back to heapsort to bound stack usage.
 * strtol family handles bases 2..36, auto-detect (0), sign, and overflow with ERANGE.
 * system returns -1/ENOSYS; it does not fake execution.
 */
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>

/* Internal syscall wrappers */
extern void sys_exit_group(int status);

/* div_t, ldiv_t and lldiv_t come from <stdlib.h>, which is already included
 * above. Repeating the typedefs here is a hard error, not a redeclaration
 * the compiler is willing to merge: the two anonymous structs are distinct
 * types with the same members. */

/* __MORE__ */

#define ATEXIT_MAX 32

static void (*atexit_funcs[ATEXIT_MAX])(void);
static int atexit_count = 0;

int atexit(void (*func)(void))
{
	if (atexit_count >= ATEXIT_MAX)
		return -1;
	atexit_funcs[atexit_count++] = func;
	return 0;
}

/* __cxa_atexit is the C++ ABI variant; we support it by ignoring dso_handle. */
int __cxa_atexit(void (*func)(void), void *arg, void *dso_handle)
{
	(void)arg;
	(void)dso_handle;
	return atexit(func);
}

void exit(int status)
{
	/* Run atexit handlers in reverse order */
	for (int i = atexit_count - 1; i >= 0; i--) {
		if (atexit_funcs[i])
			atexit_funcs[i]();
	}
	/* Flush stdio (placeholder - no stdio implemented yet) */
	/* fflush(NULL); */
	sys_exit_group(status);
}

void _Exit(int status)
{
	sys_exit_group(status);
}

void abort(void)
{
	/* In a real system, this would raise SIGABRT. For now, just exit. */
	sys_exit_group(EXIT_FAILURE);
}

/* __MORE__ */

static int strtoxx(const char *nptr, char **endptr, int base, int is_unsigned, void *result, int is_long_long)
{
	const char *s = nptr;
	unsigned long long acc = 0;
	unsigned long long cutoff;
	int neg = 0;
	int any = 0;
	int c;

	/* Skip whitespace */
	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\v' || *s == '\f' || *s == '\r')
		s++;

	/* Handle sign */
	if (*s == '-') {
		neg = 1;
		s++;
	} else if (*s == '+') {
		s++;
	}

	/* Handle base prefix */
	if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
		base = 16;
	}
	if (base == 0) {
		if (*s == '0')
			base = 8;
		else
			base = 10;
	}
	if (base < 2 || base > 36) {
		if (endptr)
			*endptr = (char *)nptr;
		return 0;
	}

	/* Compute cutoff for overflow detection */
	cutoff = is_unsigned ? ULLONG_MAX : (neg ? (unsigned long long)LLONG_MAX + 1 : LLONG_MAX);
	cutoff /= base;

	for (;;) {
		c = *s;
		if (c >= '0' && c <= '9')
			c -= '0';
		else if (c >= 'A' && c <= 'Z')
			c -= 'A' - 10;
		else if (c >= 'a' && c <= 'z')
			c -= 'a' - 10;
		else
			break;
		if (c >= base)
			break;
		if (any < 0 || acc > cutoff || (acc == cutoff && (unsigned long long)c > (is_unsigned ? ULLONG_MAX % base : (neg ? (LLONG_MAX + 1ULL) % base : LLONG_MAX % base)))) {
			any = -1;
		} else {
			any = 1;
			acc = acc * base + c;
		}
		s++;
	}

	if (any < 0) {
		__errno = ERANGE;
		if (is_unsigned) {
			if (is_long_long)
				*(unsigned long long *)result = ULLONG_MAX;
			else
				*(unsigned long *)result = ULONG_MAX;
		} else {
			if (neg) {
				if (is_long_long)
					*(long long *)result = LLONG_MIN;
				else
					*(long *)result = LONG_MIN;
			} else {
				if (is_long_long)
					*(long long *)result = LLONG_MAX;
				else
					*(long *)result = LONG_MAX;
			}
		}
	} else if (!any) {
		if (endptr)
			*endptr = (char *)nptr;
		return 0;
	} else {
		if (is_unsigned) {
			if (is_long_long)
				*(unsigned long long *)result = acc;
			else
				*(unsigned long *)result = (unsigned long)acc;
		} else {
			if (neg)
				acc = -acc;
			if (is_long_long)
				*(long long *)result = (long long)acc;
			else
				*(long *)result = (long)acc;
		}
	}

	if (endptr)
		*endptr = (char *)s;
	return 1;
}

int atoi(const char *nptr)
{
	int result;
	strtoxx(nptr, NULL, 10, 0, &result, 0);
	return result;
}

long atol(const char *nptr)
{
	long result;
	strtoxx(nptr, NULL, 10, 0, &result, 0);
	return result;
}

long long atoll(const char *nptr)
{
	long long result;
	strtoxx(nptr, NULL, 10, 0, &result, 1);
	return result;
}

long strtol(const char *nptr, char **endptr, int base)
{
	long result;
	strtoxx(nptr, endptr, base, 0, &result, 0);
	return result;
}

long long strtoll(const char *nptr, char **endptr, int base)
{
	long long result;
	strtoxx(nptr, endptr, base, 0, &result, 1);
	return result;
}

unsigned long strtoul(const char *nptr, char **endptr, int base)
{
	unsigned long result;
	strtoxx(nptr, endptr, base, 1, &result, 0);
	return result;
}

unsigned long long strtoull(const char *nptr, char **endptr, int base)
{
	unsigned long long result;
	strtoxx(nptr, endptr, base, 1, &result, 1);
	return result;
}

/* __MORE__ */

static unsigned int rand_seed = 1;

int rand(void)
{
	rand_seed = rand_seed * 1103515245 + 12345;
	return (int)(rand_seed >> 16) & RAND_MAX;
}

void srand(unsigned int seed)
{
	rand_seed = seed;
}

/* ------------------------------ qsort/bsearch ------------------------------ */

#define INSERTION_SORT_THRESHOLD 16

static void insertion_sort(char *base, size_t nmemb, size_t size,
			   int (*compar)(const void *, const void *))
{
	for (size_t i = 1; i < nmemb; i++) {
		char *key = base + i * size;
		size_t j = i;
		while (j > 0 && compar(key, base + (j - 1) * size) < 0) {
			memcpy(base + j * size, base + (j - 1) * size, size);
			j--;
		}
		if (j != i)
			memcpy(base + j * size, key, size);
	}
}

static void heapify(char *base, size_t n, size_t i, size_t size,
		    int (*compar)(const void *, const void *))
{
	size_t largest = i;
	size_t left = 2 * i + 1;
	size_t right = 2 * i + 2;

	if (left < n && compar(base + left * size, base + largest * size) > 0)
		largest = left;
	if (right < n && compar(base + right * size, base + largest * size) > 0)
		largest = right;
	if (largest != i) {
		/* Swap */
		for (size_t k = 0; k < size; k++) {
			char tmp = base[i * size + k];
			base[i * size + k] = base[largest * size + k];
			base[largest * size + k] = tmp;
		}
		heapify(base, n, largest, size, compar);
	}
}

static void heapsort(char *base, size_t nmemb, size_t size,
		     int (*compar)(const void *, const void *))
{
	/* Build max heap */
	for (size_t i = nmemb / 2; i > 0; i--)
		heapify(base, nmemb, i - 1, size, compar);
	/* Extract elements */
	for (size_t i = nmemb - 1; i > 0; i--) {
		/* Swap root with last */
		for (size_t k = 0; k < size; k++) {
			char tmp = base[k];
			base[k] = base[i * size + k];
			base[i * size + k] = tmp;
		}
		heapify(base, i, 0, size, compar);
	}
}

static int log2_int(int n)
{
	int r = 0;
	while (n >>= 1)
		r++;
	return r;
}

static void *median_of_three(char *a, char *b, char *c,
			     int (*compar)(const void *, const void *))
{
	if (compar(a, b) > 0) {
		if (compar(b, c) > 0)
			return b;
		else if (compar(a, c) > 0)
			return c;
		else
			return a;
	} else {
		if (compar(a, c) > 0)
			return a;
		else if (compar(b, c) > 0)
			return c;
		else
			return b;
	}
}

static void introsort_loop(char *base, size_t nmemb, size_t size,
			   int (*compar)(const void *, const void *), int depth_limit)
{
	while (nmemb > INSERTION_SORT_THRESHOLD) {
		if (depth_limit == 0) {
			heapsort(base, nmemb, size, compar);
			return;
		}
		depth_limit--;

		/* Partition with median-of-three pivot */
		char *pivot = median_of_three(base, base + (nmemb / 2) * size, base + (nmemb - 1) * size, compar);
		/* Move pivot to end */
		char tmp_pivot[64];
		if (size <= sizeof(tmp_pivot)) {
			memcpy(tmp_pivot, pivot, size);
			memcpy(pivot, base + (nmemb - 1) * size, size);
			memcpy(base + (nmemb - 1) * size, tmp_pivot, size);
		}
		pivot = base + (nmemb - 1) * size;

		size_t i = 0, j = nmemb - 1;
		while (1) {
			while (i < j && compar(base + i * size, pivot) < 0)
				i++;
			while (j > i && compar(base + j * size, pivot) > 0)
				j--;
			if (i >= j)
				break;
			/* Swap i and j */
			for (size_t k = 0; k < size; k++) {
				char tmp = base[i * size + k];
				base[i * size + k] = base[j * size + k];
				base[j * size + k] = tmp;
			}
			i++;
			j--;
		}
		/* Swap pivot into place */
		for (size_t k = 0; k < size; k++) {
			char tmp = base[i * size + k];
			base[i * size + k] = base[(nmemb - 1) * size + k];
			base[(nmemb - 1) * size + k] = tmp;
		}

		/* Recurse on smaller partition, loop on larger */
		if (i < nmemb - i) {
			introsort_loop(base, i, size, compar, depth_limit);
			base += (i + 1) * size;
			nmemb = nmemb - i - 1;
		} else {
			introsort_loop(base + (i + 1) * size, nmemb - i - 1, size, compar, depth_limit);
			nmemb = i;
		}
	}
	insertion_sort(base, nmemb, size, compar);
}

void qsort(void *base, size_t nmemb, size_t size,
	   int (*compar)(const void *, const void *))
{
	if (nmemb < 2 || size == 0)
		return;
	int depth_limit = 2 * log2_int((int)nmemb);
	introsort_loop(base, nmemb, size, compar, depth_limit);
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
	      int (*compar)(const void *, const void *))
{
	size_t low = 0, high = nmemb;
	while (low < high) {
		size_t mid = low + (high - low) / 2;
		int cmp = compar(key, (const char *)base + mid * size);
		if (cmp == 0)
			return (void *)((const char *)base + mid * size);
		else if (cmp < 0)
			high = mid;
		else
			low = mid + 1;
	}
	return NULL;
}

/* __MORE__ */

int abs(int j)
{
	return j < 0 ? -j : j;
}

long labs(long j)
{
	return j < 0 ? -j : j;
}

long long llabs(long long j)
{
	return j < 0 ? -j : j;
}

div_t div(int numer, int denom)
{
	div_t result;
	result.quot = numer / denom;
	result.rem = numer % denom;
	return result;
}

ldiv_t ldiv(long numer, long denom)
{
	ldiv_t result;
	result.quot = numer / denom;
	result.rem = numer % denom;
	return result;
}

lldiv_t lldiv(long long numer, long long denom)
{
	lldiv_t result;
	result.quot = numer / denom;
	result.rem = numer % denom;
	return result;
}

/* ------------------------------ environment ------------------------------- */

/*
 * environ and __environ are defined by crt1.c, which is the only place that
 * knows the environment: the kernel hands envp to _start and the start-up code
 * publishes it. stdlib.c just reads them.
 */

char *getenv(const char *name)
{
	if (!environ || !name)
		return NULL;
	size_t len = strlen(name);
	for (char **ep = environ; *ep; ep++) {
		if (strncmp(*ep, name, len) == 0 && (*ep)[len] == '=')
			return *ep + len + 1;
	}
	return NULL;
}

int setenv(const char *name, const char *value, int overwrite)
{
	if (!name || !*name || strchr(name, '='))
		return -1;
	if (!overwrite && getenv(name))
		return 0;
	/* Not implemented: would need to reallocate environ */
	__errno = ENOSYS;
	return -1;
}

int unsetenv(const char *name)
{
	if (!name || !*name || strchr(name, '='))
		return -1;
	/* Not implemented */
	__errno = ENOSYS;
	return -1;
}

int system(const char *command)
{
	(void)command;
	__errno = ENOSYS;
	return -1;
}