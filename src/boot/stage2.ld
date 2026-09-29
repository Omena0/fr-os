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
