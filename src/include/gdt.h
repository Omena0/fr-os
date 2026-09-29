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
#define GDT_FLAG_GRANULARITY  (1 << 7)  /* limit is scaled by 4096 */
#define GDT_FLAG_64BIT        (1 << 5)  /* L bit: 64-bit code segment */
#define GDT_FLAG_DB           (1 << 6)  /* B bit: 32-bit, default operand size */
#define GDT_FLAG_LONG_MODE    (1 << 5)

/* Compose a descriptor. `limit` is in bytes; with GRANULARITY it is in 4 KiB
 * pages. `base` is the 64-bit linear base. */
#define GDT_ENTRY(flags, base, limit) \
	(((uint64_t)(limit) & 0xFFFFULL) \
	 | (((uint64_t)(base) & 0xFFFFFFULL) << 16) \
	 | (((uint64_t)(flags) & 0xFFULL) << 40) \
	 | (((uint64_t)(base) >> 24) & 0xFFULL) << 56)

/* The canonical flat segments used by both stages. A limit of 0xFFFFF with a
 * 4 KiB granularity gives exactly 2^47 bytes, which is every canonical address
 * on a 48-bit virtual machine. */
#define GDT_CODE64_FLAGS \
	(GDT_ACCESS_PRESENT | GDT_ACCESS_RING0 | GDT_ACCESS_SEGMENT | \
	 GDT_ACCESS_CODE | GDT_ACCESS_EXEC | GDT_FLAG_GRANULARITY | GDT_FLAG_LONG_MODE)

#define GDT_DATA_FLAGS \
	(GDT_ACCESS_PRESENT | GDT_ACCESS_RING0 | GDT_ACCESS_SEGMENT | \
	 GDT_ACCESS_DATA | GDT_ACCESS_RW | GDT_ACCESS_ACCESSED | \
	 GDT_FLAG_GRANULARITY | GDT_FLAG_DB)

#define GDT_CODE32_FLAGS \
	(GDT_ACCESS_PRESENT | GDT_ACCESS_RING0 | GDT_ACCESS_SEGMENT | \
	 GDT_ACCESS_CODE | GDT_ACCESS_EXEC | GDT_ACCESS_ACCESSED | \
	 GDT_FLAG_GRANULARITY | GDT_FLAG_DB)

#define GDT_USER_CODE64_FLAGS \
	(GDT_ACCESS_PRESENT | GDT_ACCESS_RING3 | GDT_ACCESS_SEGMENT | \
	 GDT_ACCESS_CODE | GDT_ACCESS_EXEC | GDT_ACCESS_ACCESSED | \
	 GDT_FLAG_GRANULARITY | GDT_FLAG_LONG_MODE)

#define GDT_USER_DATA_FLAGS \
	(GDT_ACCESS_PRESENT | GDT_ACCESS_RING3 | GDT_ACCESS_SEGMENT | \
	 GDT_ACCESS_DATA | GDT_ACCESS_RW | GDT_ACCESS_ACCESSED | \
	 GDT_FLAG_GRANULARITY | GDT_FLAG_DB)

#define GDT_USER_CODE32_FLAGS \
	(GDT_ACCESS_PRESENT | GDT_ACCESS_RING3 | GDT_ACCESS_SEGMENT | \
	 GDT_ACCESS_CODE | GDT_ACCESS_EXEC | GDT_ACCESS_ACCESSED | \
	 GDT_FLAG_GRANULARITY | GDT_FLAG_DB)

#define GDT_LCODE64  GDT_ENTRY(GDT_CODE64_FLAGS, 0, 0xFFFFF)
#define GDT_LDATA64  GDT_ENTRY(GDT_DATA_FLAGS,  0, 0xFFFFF)
#define GDT_LCODE32  GDT_ENTRY(GDT_CODE32_FLAGS, 0, 0xFFFFF)
#define GDT_LDATA32  GDT_ENTRY(GDT_DATA_FLAGS,  0, 0xFFFFF)
#define GDT_LUSER64  GDT_ENTRY(GDT_USER_CODE64_FLAGS, 0, 0xFFFFF)
#define GDT_LUSERDATA GDT_ENTRY(GDT_USER_DATA_FLAGS, 0, 0xFFFFF)

/* Index 3: a 32-bit ring-3 code descriptor that user code never executes on.
 * It exists only so SYSRET's arithmetic lands in the right place — see
 * "Selector Arithmetic" in docs/src/kernel/privilege-levels.md. SYSRET computes
 * CS = STAR[63:48] + 16, and this is the descriptor STAR[63:48] names. */
#define GDT_LUSERCODE32 GDT_ENTRY(GDT_USER_CODE32_FLAGS, 0, 0xFFFFF)

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
