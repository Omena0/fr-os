/*
 * boot.h — the handoff record between stage2 and the kernel.
 *
 * stage2 fills this in at BOOTINFO_ADDR in low physical memory and passes its
 * physical address to the kernel entry point in RDI. Every field is fixed
 * width and little-endian, matching the x86-64 ABI, so the kernel can read it
 * directly through the identity map the bootstrap page tables provide.
 *
 * The structure is a report, not a contract. The kernel re-derives everything it
 * depends on: it validates the magic and version, re-checks every E820 range
 * before reserving it, and builds its own page tables from its own allocator
 * before trusting any virtual address. A corrupt or hostile stage2 can waste
 * some of the kernel's boot time, but it cannot point it at memory the kernel
 * has not independently accounted for.
 */
#ifndef BOOT_H
#define BOOT_H

#define BOOTINFO_MAGIC   0x4F53424F4F543031ULL  /* "OSBOOT01" */
#define BOOTINFO_VERSION 1

/*
 * Where stage2 leaves the handoff record and the memory map, in low physical
 * memory. Both sides need these and neither can see the other's symbols, so
 * they are declared once, here.
 *
 * They were previously defined separately in src/boot/boot_layout.h and again
 * as a literal in src/kernel/kmain.S, and the kernel's copy was wrong: it held
 * E820_ADDR rather than BOOTINFO_ADDR, one 4 KiB page below the record. So
 * kmain validated the magic against the first bytes of the memory map, always
 * failed, and panicked -- after RDI had been handed the correct address by
 * stage2 and then overwritten with the wrong constant on the way in.
 */
#define E820_ADDR       0x00090000ULL
#define BOOTINFO_ADDR   0x00091000ULL

/* Kernel virtual layout, shared with the kernel linker script. */
#define KERNEL_VIRT_BASE 0xFFFFFFFF80000000ULL
#define KERNEL_VIRT_END  0xFFFFFFFFC0000000ULL
#define PHYS_DIRECT_MAP   0xFFFF800000000000ULL  /* physical memory, identity */
#define VMALLOC_AREA      0xFFFFC00000000000ULL
#define KASLR_SLIDE_BITS  28

/*
 * Where stage2 put the kernel image, and how that relates to where it was
 * linked. The two are not the same offset, and the difference is 1 MiB.
 *
 * INT 13h cannot address above 1 MiB, so the image is copied into the landing
 * zone before paging exists, and the loader's higher-half tables map virtual
 * KERNEL_VIRT_BASE there rather than at physical zero. Every higher-half
 * address therefore translates as
 *
 *     phys = (virt - KERNEL_VIRT_BASE) + KERNEL_LANDING_ADDR
 *
 * Subtracting KERNEL_VIRT_BASE on its own looks right -- the window and the
 * image do share a base, virtually -- and puts the result exactly 1 MiB below
 * the truth. Declared here rather than in boot_layout.h because the kernel
 * needs it for that translation too, and two copies of the number is one copy
 * too many.
 */
#define KERNEL_LANDING_ADDR 0x00100000ULL

/*
 * 4-level page-table indices, plus the two the kernel's own link address walks
 * through.
 *
 * The loader builds a higher-half mapping and the kernel replaces it with its
 * own; both must fill exactly the same entries. The kernel used to write
 * PDPT 511 where the loader wrote 510, which is not a crash at the write and
 * not even a bad table: 511 names a real 1 TiB region, so every read-back
 * check passes and the first high-half access faults instead.
 *
 * 0xFFFFFFFF80000000 is 2^63 - 2^31, so the PML4 index is 511 and the address
 * starts the *second* 1 TiB region of the negative half, making the PDPT index
 * 510. The two numbers that come to mind for "the higher half" are both wrong:
 * 256 is the first address of the higher canonical half, 512 GiB below the
 * kernel, and 508 is the 512 GiB region -- while the kernel is in the 1 TiB
 * one, so 510. Deriving both indices from KERNEL_VIRT_BASE means the two
 * halves of the boot chain cannot drift apart again.
 */
#define PML4_ENTRY_OF(addr)  (((addr) >> 39) & 0x1FF)
#define PDPT_ENTRY_OF(addr)  (((addr) >> 30) & 0x1FF)
#define PD_ENTRY_OF(addr)    (((addr) >> 21) & 0x1FF)
#define PT_ENTRY_OF(addr)    (((addr) >> 12) & 0x1FF)

#define KERNEL_PML4_IDX  PML4_ENTRY_OF(KERNEL_VIRT_BASE)
#define KERNEL_PDPT_IDX  PDPT_ENTRY_OF(KERNEL_VIRT_BASE)

/* E820 memory types. */
#define E820_USABLE       1
#define E820_RESERVED     2
#define E820_ACPI_RECLAIM 3
#define E820_ACPI_NVS     4
#define E820_BAD          5

/* A single ACPI 3.0 memory map entry. */
#ifndef __ASSEMBLER__
#include <stdint.h>

struct e820_entry {
	uint64_t base;
	uint64_t length;
	uint32_t type;
	uint32_t acpi_extended;
} __attribute__((packed));

/*
 * Framebuffer description. The shift values are byte offsets into a pixel, not
 * bit positions, because every framebuffer this runs against packs 32-bit
 * pixels; the kernel converts to bit shifts itself. Keeping the raw VBE mask
 * positions here as well would invite the two to disagree.
 */
struct framebuffer_info {
	uint64_t address;
	uint32_t pitch;
	uint32_t width;
	uint32_t height;
	uint8_t  bpp;
	uint8_t  red_shift;
	uint8_t  green_shift;
	uint8_t  blue_shift;
} __attribute__((packed));

/* Bits in struct bootinfo::flags. */
#define BOOT_FLAG_HAS_FRAMEBUFFER (1u << 0)
#define BOOT_FLAG_HAS_ACPI        (1u << 1)

struct bootinfo {
	uint64_t magic;
	uint32_t version;
	uint32_t flags;

	uint64_t kernel_phys_base;    /* where stage2 copied the ELF image */
	uint64_t kernel_virt_base;    /* where the kernel was linked */
	uint64_t kernel_entry;        /* ELF entry point, virtual */

	uint64_t e820_addr;           /* physical address of the E820 array */
	uint32_t e820_count;
	uint32_t _pad0;

	struct framebuffer_info fb;
	uint64_t rsdp_addr;
	uint64_t acpi_version;

	uint64_t boot_drive;
	char     cmdline[128];
} __attribute__((packed));
#endif /* !__ASSEMBLER__ */

#endif /* BOOT_H */
