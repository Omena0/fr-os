/*
 * kstring.c — freestanding string and memory primitives.
 *
 * The bulk loops are written three times: a scalar fallback, an SSE2 path, and
 * an AVX2 path. Which one runs is decided by a function pointer installed by
 * kstring_init() from CPUID results, not by a run-time branch inside every
 * call: an indirect call costs a predicted-taken branch and a few cycles, while
 * a branch inside the function costs the same and prevents the compiler from
 * inlining the small-size cases at all.
 *
 * The threshold for non-temporal stores is the point where the destination no
 * longer fits comfortably in L1/L2 and would evict lines the caller is about to
 * use. Zeroing a page that is about to be consumed by another CPU, or that is
 * about to be handed to the page allocator as a free frame, is exactly that
 * case.
 */

#include <kstring.h>
#include <cpu_features.h>
#include <io.h>

/*
 * Cache geometry for the non-temporal threshold. These are read from CPUID leaf
 * 4 (deterministic cache parameters) when available. The constants used when
 * leaf 4 is absent are the smallest plausible modern L1 data cache, which makes
 * the non-temporal path kick in slightly early rather than never.
 */
#define L1D_DEFAULT_BYTES  32768u
#define NT_THRESHOLD       (L1D_DEFAULT_BYTES * 4)

static size_t nt_threshold = L1D_DEFAULT_BYTES * 4;

/*
 * SSE2 is architecturally guaranteed on any CPU that can run 64-bit code, so
 * the SSE2 path needs no feature test. AVX2 does.
 */
typedef void (*memset_fn)(void *dst, int c, size_t n);
typedef void *(*memcpy_fn)(void *dst, const void *src, size_t n);

static void memset_scalar(void *dst, int c, size_t n);
static void *memcpy_scalar(void *dst, const void *src, size_t n);

#ifdef __SSE2__
#include <emmintrin.h>

static void memset_sse2(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    uint8_t v = (uint8_t)c;
    __m128i pattern = _mm_set1_epi8((char)v);

    if (n >= 64) {
        /* Align the destination to 16 bytes. Page-aligned callers, which is
         * most kernel zeroing, skip this entirely. */
        size_t head = (size_t)(-(uintptr_t)d) & 15;
        size_t tail;

        for (size_t i = 0; i < head; i++)
            d[i] = v;
        d += head;
        n -= head;

        tail = n & 63;
        n -= tail;

        for (size_t i = 0; i < n; i += 64) {
            _mm_store_si128((__m128i *)(d + i), pattern);
            _mm_store_si128((__m128i *)(d + i + 16), pattern);
            _mm_store_si128((__m128i *)(d + i + 32), pattern);
            _mm_store_si128((__m128i *)(d + i + 48), pattern);
        }
        d += n;
        for (size_t i = 0; i < tail; i++)
            d[i] = v;
        return;
    }

    for (size_t i = 0; i < n; i++)
        d[i] = v;
}

static void *memcpy_sse2(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    if (n >= 64) {
        size_t head = (size_t)(-(uintptr_t)d) & 15;
        size_t tail;

        for (size_t i = 0; i < head; i++)
            d[i] = s[i];
        d += head;
        s += head;
        n -= head;

        tail = n & 63;
        n -= tail;

        for (size_t i = 0; i < n; i += 64) {
            _mm_store_si128((__m128i *)(d + i),
                    _mm_load_si128((const __m128i *)(s + i)));
            _mm_store_si128((__m128i *)(d + i + 16),
                    _mm_load_si128((const __m128i *)(s + i + 16)));
            _mm_store_si128((__m128i *)(d + i + 32),
                    _mm_load_si128((const __m128i *)(s + i + 32)));
            _mm_store_si128((__m128i *)(d + i + 48),
                    _mm_load_si128((const __m128i *)(s + i + 48)));
        }
        d += n;
        s += n;
        for (size_t i = 0; i < tail; i++)
            d[i] = s[i];
        return dst;
    }

    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
    return dst;
}

static memset_fn  memset_impl  = memset_sse2;
static memcpy_fn  memcpy_impl  = memcpy_sse2;
#else
static memset_fn  memset_impl  = memset_scalar;
static memcpy_fn  memcpy_impl  = memcpy_scalar;
#endif

#ifdef __AVX2__
#include <immintrin.h>

/*
 * AVX2 memset. The 32-byte stores halve the number of uops for the same byte
 * count, which matters because page zeroing is the single hottest bulk operation
 * in the allocator.
 */
__attribute__((target("avx2")))
static void memset_avx2(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    __m256i pattern = _mm256_set1_epi8((char)c);

    if (n >= 128) {
        size_t head = (size_t)(-(uintptr_t)d) & 31;
        size_t tail;

        for (size_t i = 0; i < head; i++)
            d[i] = (uint8_t)c;
        d += head;
        n -= head;

        tail = n & 127;
        n -= tail;

        for (size_t i = 0; i < n; i += 128) {
            _mm256_store_si256((__m256i *)(d + i), pattern);
            _mm256_store_si256((__m256i *)(d + i + 32), pattern);
            _mm256_store_si256((__m256i *)(d + i + 64), pattern);
            _mm256_store_si256((__m256i *)(d + i + 96), pattern);
        }
        d += n;
        for (size_t i = 0; i < tail; i++)
            d[i] = (uint8_t)c;
        return;
    }

    if (n >= 32) {
        size_t tail = n & 31;
        n -= tail;
        for (size_t i = 0; i < n; i += 32)
            _mm256_store_si256((__m256i *)(d + i), pattern);
        d += n;
        for (size_t i = 0; i < tail; i++)
            d[i] = (uint8_t)c;
        return;
    }

    for (size_t i = 0; i < n; i++)
        d[i] = (uint8_t)c;
}

__attribute__((target("avx2")))
static void *memcpy_avx2(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    if (n >= 128) {
        size_t head = (size_t)(-(uintptr_t)d) & 31;
        size_t tail;

        for (size_t i = 0; i < head; i++)
            d[i] = s[i];
        d += head;
        s += head;
        n -= head;

        tail = n & 127;
        n -= tail;

        for (size_t i = 0; i < n; i += 128) {
            _mm256_store_si256((__m256i *)(d + i),
                _mm256_loadu_si256((const __m256i *)(s + i)));
            _mm256_store_si256((__m256i *)(d + i + 32),
                _mm256_loadu_si256((const __m256i *)(s + i + 32)));
            _mm256_store_si256((__m256i *)(d + i + 64),
                _mm256_loadu_si256((const __m256i *)(s + i + 64)));
            _mm256_store_si256((__m256i *)(d + i + 96),
                _mm256_loadu_si256((const __m256i *)(s + i + 96)));
        }
        d += n;
        s += n;
        for (size_t i = 0; i < tail; i++)
            d[i] = s[i];
        return dst;
    }

    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
    return dst;
}
#endif /* __AVX2__ */

/*
 * Non-temporal zeroing.
 *
 * For a region far larger than the last-level cache's write-combining buffers
 * will hold, streaming stores avoid the read-for-ownership traffic of ordinary
 * stores and leave the destination out of cache. That is the right trade when
 * the destination is a page frame that is about to become free memory, or an
 * incoming buffer whose contents the CPU will not read for a long time.
 *
 * Ordinary stores are better below the threshold, because the data usually is
 * read again soon and the streaming path's WC-buffer flushes are pure overhead.
 */
static void memset_nt(void *dst, int c, size_t n)
{
#ifdef __SSE2__
    uint8_t *d = dst;
    uint8_t v = (uint8_t)c;
    __m128i pattern = _mm_set1_epi8((char)v);

    if (n >= nt_threshold) {
        size_t head = (size_t)(-(uintptr_t)d) & 63;
        size_t tail;

        for (size_t i = 0; i < head; i++)
            d[i] = v;
        d += head;
        n -= head;

        tail = n & 63;
        n -= tail;

        for (size_t i = 0; i < n; i += 64) {
            _mm_stream_si128((__m128i *)(d + i), pattern);
            _mm_stream_si128((__m128i *)(d + i + 16), pattern);
            _mm_stream_si128((__m128i *)(d + i + 32), pattern);
            _mm_stream_si128((__m128i *)(d + i + 48), pattern);
        }
        /* SFENCE is required: streaming stores are weakly ordered and may
         * still be sitting in write-combining buffers when memset returns. */
        _mm_sfence();
        d += n;
        for (size_t i = 0; i < tail; i++)
            d[i] = v;
        return;
    }
#endif
    memset_impl(dst, c, n);
}

static void memset_scalar(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    uint8_t v = (uint8_t)c;

    /* Word-at-a-time when both operands can be aligned, which lets the
     * compiler use a 64-bit store instead of a byte loop. */
    if (((uintptr_t)d & 7) == 0) {
        uint64_t word = (uint64_t)v * 0x0101010101010101ULL;

        while (n >= 8) {
            *(uint64_t *)d = word;
            d += 8;
            n -= 8;
        }
    }
    for (size_t i = 0; i < n; i++)
        d[i] = v;
}

static void *memcpy_scalar(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    if (((uintptr_t)d & 7) == 0 && ((uintptr_t)s & 7) == 0) {
        while (n >= 8) {
            *(uint64_t *)d = *(const uint64_t *)s;
            d += 8;
            s += 8;
            n -= 8;
        }
    }
    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
    return dst;
}

/*
 * Public entry points. These are the symbols the compiler emits calls to, so
 * they must be real functions with C linkage, not just the selected
 * implementation.
 */
void *memset(void *dst, int c, size_t n)
{
    if (n >= nt_threshold)
        memset_nt(dst, c, n);
    else
        memset_impl(dst, c, n);
    return dst;
}

void bzero(void *dst, size_t n)
{
    memset(dst, 0, n);
}

void *memcpy(void *dst, const void *src, size_t n)
{
    return memcpy_impl(dst, src, n);
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    if (d == s || n == 0)
        return dst;
    /* Overlap is the only case where the direction of the copy matters. Both
     * ranges fit in cache for realistic sizes, so forwarding loads in either
     * direction is safe here; the only correctness requirement is not to
     * overwrite source bytes before reading them. */
    if (d < s || d >= s + n)
        return memcpy_impl(dst, src, n);

    if (d - s < (ptrdiff_t)n) {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    } else {
        while (n--)
            *d++ = *s++;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *p = a, *q = b;

    for (size_t i = 0; i < n; i++) {
        if (p[i] != q[i])
            return (int)p[i] - (int)q[i];
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const uint8_t *p = s;
    uint8_t v = (uint8_t)c;

    for (size_t i = 0; i < n; i++) {
        if (p[i] == v)
            return (void *)(p + i);
    }
    return NULL;
}

size_t strlen(const char *s)
{
    const char *p = s;

    while (*p)
        p++;
    return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;

    while (n < max && s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i])
            return (int)(uint8_t)a[i] - (int)(uint8_t)b[i];
        if (a[i] == '\0')
            return 0;
    }
    return 0;
}

char *strchr(const char *s, int c)
{
    char v = (char)c;

    for (;; s++) {
        if (*s == v)
            return (char *)s;
        if (*s == '\0')
            return NULL;
    }
}

char *strrchr(const char *s, int c)
{
    char v = (char)c;
    const char *last = NULL;

    for (;; s++) {
        if (*s == v)
            last = s;
        if (*s == '\0')
            break;
    }
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle)
{
    size_t nlen;

    if (*needle == '\0')
        return (char *)haystack;
    nlen = strlen(needle);

    for (; *haystack; haystack++) {
        if (*haystack == *needle && strncmp(haystack, needle, nlen) == 0)
            return (char *)haystack;
    }
    return NULL;
}

ksize_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);

    if (size > 0) {
        size_t copy = len < size - 1 ? len : size - 1;

        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}

ksize_t strlcat(char *dst, const char *src, size_t size)
{
    size_t dlen = strnlen(dst, size);
    size_t slen = strlen(src);

    if (dlen == size)
        return size + slen;
    if (slen < size - dlen) {
        memcpy(dst + dlen, src, slen + 1);
    } else {
        memcpy(dst + dlen, src, size - dlen - 1);
        dst[size - 1] = '\0';
    }
    return dlen + slen;
}

static inline int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

int strcasecmp(const char *a, const char *b)
{
    int ca, cb;

    do {
        ca = lower((uint8_t)*a++);
        cb = lower((uint8_t)*b++);
    } while (ca && ca == cb);
    return ca - cb;
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    int ca = 0, cb = 0;

    for (size_t i = 0; i < n; i++) {
        ca = lower((uint8_t)a[i]);
        cb = lower((uint8_t)b[i]);
        if (ca != cb)
            return ca - cb;
        if (ca == 0)
            return 0;
    }
    return ca - cb;
}

ksize_t kstrtoull(const char *s, unsigned long long *out, int base)
{
    unsigned long long v = 0;
    int digits = 0;
    int neg = 0;

    if (base != 0 && (base < 2 || base > 36))
        return 0;
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '-') {
        neg = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    if ((base == 0 || base == 16) && s[0] == '0' &&
        (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        base = 16;
    } else if (base == 0) {
        base = (s[0] == '0') ? 8 : 10;
    }

    for (;;) {
        int c = (uint8_t)*s;
        int d;

        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'z')
            d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z')
            d = c - 'A' + 10;
        else
            break;
        if (d >= base)
            break;
        v = v * (unsigned)base + (unsigned)d;
        digits++;
        s++;
    }

    if (digits == 0)
        return 0;
    *out = neg ? (unsigned long long)-(long long)v : v;
    return (ksize_t)digits;
}

ksize_t kstrtoul(const char *s, unsigned long *out, int base)
{
    unsigned long long v;
    ksize_t n = kstrtoull(s, &v, base);

    if (n == 0)
        return 0;
    *out = (unsigned long)v;
    return n;
}

/*
 * Install the best available implementations. Called once from kernel_main after
 * CPUID, before anything allocates or copies at scale.
 */
void kstring_init(void)
{
#ifdef __AVX2__
    if (cpu_has(CPU_FEATURE_AVX2)) {
        memset_impl = memset_avx2;
        memcpy_impl = memcpy_avx2;
    }
#endif

    /* Size the non-temporal threshold from the real L1 data cache if the CPU
     * reports one, so the threshold tracks the machine rather than a guess. */
    if (cpu_features.max_leaf >= 4) {
        for (uint32_t sub = 0; sub < 16; sub++) {
            uint32_t a, b, c, d;

            __asm__ volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(4), "c"(sub));
            if ((a & 0x1F) != 1)
                continue;   /* not a data cache */
            if ((a & 0xC0) != 0)
                continue;   /* not level 1 */

            uint32_t ways = ((b >> 22) & 0x3FF) + 1;
            uint32_t sets = ((b >> 12) & 0x3FF) + 1;
            uint32_t line = (b & 0xFFF) + 1;
            uint32_t part = ((b >> 8) & 3) + 1;
            uint32_t size = ways * sets * line * part;

            /* Use the streaming path once the destination is several
             * times the cache: beyond that the working set cannot be
             * retained even with perfect replacement. */
            nt_threshold = size * 4;
            break;
        }
    }
}
