#ifndef UTF8_H
#define UTF8_H

#include <types.h>

/*
 * utf8.h — UTF-8 encode/decode primitives (kernel side).
 *
 * Mirrors src/libc/include/utf8.h using the kernel's u8/u32 aliases so the
 * kernel and userspace agree on one encoding without the kernel linking
 * userspace libc.
 */

u32 utf8_decode(const u8 **src, size_t *src_len);
size_t utf8_encode(u32 cp, u8 *out);
int utf8_seqlen(u8 lead);

#endif
