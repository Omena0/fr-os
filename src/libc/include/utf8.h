#ifndef UTF8_H
#define UTF8_H

#include <stddef.h>
#include <stdint.h>

/*
 * utf8.h — UTF-8 encode/decode helpers.
 *
 * Small, dependency-free, freestanding-safe UTF-8 primitives. Available to
 * both the kernel (libk.a) and userspace (libc.a) so every component that
 * touches text agrees on one encoding.
 */

/* Decode one code point from *src, consuming up to *src_len bytes.
 * Returns the code point, or -1 on an incomplete/invalid sequence.
 * On success *src is advanced and *src_len reduced by the bytes consumed.
 */
uint32_t utf8_decode(const uint8_t **src, size_t *src_len);

/* Encode one code point into out (up to 4 bytes). Returns the number of
 * bytes written, or 0 if the code point is not encodable. */
size_t utf8_encode(uint32_t cp, uint8_t *out);

/* Number of bytes a leading UTF-8 byte announces (1 for ASCII, 0 if not a
 * valid lead byte, -1 for a stray continuation byte). */
int utf8_seqlen(uint8_t lead);

#endif
