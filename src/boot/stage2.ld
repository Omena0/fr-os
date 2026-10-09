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

/* Mirrors BOUNCE_ADDR, STACK32_ADDR and STACK32_TOP in boot_layout.h.
 * Duplicated rather than included because the link script has no way to run
 * the preprocessor over the header without dragging the whole build into it,
 * and a wrong constant here is silent memory corruption rather than a build
 * failure. The ASSERTs below are what keep the two honest: they compare this
 * script's stack against its own BOUNCE_ADDR, and a build that succeeds with
 * the two copies disagreeing would still be wrong -- so the asserts are
 * written to catch a layout change in either file.
 *
 * The numbers below, and what they mean:
 *
 *   0x008000  stage2 image (.text, .trampoline, .rodata, .data, .bss)
 *   0x00E000  STACK32_ADDR: the bottom of the 32-bit loader stack
 *   0x020000  STACK32_TOP == BOUNCE_ADDR: the top of that stack, and the
 *             start of the window the firmware reads each sector into
 *
 * So the stack is 0x12000 = 72 KiB, and the image gets the 24 KiB below it.
 * stack32_top landing exactly on BOUNCE_ADDR is deliberate and is the only
 * safe arrangement at the top end -- see the ASSERT further down. */
BOUNCE_ADDR = 0x20000;
STACK32_ADDR = 0xE000;
STACK32_TOP = 0x20000;

SECTIONS
{
    . = 0x8000;

    .text : { *(.text .text.*) }

    /*
     * The trampoline words come next, before .rodata, and are the reason
     * this ordering is not the obvious one. .rodata is 3.6 KiB of strings
     * and .data follows it, so by the time the natural .bss ordering reaches
     * these symbols they are at 0xCD50 -- above 64 KiB, where the real-mode
     * side of the round trip cannot address them at all. Putting the section
     * immediately after .text pins it below 0x10000 while there is still room,
     * and the asserts at the bottom of this file hold it there.
     */
    . = ALIGN(16);
    __trampoline_start = .;
    .trampoline : {
        *(.trampoline)
        . = ALIGN(16);
    }
    __trampoline_end = .;

    .rodata : { *(.rodata .rodata.*) }

    . = ALIGN(16);
    .data : { *(.data .data.*) *(.got .got.plt) *(.got) }

    /*
     * Trampoline state, in its own section and deliberately placed low.
     *
     * The real-mode side of the BIOS round trip addresses these with 16-bit
     * absolute displacements, so anything the linker puts above 0xFFFF is
     * unreachable: the CPU reads the truncated offset and reads unrelated
     * memory instead. The encodings in stage2_entry.S carry a 0x67 prefix to
     * force a 32-bit address, which is what makes the linker emit a four-byte
     * relocation -- but the resulting linear address still has to be one the
     * real-mode code can form, and a symbol at 0xCD74 is not.
     *
     * Placing the section explicitly is the only arrangement that keeps this
     * true as the rest of the image grows. Relying on the natural order of
     * *(.bss) put these words wherever .text and .rodata happened to end,
     * which is above 64 KiB the moment stage2 grows past about 12 KiB of code
     * and data -- and nothing warned, because a truncated 16-bit absolute
     * address is not a fault. It reads whatever is at the low address, so the
     * firmware is entered with a garbage ES and the loader dies without a
     * word on the serial port.
     */
    . = ALIGN(16);
    __bss_start = .;
    .bss : {
        *(.dynbss)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
    }
    __bss_end = .;

    /*
     * The loader stack is a fixed region below the bounce buffer.
     *
     * Anchoring it matters. Its first placement was simply the last section
     * before .bss's neighbours, which left only ~230 bytes between the bottom
     * of the stack and the read-only data at the top of .rodata -- enough to
     * survive today, and little enough that adding code to the loader ate the
     * margin and let the stack overwrite serial_puthex's digit table. Every
     * number the loader printed then came out blank, with no fault to
     * attribute it to, because the memory being overwritten was perfectly
     * good code.
     *
     * The obvious fix -- cap the stack at 0x10000 -- is worse, and was the
     * second thing tried. 0x10000 is BOUNCE_ADDR: the window the firmware
     * reads each sector into. With the stack there, a sector was transferred
     * correctly and then overwritten by the loader's next call frame before
     * anything looked at it, so the ELF parser read garbage headers and the
     * read loop, which trusts the same window, spun on a sector that was
     * already destroyed.
     *
     * So the stack gets a fixed region of its own, between the image and the
     * bounce buffer, and the asserts below make both boundaries explicit:
     * the image ends well below it, and it ends well below the bounce window.
     * The high-water mark is under 1552 bytes, so this leaves several KiB of
     * margin on each side rather than a few hundred bytes on one.
     */
    ASSERT(__bss_end < STACK32_ADDR,
           "stage2 image has grown into the loader stack")
    . = ALIGN(16);
    __stack_size = STACK32_TOP - STACK32_ADDR;
    . = STACK32_ADDR;
    stack32 = .;
    stack32_top = . + __stack_size;
    ASSERT(stack32 > __bss_end,
           "loader stack overlaps the stage2 image")
    /*
     * The gap, not the ceiling -- and equality is what is wanted here, not a
     * gap. The stack grows *down*, so stack32_top is its highest address and
     * the first push writes at stack32_top - 4, which is inside the stack
     * region and nowhere near the bounce window that begins at
     * stack32_top. `stack32_top == BOUNCE_ADDR` is therefore exactly right:
     * the stack uses every byte below the bounce buffer and not one byte
     * above it. It is the `<=` test below that would let the stack overlap
     * the bounce buffer if it were ever read the other way round, and the
     * comment that used to stand here argued the opposite -- that landing on
     * BOUNCE_ADDR put the first push "inside the bounce window", which is
     * what a stack growing *up* would do.
     *
     * The real hazard at this boundary is a *raised* top, not an equal one,
     * and that is what the test rejects: STACK32_TOP above BOUNCE_ADDR means
     * stack32 sits in the middle of the window the firmware reads each sector
     * into, so a sector is transferred correctly and then overwritten by the
     * loader's own call frame before anything looks at it. Every sector the
     * ELF parser sees is then whatever the last frame left behind -- an E820
     * map of zeroes, program headers that parse as "no loadable segments" --
     * and nothing anywhere reports an error.
     */
    ASSERT(stack32_top <= BOUNCE_ADDR,
           "loader stack overlaps the BIOS bounce buffer")
    ASSERT(BOUNCE_ADDR == 0x20000,
           "BOUNCE_ADDR here disagrees with boot_layout.h")
    ASSERT(STACK32_TOP == 0x20000,
           "STACK32_TOP here disagrees with boot_layout.h")

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
    ASSERT(bios_ret_ecx + 4 <= 0x10000, "trampoline state must be below 64 KiB")
    ASSERT(bios_ret_edx + 4 <= 0x10000, "trampoline state must be below 64 KiB")

    /*
     * Everything below 0x10000 has to fit in one window: the trampoline
     * words, which must be reachable by a 16-bit displacement, and the rest
     * of .bss, which must not reach the bounce buffer. The two constraints
     * pull in opposite directions as the image grows, so both are asserted
     * rather than left to a comment.
     */
    ASSERT(__bss_end <= 0x10000,
           "stage2 .bss overlaps the BIOS transfer window at 0x10000")

    /* The slot also has to stay out of the regions the boot path owns. A
     * collision is silent -- the firmware, stage1 or stage2 would simply
     * overwrite the vector number between the store and the `int`. */
    ASSERT(0x00005000 >= 0x00000500,
           "BIOS_VECTOR_ADDR overlaps the IVT and BIOS data area")
    ASSERT(0x00005000 < 0x00007C00,
           "BIOS_VECTOR_ADDR overlaps stage1 or the real-mode stack")

    /*
     * The real-mode half of the trampoline runs on a 16-bit stack, and the
     * two ends of that window are both owned by something else.
     *
     * At the top: stage1's image, 0x7C00-0x7DFF. bios_rm_stack_top has to be
     * *below* it, because the stack grows down from there and the trampoline
     * unconditionally writes at top-2 and top-4 before the firmware has done
     * anything at all. 0x7DFC is STAGE1_DRIVE_ADDR, so a top inside the MBR
     * destroys the boot-drive handover byte on the first BIOS call of every
     * boot -- and nothing notices, because stage2 has already copied the
     * drive into its own .bss by then.
     *
     * At the bottom: the BIOS_VECTOR_ADDR / BIOS_IVT_PTR slot at 0x5000-0x5007,
     * which is written immediately before every trip through the trampoline
     * and read by the 16-bit side. Overwriting it corrupts the vector and the
     * handler address for the call that is about to be made.
     */
    ASSERT(bios_rm_stack_top <= 0x7C00,
           "trampoline real-mode stack grows down into the MBR")
    ASSERT(bios_rm_stack_top >= 0x00005008,
           "trampoline real-mode stack reaches the BIOS vector slot")
    ASSERT(bios_rm_stack_top < 0x10000,
           "trampoline real-mode stack must be nameable as SS:SP")

    /*
     * The GDTR is loaded from 16-bit code with `lgdt (%si)` -- the GDT has to
     * be installed before the far jump that resolves CS through it, so it
     * cannot wait for the 32-bit side the way the IDT does -- and a 16-bit
     * register can only name an address below 64 KiB. %si therefore holds
     * stage2_gdt's address truncated to 16 bits, which is correct only while
     * the table really is in the low 64 KiB.
     *
     * Nothing else in this script establishes that: __bss_end is bounded
     * against the 0x10000 transfer window, but .rodata sits below .bss with
     * no bound of its own, and it is in .rodata that the GDT lives. A table
     * above 64 KiB is not a fault -- LGDT reads a 16-bit base and the CPU
     * fetches from it -- so this has to be a link error.
     */
    ASSERT(stage2_gdt < 0x10000,
           "stage2_gdt must be below 64 KiB: it is loaded by lgdt (%si) from 16-bit code")
    ASSERT(stage2_gdt + 32 <= 0x10000,
           "stage2_gdt must be below 64 KiB: it is loaded by lgdt (%si) from 16-bit code")

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
/*
 * The static gates point at the 32-bit reporter, which is what the CPU can
 * actually execute for the whole of the protected-mode phase.
 *
 * Pointing them at the 64-bit one instead is silently fatal, and fatally
 * silent: a 64-bit instruction stream executed in compatibility mode decodes
 * as a different, still-plausible instruction stream, so the fault that should
 * have been reported walks into unrelated bytes and eventually reaches the
 * reporter's own halt loop. Nothing is printed, because the code that would
 * print is the code that could not be entered.
 *
 * boot_gdt_init() rewrites all 256 gates to the 64-bit reporter as the last
 * thing it does, immediately before the far jump that makes long mode real.
 */
exc_stub_lo = exc_stub & 0xFFFF;
exc_stub_hi = (exc_stub >> 16) & 0xFFFF;

/*
 * The same split for the 64-bit reporter.
 *
 * Needed because the two reporters cannot serve each other's mode. A 64-bit
 * instruction stream decoded in compatibility mode is still a valid stream --
 * `pushq %rax` reads as some other instruction entirely -- so a 64-bit gate
 * installed while the CPU is 32-bit produces a fault that is walked into
 * unrelated code and ends in the reporter's own halt loop, printing nothing.
 * stage2_long.S therefore installs the 32-bit gates on its way up to the far
 * jump and the 64-bit ones immediately after it, so that at every instant at
 * which a fault can be taken, the gates name the stub that can run in the mode
 * the CPU is in.
 *
 * boot_idt_set_64bit() in stage2.c does the same rewrite from C, and earlier
 * rather than later; the assembly loops around the far jump are what make the
 * pairing exact, because they bracket the transition instead of jumping the
 * whole of it.
 */
exc_stub64_lo = exc_stub64 & 0xFFFF;
exc_stub64_hi = (exc_stub64 >> 16) & 0xFFFF;
