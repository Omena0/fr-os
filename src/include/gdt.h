/*
 * gdt.h — segment descriptor encodings shared by stage2 and the kernel.
 *
 * These are the raw bit patterns for the x86-64 GDT entries. Both the stage2
 * loader and the kernel build their GDTs from them, so the privilege levels and
 * long-mode flags cannot drift apart between the two.
 */
#ifndef GDT_H
#define GDT_H

/* This header is included from .S files (assembled with -x assembler-with-cpp)
 * as well as from C, so it must not depend on <stdint.h>: the assembler's CPP
 * runs first and would choke on the typedefs. The casts below use `unsigned
 * long long`, which needs no typedef in either language. */

/* Access byte bits. The access byte is bit 7 P, bits 6-5 DPL, bit 4 S, and
 * bits 3-0 the type. DPL is a *two-bit* field, so ring 3 is 0b11 at bits 6-5
 * -- that is (3 << 5) = 0x60. It used to be written (3 << 6) = 0xC0, which set
 * P again and DPL bit 1 only, giving ring 3 a DPL of 2. */
#define GDT_ACCESS_PRESENT    (1 << 7)
#define GDT_ACCESS_RING0      (0 << 5)
#define GDT_ACCESS_RING3      (3 << 5)
#define GDT_ACCESS_SEGMENT    (1 << 4)
#define GDT_ACCESS_CODE       (1 << 3)
#define GDT_ACCESS_DATA       (1 << 2)
#define GDT_ACCESS_EXEC       (1 << 1)
#define GDT_ACCESS_RW         (1 << 1)
#define GDT_ACCESS_ACCESSED   (1 << 0)

/* Flags nibble bits (the byte following base:limit). */
#define GDT_FLAG_GRANULARITY  (1 << 3)  /* G: limit is scaled by 4096 */
#define GDT_FLAG_64BIT        (1 << 1)  /* L: 64-bit code segment */
#define GDT_FLAG_DB           (1 << 2)  /* B/DB: 32-bit default operand size */
#define GDT_FLAG_LONG_MODE    (1 << 1)

/*
 * Compose one descriptor.
 *
 * The 64-bit layout, byte by byte:
 *
 *   0-1  limit[15:0]        2-3  base[15:0]     4    base[23:16]
 *   5    access             6    flags|lim[19:16]
 *   7    base[31:28]        8-9  base[47:32]
 *
 * `flags` is the *byte-6* field -- AVL, L, DB, G -- and `base`/`limit` are
 * split across every byte they touch.
 *
 * This macro used to place `flags` in the access byte instead and never wrote
 * byte 6 at all. Everything then shifted: a kernel code descriptor came out as
 * 0x0000ba000000ffff, where 0xba is the access byte with the flags folded into
 * it and the flags nibble is zero. **L is therefore 0** -- a 16-bit code
 * segment -- and loading CS with it in long mode raises #GP with the selector
 * in the error code. The kernel was executing on stage2's descriptor, which is
 * correct, so nothing looked wrong until the first `lgdt`.
 *
 * The same collapse destroyed every DPL, because the flag constants aliased the
 * access byte's own bits: the old GDT_FLAG_64BIT/LONG_MODE was 1<<5, which is
 * DPL bit 1, and the old GDT_FLAG_DB was 1<<6, which is DPL bit 0. A separate
 * error survived that fix: GDT_ACCESS_RING3 was (3<<6), where DPL needs (3<<5),
 * so the user descriptors still came out as DPL 2. Both are corrected above.
 * A SYSRET to a DPL-2 descriptor would have #GP'd as soon as the first fault
 * was out of the way.
 *
 * That is why the #GP survived three different far-return frame shapes: the
 * frame was never the problem.
 */
#define GDT_ENTRY(access, flags, base, limit) \
    (((unsigned long long)(limit) & 0xFFFFULL) \
     | (((unsigned long long)(base) & 0xFFFFFFULL) << 16) \
     | (((unsigned long long)(access) & 0xFFULL) << 40) \
     | ((((unsigned long long)(limit) >> 16) & 0xFULL) << 48) \
     | (((unsigned long long)(flags) & 0xFULL) << 52) \
     | ((((unsigned long long)(base) >> 24) & 0xFFULL) << 56))

/* The canonical flat segments used by both stages. A limit of 0xFFFFF with a
 * 4 KiB granularity gives exactly 2^47 bytes, which is every canonical address
 * on a 48-bit virtual machine. */
#define GDT_CODE64_FLAGS \
    GDT_ACCESS_PRESENT | GDT_ACCESS_RING0 | GDT_ACCESS_SEGMENT | GDT_ACCESS_CODE | GDT_ACCESS_EXEC | GDT_ACCESS_ACCESSED


#define GDT_DATA_FLAGS \
    GDT_ACCESS_PRESENT | GDT_ACCESS_RING0 | GDT_ACCESS_SEGMENT | GDT_ACCESS_DATA | GDT_ACCESS_RW | GDT_ACCESS_ACCESSED


#define GDT_CODE32_FLAGS \
    GDT_ACCESS_PRESENT | GDT_ACCESS_RING0 | GDT_ACCESS_SEGMENT | GDT_ACCESS_CODE | GDT_ACCESS_EXEC | GDT_ACCESS_ACCESSED


#define GDT_USER_CODE64_FLAGS \
    GDT_ACCESS_PRESENT | GDT_ACCESS_RING3 | GDT_ACCESS_SEGMENT | GDT_ACCESS_CODE | GDT_ACCESS_EXEC | GDT_ACCESS_ACCESSED


#define GDT_USER_DATA_FLAGS \
    GDT_ACCESS_PRESENT | GDT_ACCESS_RING3 | GDT_ACCESS_SEGMENT | GDT_ACCESS_DATA | GDT_ACCESS_RW | GDT_ACCESS_ACCESSED


#define GDT_USER_CODE32_FLAGS \
    GDT_ACCESS_PRESENT | GDT_ACCESS_RING3 | GDT_ACCESS_SEGMENT | GDT_ACCESS_CODE | GDT_ACCESS_EXEC | GDT_ACCESS_ACCESSED


#define GDT_LCODE64  GDT_ENTRY(GDT_CODE64_FLAGS, GDT_FLAG_GRANULARITY | GDT_FLAG_LONG_MODE, 0, 0xFFFFF)
#define GDT_LDATA64  GDT_ENTRY(GDT_DATA_FLAGS,  GDT_FLAG_GRANULARITY, 0, 0xFFFFF)
#define GDT_LCODE32  GDT_ENTRY(GDT_CODE32_FLAGS, GDT_FLAG_GRANULARITY | GDT_FLAG_DB, 0, 0xFFFFF)
#define GDT_LDATA32  GDT_ENTRY(GDT_DATA_FLAGS,  GDT_FLAG_GRANULARITY | GDT_FLAG_DB, 0, 0xFFFFF)
#define GDT_LUSER64  GDT_ENTRY(GDT_USER_CODE64_FLAGS, GDT_FLAG_GRANULARITY | GDT_FLAG_LONG_MODE, 0, 0xFFFFF)
#define GDT_LUSERDATA GDT_ENTRY(GDT_USER_DATA_FLAGS, GDT_FLAG_GRANULARITY, 0, 0xFFFFF)

/* Index 3: a 32-bit ring-3 code descriptor that user code never executes on.
 * It exists only so SYSRET's arithmetic lands in the right place — see
 * "Selector Arithmetic" in docs/src/kernel/privilege-levels.md. SYSRET computes
 * CS = STAR[63:48] + 16, and this is the descriptor STAR[63:48] names. */
#define GDT_LUSERCODE32 GDT_ENTRY(GDT_USER_CODE32_FLAGS, GDT_FLAG_GRANULARITY | GDT_FLAG_DB, 0, 0xFFFFF)

/* GDT indices. These are not the selector values: a selector is an index
 * shifted left by three, with the requested privilege level in the low two
 * bits. The two disagree deliberately and confusingly often enough that the
 * distinction is worth spelling out here. */
#define GDT_INDEX_NULL   0
#define GDT_INDEX_KCODE  1
#define GDT_INDEX_KDATA  2
#define GDT_INDEX_UCODE32 3   /* STAR base only; never executed */
#define GDT_INDEX_UDATA  4
#define GDT_INDEX_UCODE64 5
#define GDT_INDEX_TSS    6   /* occupies indices 6 and 7 */
#define GDT_ENTRIES      8

/* ------------------------------------------------- TSS descriptor ----- */

/*
 * Access byte for a present, available 64-bit TSS.
 *   bit 7  P    present
 *   bit 6  S    0: this is a system descriptor, not a code/data segment
 *   bits 3-0  type 0b1001 = available 64-bit TSS
 */
#define GDT_TSS_ACCESS64  0x89

/* GDT_TSS_DESC64 — a 64-bit TSS system descriptor, as the two 8-byte halves the
 * GDT actually stores it in.
 *
 * A system descriptor carries a 20-bit limit and a 32-bit base, which is ten
 * bytes. A 64-bit TSS base does not fit in 32 bits, so the descriptor grows to
 * 16 bytes and takes a second GDT slot; that is why the TSS occupies two entries
 * and why the GDT has to be sized for GDT_INDEX_TSS + 1.
 *
 * Byte layout, low half first:
 *   0-1 limit[15:0]   2-3 base[15:0]   4 base[23:16]   5 access
 *   6 flags           7 limit[31:16]
 * High half: 0-3 base[31:24]   4-7 base[63:32]
 *
 * The flags byte is granularity in bit 7 and limit[19:16] in its low nibble.
 * Granularity is ignored for system descriptors, but it is set to match the
 * other entries in the table rather than left as a silent difference.
 *
 * Granularity was OR'd in at bit 51, which is *inside* the limit[19:16] field
 * rather than the G bit at 55, so it raised the limit by 0x800 (0x4f -> 0x84f)
 * and limit[19:16] was never written at all. */
/*
 * Low half of a 64-bit TSS descriptor.
 *
 * Byte 7 is base[31:28], NOT the high bits of the limit. It was built from
 * `(limit >> 16)`, so for a TSS of 107 bytes that field is zero -- and the real
 * base, 0xffffffff80029e20, needs 0xF there. Zeroing it produces a TSS base of
 * 0xffff8000_0029e20, which is **non-canonical**, and LTR rejects a
 * non-canonical TSS base with #GP carrying the TSS selector. That was the fault
 * the boot was dying on, immediately after the segment reload that the
 * GDT_ENTRY fix unblocked.
 *
 * Byte 6 holds limit[19:16] in bits 3:0 and the AVL/L/DB/G nibble in bits 7:4.
 * A TSS descriptor leaves G clear: the granularity bit means "the limit counts
 * 4 KiB pages", and a TSS limit does not. GDT_FLAG_GRANULARITY is a four-bit
 * nibble value now, so it must land at bit 52 as itself -- `<< 52`, not shifted
 * again.
 *
 * Byte 7 is base[31:28] -- FOUR bits. It was built as `(base >> 24) & 0xFF`,
 * an eight-bit field, which also double-counts base[27:24]: those bits already
 * reach byte 4 through `base & 0xFF0000 << 16`. For a kernel address the extra
 * four bits are 0x8, so byte 7 became 0x80 where the address needs 0xf, and
 * the reconstructed base -- 0xffff8000_0029e20 -- is **non-canonical**. LTR
 * rejects a non-canonical TSS base with #GP carrying the TSS selector, which is
 * the fault the boot was dying on.
 *
 * The static asserts below decode the result back into its fields so a mistake
 * here cannot be silent again: an access byte is the difference between a TSS
 * descriptor and a plain system segment, and the CPU reports that as #GP(0x30)
 * at an `ltr` with no further detail.
 */
#define GDT_TSS_DESC64_LOW(base, limit) \
    (((unsigned long long)((limit) & 0xFFFFULL)) \
     | (((unsigned long long)(base) & 0xFFFFULL) << 16) \
     | (((unsigned long long)(base) & 0xFF0000ULL) << 16) \
     | ((unsigned long long)GDT_TSS_ACCESS64 << 40) \
     | ((((unsigned long long)(limit) >> 16) & 0xFULL) << 48) \
     | ((((unsigned long long)(base) >> 24) & 0xFFULL) << 56))

/*
 * High half of a 64-bit TSS descriptor: base[63:32], and nothing else.
 *
 * The whole 64-bit base is *not* in these two macros. base[31:0] is spread
 * across bytes 2, 3, 4 and 7 of the low descriptor, and only base[63:32] comes
 * from this one. base[31:24] is byte 7 of the *low* descriptor -- it is the
 * last base field in the eight-byte layout, and adding a second copy here is
 * what produced a TSS base the CPU could not canonicalise.
 */
#define GDT_TSS_DESC64_HIGH(base) \
    (((unsigned long long)(base) >> 32) & 0xFFFFFFFFULL)

#ifndef __ASSEMBLER__
/*
 * Round-trip checks for the 64-bit TSS descriptor.
 *
 * A descriptor encoder is the one part of this header where a plausible mistake
 * is invisible in review and fatal at run time, because the CPU's only
 * complaint is #GP carrying a selector. These decode what the encoders above
 * produce and compare it with the address that went in, so a wrong shift is a
 * build failure rather than a boot loop.
 *
 * The helpers take (base, limit) rather than a finished descriptor because the
 * inner macro's argument list contains a comma, and the preprocessor splits
 * arguments before it expands: GDT_ACCESS_BYTE_OF(GDT_TSS_DESC64_LOW(b, l))
 * would be read as two arguments to a one-argument macro.
 */
#define GDT_TSS_BASE_OF(b) \
    (((GDT_TSS_DESC64_LOW(b, 107) >> 16) & 0xFFFFFFULL) \
    | (((GDT_TSS_DESC64_LOW(b, 107) >> 56) & 0xFFULL) << 24) \
    | (GDT_TSS_DESC64_HIGH(b) << 32))

#define GDT_TSS_ACCESS_OF(b, l) \
    ((GDT_TSS_DESC64_LOW(b, l) >> 40) & 0xFFULL)

_Static_assert(GDT_TSS_BASE_OF(0xffffffff80000000ULL) == 0xffffffff80000000ULL,
           "64-bit TSS descriptor does not round-trip its base");
_Static_assert(GDT_TSS_ACCESS_OF(0xffffffff80000000ULL, 107) == 0x89,
           "64-bit TSS descriptor must be type 9, an available 64-bit TSS");
#endif /* !__ASSEMBLER__ */


/* Descriptor selector values (index << 3 | RPL). */
#define KERNEL_CODE_SELECTOR   0x08
#define KERNEL_DATA_SELECTOR   0x10
#define LONG_CODE_SELECTOR     0x08

/* The ring-3 code selector that STAR[63:48] must hold. SYSRET adds 16 to it to
 * reach the descriptor user code actually runs on, and 8 to reach user data. Do
 * not "fix" this to 0x2B: syscall.c writes this value into STAR and the CPU does
 * the rest, so pointing it straight at 0x2B would make SYSRET return to ring 0. */
#define USER_CODE_SELECTOR     0x1B    /* index 3, RPL 3 — the STAR base */
#define USER_DATA_SELECTOR     0x23    /* index 4, RPL 3 */
#define USER_CODE64_SELECTOR   0x2B    /* index 5, RPL 3 — what SYSRET lands on */

/* The selector loaded into TR. It is a system selector, so RPL is always 0. */
#define TSS_SELECTOR           (GDT_INDEX_TSS << 3)

#endif /* GDT_H */
