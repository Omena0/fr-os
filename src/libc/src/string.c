/*
 * string.c — memory and string primitives.
 *
 * These functions are part of the freestanding contract: the compiler emits
 * calls to memset/memcpy for struct assignment and array initialisation, so
 * they must exist with their canonical names and return their first
 * argument. memmove handles the overlap case explicitly because memcpy's
 * behaviour on overlap is undefined and a naive forward copy would corrupt
 * the tail of an overlapping region.
 */
#include <string.h>
#include <stdlib.h>
#include <errno.h>

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    size_t i;

    for (i = 0; i < n; i++)
        d[i] = (unsigned char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    size_t i;

    for (i = 0; i < n; i++)
        d[i] = s[i];
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    if (d == s || n == 0)
        return dst;

    /* Copy backwards when the destination overlaps the source past the
     * start, so the overlapping tail is read before it is overwritten. */
    if (d < s) {
        size_t i;
        for (i = 0; i < n; i++)
            d[i] = s[i];
    } else {
        size_t i;
        for (i = n; i > 0; i--)
            d[i - 1] = s[i - 1];
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = a;
    const unsigned char *pb = b;
    size_t i;

    for (i = 0; i < n; i++) {
        if (pa[i] != pb[i])
            return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *ps = s;
    size_t i;

    for (i = 0; i < n; i++) {
        if (ps[i] == (unsigned char)c)
            return (void *)(ps + i);
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
    size_t i;

    for (i = 0; i < max; i++) {
        if (s[i] == '\0')
            return i;
    }
    return max;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (a[i] != b[i])
            return (int)(unsigned char)a[i] -
                   (int)(unsigned char)b[i];
        if (a[i] == '\0')
            return 0;
    }
    return 0;
}

char *strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c)
            return (char *)s;
        s++;
    }
    if ((char)c == '\0')
        return (char *)s;
    return NULL;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;

    while (*s) {
        if (*s == (char)c)
            last = s;
        s++;
    }
    if ((char)c == '\0')
        return (char *)s;
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle)
{
    size_t nlen;

    if (!*needle)
        return (char *)haystack;
    nlen = strlen(needle);

    while (*haystack) {
        if (strncmp(haystack, needle, nlen) == 0)
            return (char *)haystack;
        haystack++;
    }
    return NULL;
}

char *strdup(const char *s)
{
    size_t len = strlen(s) + 1;
    char *copy = (char *)malloc(len);

    if (copy)
        memcpy(copy, s, len);
    return copy;
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t srclen = strlen(src);

    if (size != 0) {
        size_t copy = srclen < size - 1 ? srclen : size - 1;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return srclen;
}

size_t strlcat(char *dst, const char *src, size_t size)
{
    size_t dlen = strnlen(dst, size);
    size_t slen = strlen(src);
    size_t i;

    if (dlen >= size)
        return size + slen;
    for (i = dlen; i < size - 1 && *src; i++)
        dst[i] = *src++;
    dst[i] = '\0';
    return dlen + slen;
}

static unsigned char to_lower(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

/*
 * Both case-insensitive compares have to fold *before* deciding the bytes
 * differ.  Testing `*a == *b` first and only then folding is a different
 * function: strcasecmp("ab", "A") stopped at the first pair, saw 'a' != 'A',
 * and reported the folded difference of 'a' and 'A', which is zero -- so it
 * claimed "ab" and "A" were equal, and likewise "abc" against "A".  The
 * strncasecmp had the same shape one character at a time.
 *
 * The comparisons below subtract the two *folded* bytes and return that
 * difference, which is zero if and only if the two characters fold to the same
 * byte.  That is the only property strcmp(3) promises a caller, and it is what
 * the old byte test failed to give.
 */
int strcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        int d = (int)to_lower((unsigned char)*a) -
            (int)to_lower((unsigned char)*b);

        if (d)
            return d;
        a++;
        b++;
    }
    return (int)to_lower((unsigned char)*a) -
           (int)to_lower((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        int d = (int)to_lower((unsigned char)a[i]) -
            (int)to_lower((unsigned char)b[i]);

        if (d)
            return d;
        if (a[i] == '\0')
            return 0;
    }
    return 0;
}
size_t strspn(const char *s, const char *accept)
{
    const char *p = s;

    while (*p && strchr(accept, *p))
        p++;
    return (size_t)(p - s);
}

size_t strcspn(const char *s, const char *reject)
{
    const char *p = s;

    while (*p && !strchr(reject, *p))
        p++;
    return (size_t)(p - s);
}

/*
 * The reentrant form carries its own cursor, so it is the one that can be used
 * to parse two strings at once. The non-reentrant strtok() is a thin wrapper
 * over it with the cursor in a file-static.
 */
char *strtok_r(char *s, const char *delim, char **save)
{
    char *start;

    if (!save)
        return NULL;
    if (!s)
        s = *save;
    if (!s)
        return NULL;

    /* Leading delimiters are not a token; they are skipped. */
    s += strspn(s, delim);
    if (*s == '\0') {
        *save = NULL;
        return NULL;
    }

    start = s;
    s += strcspn(s, delim);
    if (*s) {
        *s = '\0';
        *save = s + 1;
    } else {
        *save = NULL;
    }
    return start;
}

static char *strtok_save;

char *strtok(char *s, const char *delim)
{
    return strtok_r(s, delim, &strtok_save);
}

/*
 * A switch rather than a table indexed by errno, because errno.h defines the
 * numbers as macros and the header deliberately carries no string table: a
 * table would be a second place where the ABI's error numbers are spelled.
 */
char *strerror(int errnum)
{
    static const struct {
        int err;
        const char *msg;
    } table[] = {
        { 0,        "Success" },
        { EPERM,    "Operation not permitted" },
        { ENOENT,    "No such file or directory" },
        { ESRCH,    "No such process" },
        { EINTR,    "Interrupted system call" },
        { EIO,        "Input/output error" },
        { ENXIO,    "No such device or address" },
        { E2BIG,    "Argument list too long" },
        { ENOEXEC,    "Exec format error" },
        { EBADF,    "Bad file descriptor" },
        { ECHILD,    "No child processes" },
        { EAGAIN,    "Resource temporarily unavailable" },
        { ENOMEM,    "Cannot allocate memory" },
        { EACCES,    "Permission denied" },
        { EFAULT,    "Bad address" },
        { ENOTBLK,    "Block device required" },
        { EBUSY,    "Device or resource busy" },
        { EEXIST,    "File exists" },
        { EXDEV,    "Invalid cross-device link" },
        { ENODEV,    "No such device" },
        { ENOTDIR,    "Not a directory" },
        { EISDIR,    "Is a directory" },
        { EINVAL,    "Invalid argument" },
        { ENFILE,    "Too many open files in system" },
        { EMFILE,    "Too many open files" },
        { ENOTTY,    "Inappropriate ioctl for device" },
        { ETXTBSY,    "Text file busy" },
        { EFBIG,    "File too large" },
        { ENOSPC,    "No space left on device" },
        { ESPIPE,    "Illegal seek" },
        { EROFS,    "Read-only file system" },
        { EMLINK,    "Too many links" },
        { EPIPE,    "Broken pipe" },
        { ERANGE,    "Numerical result out of range" },
        { ENAMETOOLONG, "File name too long" },
        { ENOSYS,    "Function not implemented" },
        { ENOTEMPTY,    "Directory not empty" },
        { ELOOP,    "Too many levels of symbolic links" },
    };
    unsigned int i;

    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (table[i].err == errnum)
            return (char *)table[i].msg;
    }
    return "Unknown error";
}
