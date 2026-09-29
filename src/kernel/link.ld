/*
 * kernel.ld — link script for the higher-half kernel.
 *
 * The kernel is linked at 0xFFFFFFFF80000000 and executed from there. stage2
 * copies the ELF image to the low landing zone and maps it twice: once
 * identity, once at this address. Nothing in the kernel ever depends on the
 * identity view, which means a stray physical pointer cannot silently work.
 *
 * The layout is explicit rather than relying on the default script's
 * page-alignment between sections. Startup code computes addresses relative to
 * the link-time symbols below, so the gaps between .text, .rodata and .data are
 * part of the interface rather than an accident of alignment.
 */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(kernel_start)

/*
 * The higher-half base. Defined here rather than passed in with --defsym so
 * the link script is self-contained: readelf on the output already reports the
 * virtual addresses, and there is no second place to keep in sync with
 * src/include/boot.h.
 */
KERNEL_VIRT_BASE = 0xFFFFFFFF80000000;

/* Depth of the boot stack reserved in .bss. See the .bss section for why. */
KERNEL_STACK_SIZE = 0x8000;

/* Identifiers for the .note sections carrying build provenance. */
KERNEL_PROVENANCE = 0x4f53424f4f45524e;

SECTIONS
{
	. = KERNEL_VIRT_BASE + SIZEOF_HEADERS;

	.text : {
		_stext = .;
		/* The entry stub lives in its own section so it is guaranteed to be
		 * at the first executable address. kmain.S clobbers every register
		 * before touching any of them, so nothing may sit in front of it. */
		*(.text.boot)
		*(.text .text.*)
		_etext = .;
	}


	. = ALIGN(4096);
	.rodata : {
		_rodata = .;
		*(.rodata .rodata.*)
		_erodata = .;
	}

	/* Build provenance: toolchain versions, build host, build id. Read by the
	 * kernel's version syscall and the debug subsystem. */
	.note : {
		. = ALIGN(8);
		*(.note.gnu.build-id)
		*(.note.*)
	}

	. = ALIGN(4096);
	.data : {
		_data = .;
		*(.data .data.*)
		*(.got .got.plt)
		_ = .;
		PROVIDE(__global_pointer$ = . + 0x800);
		*(.sdata .sdata.*)
		_edata = .;
	}

	. = ALIGN(4096);
	.bss : {
		_bss = .;
		*(.dynbss)
		*(.bss .bss.*)
		*(COMMON)

		/*
		 * The boot stack. Reserved inside .bss rather than in a separate
		 * section so that stage2's BSS zeroing covers it: the bootloader
		 * cannot know this kernel's BSS size, so it zeroes the whole landing
		 * zone instead, and a stack outside .bss would be filled with
		 * garbage on the way past.
		 *
		 * 32 KiB is the depth the panic backtrace and the syscall path are
		 * sized for. It is not a general-purpose per-CPU stack — those are
		 * allocated by kstack_alloc() once the allocator is up, because
		 * interrupt handlers must not share a stack with the task they
		 * interrupt.
		 */
		. = ALIGN(4096);
		__kernel_stack_bottom = .;
		. += KERNEL_STACK_SIZE;
		__kernel_stack_top = .;

		. = ALIGN(4096);
		_ebss = .;
	}

	/* Discarded deliberately: unwind tables describe kernel frames in terms
	 * of the kernel's own text, and there is no unwinder in the boot path.
	 * Leaving them in would bloat the image past the landing-zone budget. */
	/DISCARD/ : {
		*(.eh_frame*)
		*(.comment)
		*(.note.GNU-stack)
	}
}

/* The kernel binary is limited to the boot landing-zone budget. Catching this
 * at link time is far cheaper than discovering it as a truncated boot. */
ASSERT(ADDR(.bss) - KERNEL_VIRT_BASE < 0x50000,
       "kernel exceeds the stage2 landing zone budget")
