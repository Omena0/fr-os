/*
 * gdt.h — segment descriptor encodings shared by stage2 and the kernel.
 *
 * These are the raw bit patterns for the x86-64 GDT entries. Both the stage2
 * loader and the kernel build their GDTs from them, so the privilege levels and
 * long-mode flags cannot drift apart between the two.
 */
#ifndef GDT_H
#define GDT_H

/* Access byte bits. */
#define GDT_ACCESS_PRESENT    (1 << 7)
#define GDT_ACCESS_RING0      (0 << 6)
#define GDT_ACCESS_RING3      (3 << 6)
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
 * The same collapse destroyed every DPL, because the flag constants alias the
 * access byte's own bits: GDT_FLAG_64BIT/LONG_MODE is 1<<5, which is DPL bit 1,
 * and GDT_FLAG_DB is 1<<6, which is DPL bit 0. GDT_ACCESS_RING3 is (3<<6),
 * giving DPL 2 rather than 3. So the user descriptors were DPL 1 or 2, and a
 * SYSRET to them would have #GP'd immediately after the first fault was fixed.
 *
 * That is why the #GP survived three different far-return frame shapes: the
 * frame was never the problem.
 */
#define GDT_ENTRY(access, flags, base, limit) \
	(((uint64_t)(limit) & 0xFFFFULL) \
	 | (((uint64_t)(base) & 0xFFFFFFULL) << 16) \
	 | (((uint64_t)(access) & 0xFFULL) << 40) \
	 | ((((uint64_t)(limit) >> 16) & 0xFULL) << 48) \
	 | (((uint64_t)(flags) & 0xFULL) << 52) \
	 | ((((uint64_t)(base) >> 24) & 0xFFULL) << 56))

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
 * other entries in the table rather than left as a silent difference. */
#define GDT_TSS_DESC64_LOW(base, limit) \
	(((uint64_t)((limit) & 0xFFFFULL)) \
	 | (((uint64_t)(base) & 0xFFFFULL) << 16) \
	 | (((uint64_t)(base) & 0xFF0000ULL) << 16) \
	 | ((uint64_t)GDT_TSS_ACCESS64 << 40) \
	 | ((uint64_t)GDT_FLAG_GRANULARITY << 48) \
	 | (((uint64_t)((limit) >> 16) & 0xFFULL) << 56))

#define GDT_TSS_DESC64_HIGH(base) \
	((((uint64_t)(base) >> 24) & 0xFFFFFFFFULL) \
	 | (((uint64_t)(base) >> 32) & 0xFFFFFFFFULL) << 32)

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
