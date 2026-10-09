/*
 * stage1.ld — link script for the 512-byte Master Boot Record.
 *
 * The BIOS loads exactly 512 bytes at 0x7C00, so every section has to land
 * contiguously with no page alignment or gap. The default script inserts a
 * MAXPAGESIZE alignment between .text and .rodata, which pushes the string
 * data past offset 512 and produces an image the firmware will not load.
 *
 * KEEP() on each output section stops --gc-sections from discarding the MBR
 * signature tail, which nothing references.
 */
OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(_start)

SECTIONS
{
    . = 0x7C00;

    .boot : {
        *(.text)
        *(.rodata)
        *(.data)
        *(.bss)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
