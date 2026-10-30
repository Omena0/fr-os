/*
 * utf8.c — UTF-8 encode/decode primitives (kernel side).
 *
 * Freestanding-safe: no libc calls. Mirrors src/libc/src/utf8.c.
 */

#include <utf8.h>

u32 utf8_decode(const u8 **src, size_t *src_len)
{
    if (*src_len == 0)
        return (u32)-1;

    u8 lead = **src;
    int n = utf8_seqlen(lead);
    if (n < 0)
        return (u32)-1;
    if (n == 0)
        return (u32)-1;
    if ((size_t)n > *src_len)
        return (u32)-1;

    u32 cp;
    switch (n) {
    case 1:
        cp = lead;
        break;
    case 2:
        cp = ((u32)(lead & 0x1F) << 6) | (u32)(src[0][1] & 0x3F);
        if (cp < 0x80)
            return (u32)-1;
        break;
    case 3:
        cp = ((u32)(lead & 0x0F) << 12) |
             ((u32)(src[0][1] & 0x3F) << 6) |
             (u32)(src[0][2] & 0x3F);
        if (cp < 0x800 || (cp & 0xF800) == 0xD800)
            return (u32)-1;
        break;
    case 4:
        cp = ((u32)(lead & 0x07) << 18) |
             ((u32)(src[0][1] & 0x3F) << 12) |
             ((u32)(src[0][2] & 0x3F) << 6) |
             (u32)(src[0][3] & 0x3F);
        if (cp < 0x10000 || cp > 0x10FFFF)
            return (u32)-1;
        break;
    default:
        return (u32)-1;
    }

    for (int i = 1; i < n; i++) {
        if ((src[0][i] & 0xC0) != 0x80)
            return (u32)-1;
    }

    *src += n;
    *src_len -= n;
    return cp;
}

size_t utf8_encode(u32 cp, u8 *out)
{
    if (cp < 0x80) {
        out[0] = (u8)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (u8)(0xC0 | (cp >> 6));
        out[1] = (u8)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (u8)(0xE0 | (cp >> 12));
        out[1] = (u8)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (u8)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        out[0] = (u8)(0xF0 | (cp >> 18));
        out[1] = (u8)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (u8)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (u8)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

int utf8_seqlen(u8 lead)
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
