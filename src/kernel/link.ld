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

/*
 * The hard ceiling on the loaded image, mirroring KERNEL_MAX_BYTES in
 * src/boot/boot_layout.h. stage2 refuses to copy a PT_LOAD whose
 * p_offset + p_memsz exceeds it (stage2.c: phdr.p_offset + phdr.p_memsz >
 * KERNEL_MAX_BYTES), so this number is the real contract: the bootloader
 * zeroes and the kernel page-tables the landing zone, and nothing may grow
 * past it. Keep the two in sync.
 *
 * The value is not negotiable and must not be raised to make an ASSERT go
 * away. A larger image means a larger landing zone, and the landing zone
 * ends at BOOT_PT_ADDR -- there is no room to grow into.
 */
KERNEL_MAX_BYTES = 0x1D0000;

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

/*
 * The kernel binary is limited to the boot landing-zone budget. Catching this
 * at link time is far cheaper than discovering it as a truncated boot.
 *
 * The test is on _ebss, not on ADDR(.bss).
 *
 * .bss is NOBITS: it occupies no file bytes, but stage2 still has to zero it
 * and the kernel still has to map it, so it is very much part of the loaded
 * image. ADDR(.bss) is only the *start* of that region. Testing the start
 * measured nothing: .bss begins at offset 0x44000 and the old limit was
 * 0x50000, so the assert passed while the image actually reached 0x1C8000 --
 * 1.78 MiB, nearly six times the number being checked, and within 32 KiB of
 * the real limit. An assert that cannot fail is worse than no assert, because
 * it reads as a guarantee.
 *
 * _ebss is the first byte past the end of .bss (after the ALIGN(4096) at the
 * end of the section), so it is the true loaded extent from the ELF header to
 * the top of the zeroed region, and it is the quantity stage2's own
 * p_offset + p_memsz check bounds.
 */
ASSERT(_ebss - KERNEL_VIRT_BASE <= KERNEL_MAX_BYTES,
       "kernel exceeds the stage2 landing zone budget (KERNEL_MAX_BYTES)")

/*
 * The stack is the last thing in .bss and grows down from _ebss, so a stack
 * overflow runs off the end of the mapped image into whatever the loader left
 * there. The ASSERT below is not a stack-depth check -- nothing at link time
 * can know that -- it only guarantees the stack top is inside the region
 * stage2 zeroed, so a runaway stack faults on an unmapped page instead of
 * silently corrupting the loader's data. __kernel_stack_top == _ebss by
 * construction; the assert exists to fail loudly if a future edit moves the
 * stack out of .bss, which would take the zeroing guarantee with it.
 */
ASSERT(__kernel_stack_top == _ebss,
       "the boot stack must end at _ebss so stage2's BSS zeroing covers it")
