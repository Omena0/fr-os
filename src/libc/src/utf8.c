/*
 * utf8.c — UTF-8 encode/decode primitives.
 *
 * Freestanding-safe: no libc calls, no assumptions about the host locale.
 * Compiles into both libc.a (userspace) and libk.a (the kernel).
 */

#include <utf8.h>

/* Decode one code point. *src must point at the lead byte; *src_len is the
 * number of bytes available. On success both are advanced past the sequence. */
uint32_t utf8_decode(const uint8_t **src, size_t *src_len)
{
    if (*src_len == 0)
        return (uint32_t)-1;

    uint8_t lead = **src;
    int n = utf8_seqlen(lead);
    if (n < 0)
        return (uint32_t)-1;          /* stray continuation byte */
    if (n == 0)
        return (uint32_t)-1;          /* not a valid lead byte */
    if ((size_t)n > *src_len)
        return (uint32_t)-1;          /* incomplete sequence */

    uint32_t cp;
    switch (n) {
    case 1:
        cp = lead;
        break;
    case 2:
        cp = ((uint32_t)(lead & 0x1F) << 6) | (uint32_t)(src[0][1] & 0x3F);
        if (cp < 0x80)
            return (uint32_t)-1;      /* overlong */
        break;
    case 3:
        cp = ((uint32_t)(lead & 0x0F) << 12) |
             ((uint32_t)(src[0][1] & 0x3F) << 6) |
             (uint32_t)(src[0][2] & 0x3F);
        if (cp < 0x800 || (cp & 0xF800) == 0xD800)
            return (uint32_t)-1;      /* overlong or surrogate */
        break;
    case 4:
        cp = ((uint32_t)(lead & 0x07) << 18) |
             ((uint32_t)(src[0][1] & 0x3F) << 12) |
             ((uint32_t)(src[0][2] & 0x3F) << 6) |
             (uint32_t)(src[0][3] & 0x3F);
        if (cp < 0x10000 || cp > 0x10FFFF)
            return (uint32_t)-1;      /* overlong or out of range */
        break;
    default:
        return (uint32_t)-1;
    }

    /* Validate continuation bytes. */
    for (int i = 1; i < n; i++) {
        if ((src[0][i] & 0xC0) != 0x80)
            return (uint32_t)-1;
    }

    *src += n;
    *src_len -= n;
    return cp;
}

/* Encode one code point. Returns bytes written, or 0 if not encodable. */
size_t utf8_encode(uint32_t cp, uint8_t *out)
{
    if (cp < 0x80) {
        out[0] = (uint8_t)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (uint8_t)(0xC0 | (cp >> 6));
        out[1] = (uint8_t)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (uint8_t)(0xE0 | (cp >> 12));
        out[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (uint8_t)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        out[0] = (uint8_t)(0xF0 | (cp >> 18));
        out[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (uint8_t)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

/* Bytes a lead byte announces. */
int utf8_seqlen(uint8_t lead)
{
    if (lead < 0x80)
        return 1;
    if ((lead & 0xE0) == 0xC0)
        return 2;
    if ((lead & 0xF0) == 0xE0)
        return 3;
    if ((lead & 0xF8) == 0xF0)
        return 4;
    return -1;
}
