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
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>

/* Internal syscall wrappers. Both are noreturn and declared as such: see the
 * note on sys_exit_group in syscall.c. Without it, every _Noreturn function
 * built on them (exit, _Exit, abort) ends with a call the compiler believes
 * cannot return, which is what produced the four 'noreturn function does
 * return' warnings this used to build with. */
extern void sys_exit_group(int status) __attribute__((noreturn));

/* div_t, ldiv_t and lldiv_t come from <stdlib.h>, which is already included
 * above. Repeating the typedefs here is a hard error, not a redeclaration
 * the compiler is willing to merge: the two anonymous structs are distinct
 * types with the same members. */


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
	/*
	 * Flush every stream, not just stdout. stdout is line buffered, so a
	 * printf with no trailing newline -- the progress dots and the partial
	 * line a program was interrupted on -- is still in its buffer at this
	 * point, and this comment used to claim there was no stdio to flush.
	 * There is: stdio.c is linked in whenever this file is. init.c only
	 * survived without this by calling fflush(stdout) by hand before
	 * returning, which is not something a program's exit() should depend
	 * on.
	 */
	fflush(NULL);
	sys_exit_group(status);
}

void _Exit(int status)
{
	/* Deliberately no flush: _Exit is the "leave now" path. */
	sys_exit_group(status);
}

void abort(void)
{
	/* In a real system, this would raise SIGABRT. There is no signal
	 * delivery here, so the honest equivalent is to terminate immediately
	 * with the abort status and skip the atexit handlers and the stdio
	 * flush, which is what abort() is specified not to do. */
	sys_exit_group(EXIT_FAILURE);
}


/*
 * How wide the caller's result object is. This has to be the *actual* width and
 * not a signed/unsigned/long-long split: atoi() passes an int (4 bytes) and
 * atol()/strtol() pass a long (8), so a single "not long long" case that stores
 * a long through both of them overwrites four bytes past atoi's local. That
 * corruption is silent -- atoi still returns the right value -- and it lands on
 * whatever the compiler happened to put next to the variable.
 */
enum str_width { W_INT = 0, W_LONG = 1, W_LLONG = 2 };

/*
 * The one place that knows how wide the caller's result object is. Every exit
 * path of strtoxx stores through it, which is what lets "no digits" and "invalid
 * base" store a zero at all: the atoi/atol/atoll wrappers ignore strtoxx's
 * return value and read their own local, so a path that stores nothing hands
 * them an uninitialised int.
 */
static void store_result(void *result, enum str_width width, int is_unsigned,
			 unsigned long long value)
{
	if (is_unsigned) {
		switch (width) {
		case W_INT:  *(unsigned int *)result = (unsigned int)value; break;
		case W_LONG: *(unsigned long *)result = (unsigned long)value; break;
		default:     *(unsigned long long *)result = value; break;
		}
	} else {
		switch (width) {
		case W_INT:  *(int *)result = (int)value; break;
		case W_LONG: *(long *)result = (long)value; break;
		default:     *(long long *)result = (long long)value; break;
		}
	}
}

static int strtoxx(const char *nptr, char **endptr, int base, int is_unsigned,
		   void *result, enum str_width width)
{
	const char *s = nptr;
	unsigned long long acc = 0;
	unsigned long long cutoff;
	unsigned long long sat;
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
		/*
		 * An invalid base writes 0 through result before returning, for
		 * the same reason the no-digits case below does: the atoi/atol/
		 * atoll wrappers ignore this function's return value and read
		 * whatever was in their own uninitialised local.
		 */
		store_result(result, width, is_unsigned, 0);
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
		if (is_unsigned)
			/* "-1" is 1 converted to unsigned and then negated,
			 * which is the maximum. */
			sat = neg ? 1ULL : ULLONG_MAX;
		else
			sat = neg ? (unsigned long long)LLONG_MAX + 1
				 : (unsigned long long)LLONG_MAX;
		store_result(result, width, is_unsigned, sat);
	} else if (!any) {
		/*
		 * No digits converted. C99 7.20.1.4: the value is 0. It is also
		 * the only path that does not write *result at all, which is
		 * invisible through strtol (the caller passes a real variable)
		 * and a read of uninitialised stack through atoi, which passes
		 * `int result;` and ignores this function's return value.
		 */
		store_result(result, width, is_unsigned, 0);
		if (endptr)
			*endptr = (char *)nptr;
		return 0;
	} else {
		/*
		 * The sign is applied for the unsigned conversions too. C99
		 * 7.20.1.4: the subject sequence has the expected form and is
		 * converted, and "-1" converted to unsigned long is ULONG_MAX by
		 * the narrowing rule, not 1. The previous code applied
		 * `if (neg) acc = -acc` only in the signed branch.
		 */
		if (neg)
			acc = (unsigned long long)0 - acc;
		store_result(result, width, is_unsigned, acc);
	}

	if (endptr)
		*endptr = (char *)s;
	return 1;
}

int atoi(const char *nptr)
{
	int result;
	strtoxx(nptr, NULL, 10, 0, &result, W_INT);
	return result;
}

long atol(const char *nptr)
{
	long result;
	strtoxx(nptr, NULL, 10, 0, &result, W_LONG);
	return result;
}

long long atoll(const char *nptr)
{
	long long result;
	strtoxx(nptr, NULL, 10, 0, &result, W_LLONG);
	return result;
}

long strtol(const char *nptr, char **endptr, int base)
{
	long result;
	strtoxx(nptr, endptr, base, 0, &result, W_LONG);
	return result;
}

long long strtoll(const char *nptr, char **endptr, int base)
{
	long long result;
	strtoxx(nptr, endptr, base, 0, &result, W_LLONG);
	return result;
}

unsigned long strtoul(const char *nptr, char **endptr, int base)
{
	unsigned long result;
	strtoxx(nptr, endptr, base, 1, &result, W_LONG);
	return result;
}

unsigned long long strtoull(const char *nptr, char **endptr, int base)
{
	unsigned long long result;
	strtoxx(nptr, endptr, base, 1, &result, W_LLONG);
	return result;
}


/*
 * A 64-bit LCG rather than the 32-bit one, because RAND_MAX is 0x7fffffff and
 * `rand_seed >> 16` of a 32-bit state can only ever produce 16 bits: the range
 * was [0, 65535] and `& RAND_MAX` was a no-op on an already-truncated value.
 * Bits 33..63 of a 64-bit multiply give all 31 bits RAND_MAX promises, which
 * is the top of the range rather than the well-behaved low bits of the old LCG.
 */
static unsigned long long rand_state = 1;

int rand(void)
{
	rand_state = rand_state * 6364136223846793005ULL + 1442695040888963407ULL;
	return (int)((rand_state >> 33) & (unsigned long long)RAND_MAX);
}

void srand(unsigned int seed)
{
	/* POSIX: srand(0) behaves as srand(1). A zero state would also make the
	 * sequence start at a fixed point of the multiplier. */
	rand_state = seed ? (unsigned long long)seed : 1ULL;
}

/* ------------------------------ qsort/bsearch ------------------------------ */

#define INSERTION_SORT_THRESHOLD 16

/*
 * The element being inserted is copied out before anything moves. It cannot be
 * left where it is: `key` used to alias base[i], and the first shift writes over
 * base[i] with base[i-1]. Without the copy the value being inserted is
 * overwritten by its predecessor before it is ever placed, and a descending
 * sort of three elements returns three copies of the largest.
 *
 * qsort() returns immediately for nmemb <= 16, so this *is* the whole sort for
 * every small array -- which is why the bug was silent rather than rare.
 */
static void insertion_sort(char *base, size_t nmemb, size_t size,
			   int (*compar)(const void *, const void *))
{
	char key[64];

	if (size <= sizeof(key)) {
		for (size_t i = 1; i < nmemb; i++) {
			size_t j = i;

			memcpy(key, base + i * size, size);
			while (j > 0 && compar(key, base + (j - 1) * size) < 0) {
				memcpy(base + j * size,
				       base + (j - 1) * size, size);
				j--;
			}
			if (j != i)
				memcpy(base + j * size, key, size);
		}
		return;
	}

	/*
	 * Elements wider than the scratch buffer. Same algorithm, but the shift
	 * is a memmove rather than a copy-through-key: moving the tail down by
	 * one slot cannot clobber the element still waiting at base[i] because
	 * it copies from strictly lower addresses upwards.
	 */
	/*
	 * Compare against base + i * size -- but that slot stops holding the
	 * element being inserted after the very first shift.
	 *
	 * The first iteration compares element i with element i-1 and then moves
	 * element i-1 *into* slot i, overwriting the element being inserted. The
	 * next iteration compares that overwritten slot against element i-2, so
	 * the key being compared is the previous element, not the one being
	 * placed. For a descending run the result is that the whole array is
	 * shifted down one slot and the last element is duplicated at the end:
	 * data is lost, silently, and qsort is the only caller.
	 *
	 * The fix is to find the insertion point first, then rotate the range
	 * [k, i] right by one. Rotating rather than shifting means the element
	 * being inserted is only ever moved once, at the end, from a slot nothing
	 * else has touched.
	 */
	for (size_t i = 1; i < nmemb; i++) {
		size_t k = i;

		while (k > 0 && compar(base + (k - 1) * size,
					base + i * size) > 0)
			k--;

		if (k == i)
			continue;		/* already in place */

		/*
		 * Rotate [k, i] right by one, byte column at a time so no
		 * scratch buffer the size of an element is needed.
		 */
		for (size_t off = 0; off < size; off++) {
			char tmp = base[i * size + off];

			for (size_t idx = i; idx > k; idx--)
				base[idx * size + off] =
					base[(idx - 1) * size + off];
			base[k * size + off] = tmp;
		}
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

/*
 * A pivot selection that is only a heuristic -- getting it wrong costs time,
 * not order -- but this one was wrong in a way that did cost order in the
 * caller: when compar(a, b) <= 0 and compar(a, c) > 0, a is the *smallest* of
 * the three (b >= a > c) so the median is b, and the code returned a. Left
 * alone it still sorts correctly; it just picks the smallest of three samples
 * as the pivot far more often than intended.
 */
static void *median_of_three(char *a, char *b, char *c,
			     int (*compar)(const void *, const void *))
{
	if (compar(a, b) > 0) {
		if (compar(b, c) > 0)
			return b;
		if (compar(a, c) > 0)
			return c;
		return a;
	}
	if (compar(a, c) > 0)
		return b;
	if (compar(b, c) > 0)
		return c;
	return b;
}

static void swap_bytes(char *a, char *b, size_t n)
{
	for (size_t k = 0; k < n; k++) {
		char t = a[k];

		a[k] = b[k];
		b[k] = t;
	}
}

/*
 * Lomuto partition, with the pivot already moved to the last element. The
 * boundary index is the single piece of state: on entry everything in
 * [0, boundary) is known to be <= the pivot and everything in [boundary, k) is
 * known to be > it, which is why the element at k can be moved into the gap
 * without disturbing either side.
 *
 * This replaces a Hoare-style two-pointer scan that assumed the pivot could not
 * end up on the wrong side of the boundary. It could: when the pivot is the
 * smallest element, the left scan stopped at index 0 and the right scan started
 * at the pivot itself -- where `base[j] > pivot` is false -- so it never moved,
 * the first iteration swapped the pivot to index 0, and the closing
 * "swap pivot into place" then shuffled some unrelated element instead. The
 * pivot at index 0 was then outside both recursive sub-ranges and was never
 * sorted at all. Reproduced with a 17-element int array: qsort left it with 15
 * out-of-order pairs.
 */
static size_t partition(char *base, size_t nmemb, size_t size,
			int (*compar)(const void *, const void *))
{
	char *pivot = base + (nmemb - 1) * size;
	size_t boundary = 0;

	for (size_t k = 0; k + 1 < nmemb; k++) {
		if (compar(base + k * size, pivot) <= 0) {
			if (k != boundary)
				swap_bytes(base + boundary * size,
					   base + k * size, size);
			boundary++;
		}
	}
	if (boundary != nmemb - 1)
		swap_bytes(base + boundary * size, pivot, size);
	return boundary;
}

static void introsort_loop(char *base, size_t nmemb, size_t size,
			   int (*compar)(const void *, const void *), int depth_limit)
{
	while (nmemb > INSERTION_SORT_THRESHOLD) {
		char *pivot_slot;
		char tmp_pivot[64];
		size_t mid;

		if (depth_limit == 0) {
			heapsort(base, nmemb, size, compar);
			return;
		}
		depth_limit--;

		/* Partition with a median-of-three pivot, moved to the end. */
		pivot_slot = median_of_three(base, base + (nmemb / 2) * size,
					     base + (nmemb - 1) * size, compar);
		if (size <= sizeof(tmp_pivot)) {
			memcpy(tmp_pivot, pivot_slot, size);
			memcpy(pivot_slot, base + (nmemb - 1) * size, size);
			memcpy(base + (nmemb - 1) * size, tmp_pivot, size);
		}

		mid = partition(base, nmemb, size, compar);

		/* Recurse on the smaller partition, loop on the larger one, so the
		 * stack depth stays logarithmic regardless of the split. */
		if (mid < nmemb - mid - 1) {
			introsort_loop(base, mid, size, compar, depth_limit);
			base += (mid + 1) * size;
			nmemb -= mid + 1;
		} else {
			introsort_loop(base + (mid + 1) * size, nmemb - mid - 1,
				       size, compar, depth_limit);
			nmemb = mid;
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


/*
 * The negation is done in the unsigned type and converted back. `j < 0 ? -j : j`
 * on a signed j is undefined behaviour for the one value where -j is not
 * representable, which is exactly the value abs() is documented to have to
 * handle: abs(INT_MIN) == INT_MIN.
 */
int abs(int j)
{
	unsigned int mag = (unsigned int)j;

	if (j < 0)
		mag = 0u - mag;
	return (int)mag;
}

long labs(long j)
{
	unsigned long mag = (unsigned long)j;

	if (j < 0)
		mag = 0ul - mag;
	return (long)mag;
}

long long llabs(long long j)
{
	unsigned long long mag = (unsigned long long)j;

	if (j < 0)
		mag = 0ull - mag;
	return (long long)mag;
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

/*
 * POSIX: an empty name, or one containing '=', is rejected with EINVAL. ENOSYS
 * is for "the variable manipulation is not implemented", which is a different
 * condition and would hide the mistake. The allocation these two need is not
 * written yet, so a valid name still fails -- but now with the error that
 * describes why.
 */
int setenv(const char *name, const char *value, int overwrite)
{
	if (!name || !*name || strchr(name, '=')) {
		__errno = EINVAL;
		return -1;
	}
	if (!value) {
		__errno = EINVAL;
		return -1;
	}
	if (!overwrite && getenv(name))
		return 0;
	/* Not implemented: would need to reallocate environ */
	__errno = ENOSYS;
	return -1;
}

int unsetenv(const char *name)
{
	if (!name || !*name || strchr(name, '=')) {
		__errno = EINVAL;
		return -1;
	}
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