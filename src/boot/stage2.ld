/*
 * stage2.ld — link script for the 32-bit second-stage bootloader.
 *
 * The image is loaded to physical 0x8000 by stage1 and executed in place, so
 * the link address must match the load address exactly. `.bss` follows `.data`
 * contiguously and is emitted explicitly because stage2_entry.S zeroes it by
 * symbol: a stage2 that was instead loaded through an ELF loader would need no
 * such thing, and the symbols would go away.
 */
OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(stage2_entry)

SECTIONS
{
	. = 0x8000;

	.text : { *(.text .text.*) }
	.rodata : { *(.rodata .rodata.*) }

	. = ALIGN(16);
	.data : { *(.data .data.*) *(.got .got.plt) *(.got) }

	. = ALIGN(16);
	__bss_start = .;
	.bss : {
		*(.dynbss)
		*(.bss .bss.*)
		*(COMMON)
		. = ALIGN(16);
	}
	__bss_end = .;

	/* stage2's .bss must not reach the BIOS transfer window.
	 *
	 * 0x10000 is where the bounce buffer, the disk address packet and the VBE
	 * scratch block live, and the linker places .bss straight after .rodata
	 * with nothing reserving the gap. A stack that grew past the boundary
	 * would overwrite all three, and the symptom would be a disk transfer
	 * that appears to succeed and then hands back whatever the stack left
	 * there -- an ELF header full of return addresses. Nothing at run time can
	 * distinguish that from a corrupt image, so it is a link-time error
	 * instead. */
	ASSERT(__bss_end <= 0x10000,
	       "stage2 .bss overlaps the BIOS transfer window at 0x10000")

	/* The BIOS trampoline encodes the vector slot's address into an `int`
	 * instruction as a 16-bit offset, so the slot has to be reachable below
	 * 64 KiB with CS = 0. Above that it would not wrap to a harmless offset --
	 * it would name a completely different low address, and the call would
	 * reach whatever interrupt vector the word there happens to contain. The
	 * assembler only warns about the truncation, so the bound is enforced
	 * here, where it becomes a link error. */
	ASSERT((0x00005000 & 0xFFFF) == 0x00005000,
	       "BIOS_VECTOR_ADDR must be below 64 KiB")
	ASSERT((0x00005004 & 0xFFFF) == 0x00005004,
	       "BIOS_IVT_PTR must be below 64 KiB")

	/* The trampoline's own state is reached from the 16-bit side of the round
	 * trip, where the default address size is 16 bits. Every access to it is
	 * an absolute displacement, so a symbol above 64 KiB would be truncated
	 * rather than rejected: the real-mode code would read and write some
	 * unrelated low address, and nothing would fault. The encodings in
	 * stage2_entry.S force a 32-bit address, which makes the linker emit a
	 * four-byte relocation, so this bound is about the linear addresses being
	 * reachable at all rather than about silent truncation -- but it is the
	 * same bound, and the reason it exists. */
	ASSERT(bios_saved_eax < 0x10000, "trampoline state must be below 64 KiB")
	ASSERT(bios_saved_esp < 0x10000, "trampoline state must be below 64 KiB")
	ASSERT(bios_saved_eax + 4 <= 0x10000, "trampoline state must be below 64 KiB")
	ASSERT(bios_ret_ebx + 4 <= 0x10000, "trampoline state must be below 64 KiB")
	ASSERT(bios_es_seg + 2 <= 0x10000, "trampoline state must be below 64 KiB")

	/* The slot also has to stay out of the regions the boot path owns. A
	 * collision is silent -- the firmware, stage1 or stage2 would simply
	 * overwrite the vector number between the store and the `int`. */
	ASSERT(0x00005000 >= 0x00000500,
	       "BIOS_VECTOR_ADDR overlaps the IVT and BIOS data area")
	ASSERT(0x00005000 < 0x00007C00,
	       "BIOS_VECTOR_ADDR overlaps stage1 or the real-mode stack")

	/DISCARD/ : {
		*(.comment)
		*(.note*)
		*(.eh_frame*)
		*(.gnu.hash)
		*(.hash)
	}
}

/* Names stage2_entry.S and stage2.c refer to. */
bss_start = __bss_start;
bss_end   = __bss_end;

/*
 * The two halves of the exception stub's address, for the IDT gate.
 *
 * An interrupt gate stores its handler as a 16-bit low half and a 16-bit high
 * half with the selector and type attributes between them, so the address has
 * to be split. That cannot be done in the assembler: exc_stub's absolute value
 * is only known after the sections have been placed, and GAS rejects bit
 * operations on a symbol it has not resolved. Defining the halves here defers
 * the split to the linker, which does know the final addresses, and lets the
 * assembly emit four plain `.word`s with no arithmetic at all.
 */
exc_stub_lo = exc_stub & 0xFFFF;
exc_stub_hi = (exc_stub >> 16) & 0xFFFF;
