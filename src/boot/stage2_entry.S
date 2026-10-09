/*
 * stage2_entry.S — real mode to 32-bit protected mode transition.
 *
 * stage1 far-jumps here at 0x8000 in 16-bit real mode with interrupts
 * disabled and the boot drive number pushed on the stack. This stub loads a
 * GDT, sets CR0.PE, far-jumps into a 32-bit code segment, clears the BSS, and
 * calls the C loader.
 *
 * The transition is a far jump rather than a far call on purpose: ljmp loads
 * CS from the GDTR and abandons the 16-bit code path entirely, so no return
 * address is left pointing into a segment that no longer exists. Nothing in
 * stage2 or the kernel ever returns to real mode.
 */
    .code16
    .section .text
#include "boot_layout.h"

/*
 * Stack top for the real-mode half of the BIOS trampoline.
 *
 * An absolute symbol rather than a use of BIOS_RM_STACK_TOP, because that
 * macro's value is a trap: 0x7E00 is inside stage1's own image and the
 * trampoline's pushfw + far call write over the boot-drive handover byte. See
 * the note on the `movw` in bios_trampoline for the whole story; the short
 * version is that a downward-growing stack at 0x7E00 eats 0x7DFC, which is
 * STAGE1_DRIVE_ADDR.
 *
 * 0x7B00 is the top of the only low memory nothing claims -- above the BIOS
 * data area and the 0x5000 vector slot, below the MBR -- which leaves 10 KiB
 * for the firmware's register save and interrupt frame. stage2.ld asserts both
 * ends of that window, so neither a larger MBR nor a relocated slot can make
 * this silently wrong.
 */
    .equ    bios_rm_stack_top, 0x7B00
    .globl    bios_rm_stack_top

    .globl stage2_entry
stage2_entry:
    cli
    cld

    /* Set up a stack in low memory that stage1's image does not overlap.
     *
     * Nothing is on the stack to preserve: the boot drive travels in a fixed
     * byte inside stage1's own image rather than as a pushed value, because
     * the MBR has no room for the push. See STAGE1_DRIVE_ADDR in
     * boot_layout.h. */
    movw    $STAGE2_STACK_TOP, %sp

    /*
     * Load the descriptor tables.
     *
     * The GDT goes in here, in 16-bit code, and cannot go anywhere else: the
     * far jump at the end of this block resolves CS through the GDTR, so the
     * table it depends on has to be installed before the jump that needs it.
     * There is no ordering that gets round this and no second chance.
     *
     * The IDT does not have that constraint, so it is installed from the
     * 32-bit side, immediately after the mode switch. See protected_mode.
     *
     * `lgdt (%si)`, with no suffix, is the spelling that assembles to the
     * m16&32 form: `0f 01 /2`, no 0x66 prefix, so the base read out of memory
     * is the full 32 bits. The suffixed spellings are the traps here:
     *
     *   - `lgdtl (%si)` assembles to `66 0f 01 /2`. The 0x66 is what selects
     *     the 16-bit-base form, so the `l` suffix -- the one that reads like
     *     "long base" -- produces the short base. It only differs from the
     *     correct encoding when the table sits above 64 KiB, which is the
     *     condition nothing in the build used to check.
     *   - `lidtl (%si)` assembles to `66 0f 01 /3`, and /3 is LGDT in 16- and
     *     32-bit mode and LIDT in 64-bit mode. Both of those were live here at
     *     some point and neither worked, for that one reason: the mnemonic that
     *     means LIDT everywhere else assembles to the other instruction
     *     entirely.
     *
     * The version this replaces was `.byte 0x0f, 0x01, 0x1c` -- a hand-encoded
     * /3 with no operand-size prefix. ModRM 0x1c is mod=00, reg=/3, rm=100, and
     * in 16-bit addressing rm=100 is %si, not a SIB indicator, so the encoding
     * is complete and well-formed: it is three bytes that load a *GDTR* from the
     * six bytes at `idtr`, i.e. limit 0x0FFF and a base of the low half of the
     * `idt` address. The next two instructions then loaded the real GDT over the
     * top of it. The IDT was never loaded at all, in the whole boot.
     *
     * That is load-bearing by accident in both directions. The PICs stay masked
     * until the kernel remaps them, so no interrupt is ever delivered through
     * the table that was never installed; and the real-mode IVT is still what
     * the IDTR names, so an exception taken in this window is decoded as a
     * 16-bit gate out of the BIOS vector table rather than as anything stage2
     * owns. Both of those had to stay true for the missing LIDT to be invisible,
     * and neither is asserted anywhere.
     *
     * The 16-bit form does put one constraint back: %si can only name an
     * address below 64 KiB. stage2.ld asserts that the GDT is, so growing
     * .rodata past the window is a link error rather than a silently
     * truncated base. The IDT has no such constraint -- it is loaded from the
     * 32-bit side, with the full 32-bit base.
     */
    movw    $gdtr, %si
    lgdt    (%si)

    /*
     * Enter protected mode. Written as bytes because GAS will not assemble
     * the equivalent in .code16.
     *
     * CR0.PE is bit 0. Bit 31 is CR0.PG, and setting that here is a trap
     * worth naming: it turns on paging while the CPU is still in real mode
     * with CR3 = 0, so every instruction fetch is translated through the
     * null page tables and faults immediately. It presents as a page fault
     * at whatever instruction happens to be next -- which is a far jump
     * that never completes -- so the symptom points squarely at the far
     * jump and not at all at the control register write that caused it.
     *
     * Inside .code16 the assembler narrows `orl $imm32, %eax` to a 16-bit OR
     * of AX, so the 0x66 prefixes here are what force 32-bit operand size on
     * instructions still being fetched as 16-bit code. The prefixes on the
     * CR0 moves are not optional either: MOV to and from a control register
     * takes its register from the operand size, so without one these would
     * load and store AX and leave the rest of EAX undefined.
     */
    .byte    0x66, 0x0f, 0x20, 0xc0            /* movl %cr0, %eax           */
    .byte    0x66, 0x83, 0xc8, 0x01            /* orl $0x1, %eax  (PE = b0) */
    .byte    0x66, 0x0f, 0x22, 0xc0            /* movl %eax, %cr0           */

    ljmp    $0x08, $protected_mode

    .code32
protected_mode:
    /*
     * Reload the data segments before anything uses the stack.
     *
     * A far jump reloads CS and nothing else. DS, ES, FS, GS and SS keep the
     * cached descriptors they had in real mode -- base zero, 64 KiB limit, and
     * for SS a *16-bit* stack width -- so until SS names the flat 32-bit data
     * descriptor, every push and pop uses the low 16 bits of ESP and lands at
     * 0xFFFC and below rather than at 0x20000.
     *
     * That is silent rather than fatal, which is what makes it worth being
     * explicit about: an SS with a zero-width stack means `pushl %eax` decrements
     * ESP by four and stores through a *16-bit* SP of 0x0000, so the frame the
     * exception reporter reads back is the contents of low memory rather than
     * the frame the CPU just wrote. Every field prints as zero and the reporter
     * looks broken.
     */
    movw    $0x10, %ax        /* flat data descriptor */
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %fs
    movw    %ax, %gs
    movw    %ax, %ss

    /*
     * Switch to the 32-bit stack, next, and before anything that can fault.
     *
     * The CPU does not change stacks on a far jump, so the 16-bit stack pointer
     * is still live, and it points into the MBR: STAGE2_STACK_TOP is 0x7E00 and
     * the first four-byte push below it writes 0x7DFC, which is
     * STAGE1_DRIVE_ADDR. So an exception in the next few instructions would
     * report itself by destroying the boot drive number, and the loader would
     * then read the kernel from drive 0x00 -- the floppy controller -- and fail
     * with a disk error that has nothing to do with the fault.
     *
     * It also has to be the *32-bit* stack and not a lower SP: the reporter's
     * prologue pushes six registers and calls, so it needs a frame, and it
     * needs one that is not about to run into stage1's image.
     *
     * The hex digit table is what the equivalent mistake looked like when this
     * used to load the *bottom* of the stack. stack32 sits at the very bottom of
     * .bss, so the first 84 bytes of downward growth land exactly across the
     * last 84 bytes of .rodata -- and the last string in .rodata is
     * serial_puthex's "0123456789abcdef". serial_puthex then printed its own
     * lookup table, byte for byte, in place of the digits, so every number the
     * loader printed was wrong and nothing faulted. stack32_top is the top of
     * a region the linker places after everything else, so the growth walks
     * into empty space instead.
     */
    movl    $stack32_top, %esp

    /*
     * Install the IDT, here, from 32-bit code.
     *
     * Everything above had to be 16-bit because the GDT has to be in place
     * before the far jump. This has no such constraint: the CPU is a 32-bit
     * CPU from the instruction after the jump, and in 32-bit mode the operand
     * and address sizes both default to 32 bits, so an ordinary memory
     * operand is the whole m16&32 form with nothing to get wrong --
      * `lidt idtr` assembles to `0f 01 /3` with a four-byte displacement, and
      * /3 is LIDT in this mode.
     *
     * The 32-bit base is the whole point. Both tables are in .rodata and .rodata
     * has no fixed address -- it grew past 0xC000 while the 4 KiB bootstrap IDT
     * was added to it -- so a 16-bit base is correct only while the table
     * happens to sit below 64 KiB, which is not a property anything asserted.
     * With the base truncated, the CPU decodes the first gate out of unrelated
     * memory: a fault still gets *reported*, which is what makes this so
     * expensive to find, but it is reported by whatever the truncated address
     * happens to point at.
     *
     * Nothing between here and the kernel's own LIDT is allowed to fault with
     * the real-mode IVT still loaded, so this is also the first instruction that
     * makes a fault in stage2 a fault stage2 can describe.
     */
    lidt    idtr

    /* Retire the boot drive. stage1 left it at a fixed offset in its own
     * image, which is still intact here: stage1 occupies 0x7C00-0x7DFF and
     * stage2 was loaded to 0x8000, so nothing has overwritten it.
     *
     * The operand has to be a dereference. `movb $addr, %al` moves the
     * address itself into AL, and 0x7DFC truncated to a byte is 0xFC -- a
     * perfectly plausible-looking drive number that is not the one stage1
     * wrote, and not a disk that exists either. */
    movb    STAGE1_DRIVE_ADDR, %al

    /* Park it in EDX rather than storing it straight away. stage1_boot_drive
     * lives in .bss, and the BSS is cleared a few instructions below with a
     * `rep stosb` that sources EAX. Storing the drive first meant the clear
     * overwrote it with zero every single boot, so the kernel was handed boot
     * device 0 -- the floppy controller -- and the first disk read of the
     * kernel image failed against a drive that was never there. EDX is
     * untouched by the clear (stosb only consumes EAX, ECX and EDI), so it
     * carries the value across safely. */
    movb    %al, %dl

    /* Stage2 is loaded by sector rather than through an ELF loader, so the
     * linker-emitted BSS bounds are the only record of what needs zeroing.
     * Clearing it before any C runs keeps .bsetup initialised by the C
     * runtime assumptions the compiler already made. */
    movl    $bss_start, %edi
    movl    $bss_end, %ecx
    subl    %edi, %ecx
    xorl    %eax, %eax
    cld
    rep stosb

    /* Safe to store now that the BSS has been zeroed. See the note on the
     * `movb %al, %dl` above for why this could not come earlier. */
    movb    %dl, stage1_boot_drive

    call    stage2_start

    /* stage2_start only returns if the loader gave up. */
    .globl stage2_halt
stage2_halt:
    cli
1:    hlt
    jmp    1b

/* --------------------------------------------------------- BIOS trampoline --- */
/*
 * Calling the firmware from 32-bit protected mode.
 *
 * The only correct way to reach a BIOS service from protected mode is to leave
 * protected mode, run the interrupt in real mode, and come back. Two other
 * designs look workable and are not:
 *
 *   1. `int $0x15` in protected mode. Rejected outright: whether an interrupt
 *      consults the IDT or the real-mode IVT is decided by CR0.PE and by nothing
 *      else, so with PE set every `int` is an IDT reference. The 16-bit stub
 *      below issues `int`, but it does so from real mode, where the IVT is live.
 *
 *   2. Far-call the handler the IVT names, giving it a code descriptor whose
 *      base is the handler's linear address. This reaches the firmware and even
 *      runs its first instructions, which is exactly what makes it so
 *      deceptive: the far call succeeds and the CPU begins executing real BIOS
 *      code. It then dies on the handler's second instruction. SeaBIOS's entry
 *      stub is
 *
 *          cli
 *          cld
 *          pushw %ds
 *          pushl %eax
 *          movl $0xde80, %eax
 *          movw %ax, %ds        <-- #GP, error code 0xDE80
 *
 *      and that `mov %ax, %ds` is a real-mode segment load. Under protected
 *      mode the value in AX is a selector, 0xDE80 is not one, and the CPU
 *      raises #GP. No amount of fixing the call frame helps: the firmware is
 *      written to run in real mode, loads real-mode segment values, and assumes
 *      segment arithmetic means `segment << 4`. There is no GDT arrangement that
 *      makes that work, because the value loaded is not under our control.
 *
 * So the round trip is not a stylistic preference. It is the only shape the
 * hardware supports, and the fault it replaces is unrecoverable by patching.
 *
 * The sequence, and what each step is actually required to do:
 *
 *   - Save CR0, EFLAGS, ESP and the segment registers. CR0.PE is about to be
 *     cleared, and the BIOS is entitled to change every register it is given;
 *     the only state that must survive is the one it is not going to restore.
 *   - Clear CR0.PE. Doing this while interrupts are disabled matters: an
 *     interrupt taken between here and the far jump would be decoded through
 *     stage2's 32-bit IDT, which is still installed, and reported as a fault in
 *     stage2 rather than handled.
 *   - Load the real-mode segment registers. CR0.PE going away does not clear the
 *     descriptor caches, so DS/ES/FS/GS/SS still name the 32-bit flat selectors
 *     until they are reloaded. Leaving them would make the 16-bit code index
 *     through a 4 GiB-limit descriptor and quietly read the wrong memory.
 *   - Switch to a 16-bit stack below 64 KiB. SS:SP is a real-mode pair, so SP
 *     has to fit in 16 bits or the firmware's own pushes land off the top of
 *     memory.
 *   - Far jump to 16-bit code with a real-mode CS, then `int $n`. Being in real
 *     mode is what makes the `int` an IVT reference, and the far jump is what
 *     flushes the stale descriptor state out of CS.
 *   - The handler's `iret` returns to the label after the `int`. Set CR0.PE
 *     again and far jump back to 32-bit code, which reloads CS from the GDT and
 *     drops the real-mode CS cache along with it.
 *   - Reload the flat segments and restore the saved registers, so the C caller
 *     sees the same stack pointer and flags it left behind.
 *
 * Interrupts stay disabled across the whole trip. The firmware is free to enable
 * them internally, but the flags it `iret`s back with are the ones we pushed, so
 * the round trip cannot leave the loader re-entrant on a BIOS call in progress.
 */
.code32
    .globl bios_call
    .type bios_call, @function
/*
 * cdecl wrapper: bios_call(vector, a, b, c, d, si, di, es)
 *
 * a..d are EAX, EBX, ECX and EDX in that order. They are named after the
 * registers rather than after the services' own field names because the
 * services disagree about which register carries what, and a wrapper that
 * renumbered them is how the E820 call ended up with its arguments in the
 * wrong places for months.
 *
 * A real function call rather than inline asm at the call site, and that is the
 * whole point of it. GCC's rule for inline asm is that a register named as an
 * input is assumed to survive unless it is also an output or a clobber, and
 * naming it as both is a constraint error -- so call sites that set up the
 * firmware's registers through input constraints left the compiler believing
 * those registers were intact across a BIOS call that destroys every one of
 * them. The corrupted live values showed up as an E820 map full of nonsense.
 * An ordinary call states the truth: everything caller-saved is destroyed.
 *
 * SI and DI are separate parameters because the services disagree about which
 * one carries a buffer address -- INT 13h/AH=42h wants DS:SI, VBE wants ES:DI.
 *
 * ES is a parameter for the same reason seen from the other side: the two
 * services that take ES:DI would be pinned to buffers under 64 KiB if the
 * trampoline zeroed it, and the E820 map is at 0x90000.
 *
 * The vector is stored rather than passed in a register because the 16-bit stub
 * reads it from an absolute address after the segment registers have been
 * reloaded for real mode; a register's meaning changes across the mode switch.
 *
 * EBX, ESI and EDI are saved and restored, and this is not politeness. Being an
 * ordinary extern function, this one obeys cdecl, and cdecl makes those three
 * callee-saved: GCC is entitled to keep a live value in any of them across the
 * call, and does. This code uses all three as scratch while setting up the
 * firmware's arguments up, and never put them back, so every one of those live
 * values was destroyed by the call.
 *
 * The damage did not look like a broken E820 call. The E820 loop keeps the
 * address of the entry it is asking for in EBX, and reads the entry's length
 * and type back through it after the call; with EBX holding the firmware's
 * return value instead, the map's first eight bytes came out right and
 * everything after them came out of unrelated memory. The base address is
 * reloaded from a constant, so entry zero read back with a correct base and a
 * length made of whatever followed it, and the loader printed a memory map
 * built out of the interrupt vector table.
 *
 * EBP and ESP need nothing here: the trampoline already saves and restores
 * both, and this function does not touch EBP.
 *
 * The three are pushed before the first argument is read, not after. They are
 * the registers the arguments are loaded into, so a save taken once the
 * loading is done has saved the wrong values: the caller's EBX is already gone,
 * replaced by the entry index the firmware wanted, and what comes back is that
 * index rather than what the caller had. The argument offsets below are shifted
 * by the twelve bytes these three pushes occupy.
 */
bios_call:
    pushl    %ebx
    pushl    %esi
    pushl    %edi

    movl    16(%esp), %eax        /* vector */
    movw    %ax, BIOS_VECTOR_ADDR
    shll    $2, %eax        /* IVT entries are four bytes */
    movw    (%eax), %dx        /* handler offset  */
    movw    2(%eax), %cx        /* handler segment */
    movw    %dx, BIOS_IVT_PTR
    movw    %cx, BIOS_IVT_PTR + 2
    movl    20(%esp), %eax        /* AH/AL */
    movl    24(%esp), %ebx        /* BH/BL */
    movl    28(%esp), %ecx        /* CH/CL */
    movl    32(%esp), %edx        /* DH/DL */
    movl    36(%esp), %esi        /* SI */
    movl    40(%esp), %edi        /* DI */
    movl    44(%esp), %eax        /* ES */
    movw    %ax, bios_es_seg
    movl    20(%esp), %eax        /* AH/AL, again: EAX was the scratch */

    call    bios_trampoline        /* returns the firmware's EBX:EAX */

    /*
     * Interrupts off, again, before anything else runs.
     *
     * The trampoline has to enable interrupts to call the firmware, and the
     * firmware leaves them enabled when it comes back -- it restores our flags
     * with a popf, and by then our IF is whatever the round trip left it as.
     * The loader is single-threaded and needs no interrupts at all, so IF is
     * cleared here once per call rather than being left to chance.
     *
     * Leaving it set is not merely untidy. The PIT interrupt is delivered
     * through the real-mode IVT, and if the loader has touched a vector the
     * firmware's handler is gone and control lands in stage2's own image --
     * where the first instruction executed is whatever happens to sit at that
     * offset, with no way to report it. The loader then stops making progress
     * partway through reading the kernel, with the console showing its last
     * successful line and nothing after it.
     */
    cli

    popl    %edi
    popl    %esi
    popl    %ebx
    ret
    .size bios_call, . - bios_call

    .globl bios_trampoline

/*
 * Enter with the vector number already written to BIOS_VECTOR_ADDR. That is how
 * it reaches the 16-bit side: the stub can only name an absolute address below
 * 64 KiB, and it has to read the vector after the segment registers have been
 * reloaded for real mode, so the value cannot travel in a register whose
 * meaning the mode switch changes.
 *
 * Every register other than ESP, EBP and the segment registers is destroyed,
 * which is the firmware's doing, not this code's. Callers reach this through
 * bios_call above, which is what tells the compiler so.
 *
 * The return is the firmware's EBX:EAX as a 64-bit value, because EAX alone
 * is not enough for the services that walk a list: they return the same EAX
 * every time and put the position in EBX.
 */
bios_trampoline:
    cli
    cld

    movl    %esp, bios_saved_esp
    movl    %ebp, bios_saved_ebp
    movl    %eax, bios_saved_eax        /* the firmware's first argument */
    movw    %ds, bios_saved_ds
    movw    %es, bios_saved_es
    movl    %cr0, %eax
    movl    %eax, bios_saved_cr0

    /* The first step is not the mode change; it is the code segment change.
     * The far jump below loads a 16-bit code descriptor from the GDT, so from
     * here on the CPU fetches and decodes 16-bit instructions -- while PE is
     * still set, so the selector really is a GDT selector and the descriptor
     * really is the one the table names.
     *
     * Clearing CR0.PE first, and far jumping into real mode afterwards, is the
     * shape this used to have and it cannot work. PE and the code segment
     * register are independent: clearing PE leaves CS's cached descriptor
     * exactly as it was, D bit included, and it is that descriptor's D bit the
     * CPU decodes the next instruction with. CS is still the 32-bit flat
     * selector stage2 entered through, so every instruction after the CR0
     * write is decoded as 32-bit code. The stub below, written for 16-bit,
     * then decodes into something else entirely: `movw $0xE9, %dx` becomes a
     * five-byte `mov $0xE9, %edx` that swallows the two bytes after it, the
     * `cmpw` pairs vanish into the middle of an immediate, and the `int` is
     * reached or not reached by arithmetic rather than by design. Nothing
     * faults -- it is all valid 32-bit code somewhere else in the loader -- so
     * the round trip simply never returns, and the loader is left reporting an
     * E820 map it never filled in. On QEMU the far jump in that position is
     * read as the 32-bit seven-byte form, so the segment it loads is 0x00A1
     * and the CPU carries on in real mode at 0xA1:0x80C9. On hardware that
     * keeps D set the far jump lands on the right offset and still decodes
     * 32-bit, which is no better.
     *
     * A GDT far jump reloads the descriptor, and that is the only thing that
     * does. So the order is: reload CS as 16-bit, then clear PE. */
    ljmp    $BIOS_RM_CODE_SEL, $bios_real_mode

    .code16
bios_real_mode:
    /* Now executing 16-bit instructions. Clear PE. The CR0 moves are written as
     * bytes for the same reason as the ones in stage2_entry: GAS assembles
     * `movl %cr0, %eax` in .code16 as `0f 20 c0`, with no operand-size prefix,
     * and the CPU then decodes that as a *16-bit* operand move. Only AX would
     * be loaded, the upper half of EAX would be left holding whatever the
     * firmware returned, and the matching `movl %eax, %cr0` would store sixteen
     * bits of a control register that needs all of them -- so PE would never
     * come back, and the far jump at the end of the trip would be taken with
     * PE clear, where selector 0x08 means segment 0x08 rather than GDT index 1
     * and the CPU would resume at 0x80:0x80F0 in the middle of stage2's text.
     *
     * `orl $1, %eax` does get its prefix from GAS, because the immediate form
     * needs one; the control-register moves do not, and they are exactly the
     * two instructions that clear and set PE. */
    .byte    0x66, 0x0f, 0x20, 0xc0        /* movl %cr0, %eax           */
    .byte    0x66, 0x83, 0xe0, 0xfe        /* andl $0xfffffffe, %eax    */
    .byte    0x66, 0x0f, 0x22, 0xc0        /* movl %eax, %cr0           */

    /* Real mode now, but CS is still 0x18 -- a GDT selector, loaded while PE
     * was set, with base 0. The base is what we want; the *value* is not, and
     * the instruction that takes it is the interrupt three lines below.
     *
     * `int $n` pushes FLAGS, CS and IP, and the firmware's `iret` pops them.
     * In real mode the popped CS is a segment, not a selector, so it would be
     * resumed at 0x180:0x80e1 instead of 0:0x80e1 -- 0x18 pages further on,
     * in whatever stage2 happens to have there. Nothing faults; the CPU simply
     * executes whatever it finds, which is how a loader can print a memory map
     * and then carry on through code it was never meant to run.
     *
     * Only a far jump can put a real-mode value in CS, and `mov %ax, %cs` does
     * not exist. This one is also what keeps 16-bit decoding: a far jump
     * reloads the descriptor, and a real-mode segment always decodes with D
     * clear. The form is the five-byte one because CS's cached descriptor --
     * the 16-bit GDT entry the jump above installed -- has D clear, which is
     * what decides whether the offset is 16 or 32 bits wide.
     *
     * The reloads of the other segments have to come after PE is clear, and for
     * the same reason this one does: loading a zero into one of them while PE is
     * set loads the *null* descriptor, whose limit is zero, and the first
     * 16-bit access through it faults. After the mode change the same store is
     * a real-mode segment of base 0 and a 64 KiB limit. */
    ljmp    $0x0000, $bios_real_mode_flat

bios_real_mode_flat:
    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %ax, %fs
    movw    %ax, %gs
    movw    %ax, %ss

    /* ES is the one segment the caller gets to choose, because two of the
     * services disagree about where a buffer pointer lives: INT 15h/AH=E820h
     * takes ES:DI, and so does VBE, while INT 13h/AH=42h takes DS:SI. With
     * ES pinned at zero the first two can only name buffers under 64 KiB, and
     * the E820 map is at 0x90000.
     *
     * A 16-bit `mov` is enough here and deliberate: the value is a real-mode
     * segment, 16 bits by construction, and the high half of EAX is the
     * firmware's argument rather than part of a segment. */
    movw    bios_es_seg, %ax
    movw    %ax, %es

    /* EAX is restored here rather than left in a register across the mode
     * switch, and it has to be: reading CR0 on the way out needs a general
     * register, and EAX is the one that is about to be handed to the firmware.
     * The value that survives is CR0, not the function number, so the handler
     * is entered with EAX = 0x10 and finds no function it recognises --
     * INT 15h/AH=00h, "get system type", which SeaBIOS answers by opening the
     * A20 gate and returning. Nothing faults, nothing is logged, and the caller
     * reads back a buffer the firmware never wrote.
     *
     * That is what the E820 query looked like for a long time: the map came
     * back holding the 0xA5 the caller had poisoned it with, once per
     * iteration, until the entry cap, and every one of those iterations had
     * quietly asked the firmware for the machine type instead.
     *
     * The register is saved whole rather than as AX, because a 16-bit move
     * here would only restore half of it and the upper half is CR0's.
     *
     * The encoding is spelled out for the reason given on the store side
     * below: 0x66 makes the operand 32 bits and 0x67 makes the address 32
     * bits, which together are what `movl` means and neither of which .code16
     * supplies on its own. */
    .byte    0x67, 0x66, 0xa1        /* movl bios_saved_eax, %eax    */
    .long    bios_saved_eax

    /* Switch to a 16-bit stack below 64 KiB. SS:SP is a real-mode pair, so SP
     * has to fit in 16 bits or the firmware's own pushes land off the top of
     * memory.
     *
     * Below 64 KiB is the easy half of that. The other half is that it has to
     * be clear of stage1, and BIOS_RM_STACK_TOP (0x7E00) is not: stage1 owns
     * 0x7C00-0x7DFF and a stack grows *down*, so the two lines below write
     * 0x7DFE and 0x7DFC -- and 0x7DFC is STAGE1_DRIVE_ADDR, the one byte of
     * memory stage1 and stage2 share. The `pushfw` lands on the drive byte
     * itself, on the first BIOS call of every boot.
     *
     * It is harmless today for one reason only, and it is a coincidence rather
     * than a design: stage2 reads the drive byte at the top of protected_mode,
     * long before anything calls the firmware, so the value has already been
     * copied into .bss by the time it is destroyed. The header's own comment
     * ("0x7E00 is above stage1's image ... so nothing the firmware writes there
     * can collide") reasons backwards -- 0x7E00 is above 0x7C00-0x7DFF in
     * address order and a downward-growing stack at 0x7E00 grows straight into
     * it.
     *
     * So the top is taken below the MBR instead, in the gap between the BIOS
     * data area and stage1's image, which is the only low memory nothing in the
     * boot path claims. Two things bound it: stage1's image above, and the
     * BIOS_VECTOR_ADDR / BIOS_IVT_PTR slot at 0x5000 below. That is 10 KiB of
     * headroom for what the firmware actually pushes -- its own register save
     * plus a full interrupt frame, a few dozen bytes -- and both bounds are
     * asserted in stage2.ld, so a stage1 image that grew into the gap or a
     * slot that moved up into it is a link error. */
    movw    $bios_rm_stack_top, %sp
    movw    %sp, %bp

    /* Take the firmware's interrupt for the vector the C caller stored at
     * BIOS_VECTOR_ADDR, by far-calling the vector *through the IVT entry* rather
     * than with an `int`.
     *
     * `int $n` is the obvious instruction and it is the wrong one here. QEMU
     * does not take the vector from the real-mode interrupt vector table at
     * physical zero: with a non-zero IDTR base left over from stage2's own
     * protected-mode setup, the handler address it dispatches to comes from
     * somewhere else entirely. It was measured, not guessed -- with a breakpoint
     * on the `int` and IVT[0x0f] patched to `0000:4321` from the debugger, the
     * CPU still left for `0000:8e00`, which is stage2's own exception-reporting
     * stub and not a firmware address at all. Nothing was even printed by the
     * firmware: stage2's stub picked the call up, and its reporter ran stage2's
     * 32-bit code in 16-bit decoding until it wandered into a string-printing
     * loop that has no end. The visible symptom was a memory map that started
     * printing and then degenerated into megabytes of garbage.
     *
     * Reading the entry and calling it directly has no such ambiguity. The IVT
     * entry *is* the handler address, the address computation is explicit, and
     * the handler returns exactly where `int` would have left it.
     *
     * `pushfw` is part of that. The firmware's entry stub ends with `iret`, not
     * `retf`: it saves the caller's IP, CS and flags in its own register block and
     * then pops all three off the caller's stack, which is precisely the shape
     * `int` leaves behind. A far call only pushes two of the three words, so
     * without this the `iret` would take the third from whatever happened to be
     * below the frame -- stale stack contents, and a different set of flag bits on
     * the way out than on the way in.
     *
     * The address itself is precomputed on the 32-bit side and left in memory,
     * because there is no register here that can be used to compute it. Every one
     * of AX, BX, CX, DX, SI and DI is about to be handed to the firmware as an
     * input, and 16-bit arithmetic on one of them corrupts the call rather than
     * the address: `movw $15, %bx` leaves EBX as 0x534D003C rather than
     * 0x534D4150, and INT 15h/AH=E820 rejects anything but the exact 'SMAP'
     * signature, so the request is refused and the entry buffer is never written.
     * That is what an earlier version of this did, and it is why the E820 map came
     * back holding the 0xA5 the caller had pre-filled it with.
     *
     * BIOS_IVT_PTR holds the handler address, as an offset word followed by a
     * segment word -- the shape `call far` expects to find in memory. bios_call
     * resolves it from the IVT before the mode switch, using physical zero,
     * which the flat 32-bit segments can reach with paging off. The three
     * registers it spends doing so are loaded again from the stack immediately
     * afterwards, so nothing reaches the firmware disturbed. It is below 64 KiB
     * like the vector itself, so the 16-bit side can name it with a plain
     * absolute far call, and the link-time assert in stage2.ld covers it. */
    pushfw
    lcallw    *BIOS_IVT_PTR

    /* Park the firmware's return values before anything else touches a
     * register.
     *
     * The three instructions after this one read CR0 into EAX, and the
     * caller's answer arrives in EAX. The first version of this trampoline
     * simply let the CR0 read win, so every BIOS call returned 0x10 -- the
     * value of the control register -- whatever the firmware had said. With
     * the E820 query that reads as "the call was refused", because the
     * signature it checks for is not 0x10, and there is no way to tell from
     * out here whether the firmware refused the call or the trampoline ate
     * its answer.
     *
     * Both words are written, not just AX: the services are 16-bit, but the
     * value is handed back in a 32-bit register and there is no reason to
     * depend on the upper half happening to be zero.
     *
     * The encodings are spelled out because both the operand size and the
     * address size default to 16 bits in .code16, and only the first of them
     * is what `movl` asks for. `movl bios_ret_eax, %eax` assembles to 66 A3
     * with a *two*-byte address: the operand is 32 bits, the address is not,
     * and the linker's remaining two address bytes fall through to be
     * executed as the next instruction -- `add %al, (%bx)`, corrupting
     * whatever EBX pointed at, once per BIOS call. 0x67 in front selects the
     * 32-bit address, which is what the four relocation bytes are for. */
    .byte    0x67, 0x66, 0xa3        /* movl %eax, bios_ret_eax       */
    .long    bios_ret_eax
    .byte    0x67, 0x66, 0x89, 0x1d    /* movl %ebx, bios_ret_ebx       */
    .long    bios_ret_ebx

    /*
     * ECX and EDX are saved for the same reason and by the same encoding.
     *
     * They are not part of the return value -- EAX and EBX are, packed into the
     * 64-bit one -- because no service this loader calls answers in them. One
     * does put its answer *in* them: INT 13h/AH=08h reports the drive geometry
     * in CL, CH and DH, and there is nowhere else to read it from. Without
     * these two stores the only way to get a geometry is to have stage1 write
     * one into its nine spare bytes, which does not fit.
     */
    .byte    0x67, 0x66, 0x89, 0x0d    /* movl %ecx, bios_ret_ecx       */
    .long    bios_ret_ecx
    .byte    0x67, 0x66, 0x89, 0x15    /* movl %edx, bios_ret_edx       */
    .long    bios_ret_edx

    /*
     * Tripwires, written on the first instructions after the firmware's iret
     * has returned. Both words are zeroed by diag_tick() on the C side before
     * each call, so a call that never comes back leaves them zero while a
     * normal return makes both non-zero.
     *
     * bios_post_sp is the interesting one. It is compared against
     * BIOS_RM_STACK_TOP, which is the whole question: the firmware is handed
     * a real-mode SS:SP pair and is entitled to push onto it, but it has to
     * give the stack pointer back where it found it, because the 16-bit code
     * after this point has no other way to find its own frame. A handler that
     * returns with SP somewhere else resumes the loader on a stack the loader
     * did not set up, and the symptom is a jump into the middle of stage2's
     * text with nothing on the serial port.
     *
     * A word each rather than a long: stage2's .bss sits within a few bytes of
     * the 0x10000 transfer window, and stage2.ld asserts the bound, so this
     * has to be added without pushing __bss_end past it. SP is 16 bits in real
     * mode in any case, so a word loses nothing.
     *
     * Encodings spelled out for the same reason as the stores above: in
     * .code16 both the operand size and the address size default to 16 bits,
     * and 0x67 is what forces the four-byte relocation the linker needs.
     */

    /* Back from the handler, still in real mode. Set PE again and far jump
     * back to 32-bit code.
     *
     * PE is restored *before* the far jump, not after, and the order is the
     * whole point: the far jump is decoded with D clear (five bytes, a 16-bit
     * offset) but its selector is interpreted according to PE. With PE still
     * clear, 0x08 is a segment value and the CPU resumes at 0x80:0x80F0. With
     * PE set, it is GDT index 1 -- the flat 32-bit code descriptor, base 0 --
     * and the offset is where it was written to be. */
    .byte    0x66, 0x0f, 0x20, 0xc0        /* movl %cr0, %eax           */
    .byte    0x66, 0x83, 0xc8, 0x01        /* orl $0x1, %eax  (PE = b0) */
    .byte    0x66, 0x0f, 0x22, 0xc0        /* movl %eax, %cr0           */
    ljmp    $0x08, $bios_pm_return

    .code32
bios_pm_return:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    $0x10, %ax
    movw    %ax, %fs
    movw    %ax, %gs

    /* SS is reloaded before ESP is restored, deliberately. Until SS names the
     * flat descriptor again it still refers to the real-mode 16-bit cached
     * descriptor, and writing the full 32-bit saved stack pointer through it
     * would be a privilege-width violation the moment anything pushed. */
    movw    $0x10, %ax
    movw    %ax, %ss

    movl    bios_saved_esp, %esp
    movl    bios_saved_ebp, %ebp

    /* The firmware's answers, read back from where the real-mode side left
     * them. AX was used for the segment reloads above, so EAX is reloaded
     * whole rather than patched.
     *
     * EBX goes back in EDX, which is the second half of the 64-bit return,
     * because the services that index rather than flag their results do not
     * put the index anywhere in EAX -- INT 15h/AH=E820h returns the same
     * 'SMAP' value in EAX for every entry and the "was that the last one"
     * answer only in EBX. */
    movl    bios_ret_eax, %eax
    movl    bios_ret_ebx, %edx
    ret

/* ---------------------------------------------------------------------------
 * The 32-bit GDT used for the initial protected-mode switch. The long-mode
 * switch installs a different table (with the L bit set on the code segment)
 * from boot_gdt_init() in stage2.c.
 *
 * Four entries, all at base 0, and nothing else. The BIOS trampoline has exactly
 * one of its own: the 16-bit code descriptor at 0x18, which the far jump on the
 * way out loads so that the CPU decodes 16-bit instructions before PE is cleared.
 * See the trampoline above.
 *
 * There is deliberately no per-vector descriptor for the firmware, because the
 * earlier design that had one -- far-call the IVT handler through a descriptor
 * based at the handler's linear address -- cannot work.
 * --------------------------------------------------------------------------- */
/*
 * The 64-bit literals below are each descriptor read as a little-endian word,
 * and the byte order is the whole trap. 0x9A is the access byte -- the one
 * that says "present, ring 0, code, execute/read" -- and it has to land in
 * byte 5. Written as 0x9A0F00000000FFFF it lands in byte 7 instead, which
 * leaves the access byte zero: a descriptor that is not a valid segment at all.
 * Nothing complains at load time, because LGDT does not validate, and the GDT
 * is not consulted again until something far jumps. The failure is therefore
 * deferred to the first far jump, where it arrives as a #GP whose error
 * code is the offending selector and reads like a trampoline bug rather than a
 * descriptor bug.
 */
    .section .rodata
    .align 8
    .globl stage2_gdt
stage2_gdt:
gdt:
    .quad    0            /* 0x00 null */
    .quad    0x00CF9A000000FFFF    /* 0x08 32-bit code, base 0, limit 4G */
    .quad    0x00CF93000000FFFF    /* 0x10 32-bit data, base 0, limit 4G */
    .quad    0x00009A000000FFFF    /* 0x18 16-bit code, base 0, limit 64K */
    /* gdt_end is what the GDTR limit is measured from, and it is the one
     * label that cannot be inferred: the entries are 8 bytes each, but the
     * limit field counts bytes and the table has to end on a descriptor
     * boundary or the last descriptor is only partly loaded. */
gdt_end:
    .equ    gdt_bytes, gdt_end - gdt
gdtr:
    .word    gdt_bytes - 1
    .long    gdt

/* --------------------------------------------------------------- IDT ------- */

/*
 * 256 32-bit interrupt gates, all pointing at one reporter defined below.
 *
 * The reporter is assembly rather than a call into stage2.c because it has to
 * work before the C runtime is reachable and while the only stack in existence
 * is the 0x7e00 one set a few instructions earlier. A fault taken during the
 * mode switch is exactly the case this exists for, and without it that fault is
 * a triple fault: a machine reset that leaves no register state behind to
 * explain it. This way the same fault prints CR2 and stops.
 *
 * The logical reason for having an IDT here at all: the real-mode IVT is still
 * loaded at this point, and a far jump changes the meaning of every byte in
 * it. The 16-bit gate at vector 8 is not a 32-bit gate, so the first interrupt
 * taken after the switch would be decoded from the wrong shape.
 */
/* --------------------------------------------------- exception reporter ---- */

    .code32

/* outb to COM1, clobbers nothing */
    .set    COM1, 0x3F8

/*
 * Put one character on COM1. The character arrives in EDI.
 *
 * This used to be `movl %edi,%edx`, with a comment claiming EDI was the port.
 * It is not: every caller passes the *character*, so the character went into DX
 * and `outb %al,(%dx)` sent a byte to ports 0x0A-0xFF -- the PIC at 0x20 and
 * 0x21, the 8042 at 0x60 and 0x61, the CMOS index port at 0x70, and so on.
 *
 * Nothing reached COM1, so the 32-bit reporter printed its hex fields and no
 * message, which reads as "the reporter is broken" rather than "the reporter is
 * aimed at the hardware". And every character of every exception report was
 * written into real device state: into the PIC's mask registers, which is how a
 * fault gets followed by a machine that will not take another interrupt, and
 * into the CMOS index port, which is how it gets followed by a machine whose
 * real-time clock and RAM configuration registers now hold ASCII.
 *
 * The port is a constant. No caller wants anywhere else.
 */
exc_putc:
    movl    $COM1, %edx
    movl    %edi, %eax
    andl    $0xFF, %eax
    outb    %al, (%dx)
    ret

/* Print EAX as 16 hex digits, then a space. Clobbers EBX, ECX, EDX, EDI. */
exc_hex32:
    movl    %eax, %ebx
    movl    $16, %ecx
    movl    $COM1, %edx
1:    roll    $4, %ebx
    movl    %ebx, %edi
    andl    $0xF, %edi
    cmpl    $10, %edi
    jb    2f
    addl    $('a' - '0' - 10), %edi
2:    addl    $'0', %edi        /* bias into '0'-'9' last, after the
                     * compare. Biasing first and then
                     * testing against 10 can never be
                     * true, since '0' is 0x30, so every
                     * digit took the letter branch and the
                     * report printed as one repeated
                     * character with the real value
                     * nowhere in it. */
    movl    %edi, %eax        /* AL must hold the digit itself */
    outb    %al, (%dx)
    loop    1b

    movb    $' ', %al
    outb    %al, (%dx)
    ret

exc_puts:
    pushl    %esi
    /* The caller passes the string in EDI, and exc_hex32 above has already
     * used and destroyed EDI, so the pointer has to be picked up here rather
     * than left in whatever ESI happened to hold. `lodsb` reads through
     * DS:ESI, so an EDI that is never moved into ESI prints nothing at all --
     * which is how a fault managed to dump four registers and then stop
     * without ever naming itself. */
    movl    %edi, %esi
1:    lodsb
    testb    %al, %al
    jz    2f
    movl    %eax, %edi
    call    exc_putc
    jmp    1b
2:    popl    %esi
    ret

/*
 * Common entry for every vector.
 *
 * The stack on arrival is, from low address to high: error code (only for the
 * vectors that push one), EIP, CS, EFLAGS. ESP/SS follow if the fault came from
 * ring 3, which cannot happen before the kernel installs its own tables.
 *
 * There is no vector number on the stack, and that is not an oversight in
 * the gate. A 32-bit interrupt gate pushes EFLAGS, CS and EIP -- and the
 * error code for the vectors that have one -- but never the vector. Real-mode
 * `int` puts the vector in the high byte of the pushed CS, which is a trick
 * that has no meaning in protected mode, where CS is a selector.
 *
 * So the reporter cannot print the vector, and the earlier version's claim that
 * it could was the reason its output was unreadable: it read four words above
 * the frame and labelled them error code, CR2, vector and EIP, which made them
 * out to be EIP, CS, EFLAGS and whatever sat above the frame. Every field was
 * off by one word, so a #GP reported its own EIP as the error code -- 0xD893
 * instead of the real 0xDE80 -- and the fault looked like it had happened in
 * stage2 at an address it had never been near.
 *
 * One entry stub per vector is what would recover the number, and it is not
 * worth 256 gates to fix a diagnostic. The CS value is printed instead, and
 * between it and the EIP it is enough to tell a fault in the loader from a
 * fault inside the firmware.
 */
/*
 * The exception stub, reached from both worlds.
 *
 * A single 32-bit stub cannot serve both. Once the far jump into long mode has
 * taken effect, CS.L is set and every instruction here is decoded with a
 * 64-bit default operand size: `pushl %eax` pushes eight bytes rather than
 * four, `call` pushes a return address of eight, and the stack offsets below
 * are wrong by a factor of two. The stub then faults again while handling the
 * first fault, which is indistinguishable from a hang -- no message, no
 * register values, and a CPU spinning in its own handler.
 *
 * That is exactly what happens if the kernel is handed a page table that does
 * not map something it touches: the kernel's first fault is reported by a
 * reporter that cannot run, and the loader looks as though it simply stopped
 * at the jump. So the mode is selected rather than assumed: the long-mode
 * entry point is a separate stub, and the gate picks between them on the
 * selector it was entered with.
 */
    .globl    exc_stub        /* stage2.ld splits its address for the gate */
    .globl    exc_stub64
exc_stub:
    pushl    %eax
    pushl    %ebx
    pushl    %ecx
    pushl    %edx
    pushl    %esi
    pushl    %edi

    /* The six saved registers occupy 24 bytes. Above them the frame starts
     * with the error code where the vector has one, and with EIP where it does
     * not -- so this is a best-effort read, and the two cases are told apart
     * by the vector, which is exactly what is not available. Printing it as
     * "err/EIP" rather than "error code" keeps the ambiguity visible instead
     * of presenting a guess as a fact. */
    movl    24(%esp), %eax
    call    exc_hex32

    movl    %cr2, %eax
    call    exc_hex32

    movl    28(%esp), %eax        /* EIP */
    call    exc_hex32

    movl    32(%esp), %eax        /* CS: 0x08 is stage2, anything else is
                     * a selector the loader loaded for a
                     * real-mode segment and should never
                     * have been able to. */
    call    exc_hex32

    movl    $exc_msg, %edi
    call    exc_puts

    /* Stop here. Returning would re-execute the faulting instruction and
     * fault again forever, which looks exactly like a hang. Falling through
     * to stage2_halt, which is defined after the call to stage2_start. */
    jmp    stage2_halt

/*
 * The same reporter, reached with CS.L set.
 *
 * A separate copy rather than a shared one because the operand sizes and the
 * stack layout are not the same in the two modes, and a stub that cannot run
 * reports nothing at all -- the failure looks like silence. The frame here is
 * six eight-byte registers, then optionally an eight-byte error code, then RIP
 * and CS; the vector is not available, so the error code is printed under the
 * same "err/EIP" heading as before rather than guessed at.
 */
    .code64
exc_stub64:
    pushq    %rax
    pushq    %rbx
    pushq    %rcx
    pushq    %rdx
    pushq    %rsi
    pushq    %rdi

    /* 48 bytes of saved registers; the frame starts above that. */
    /* The frame offsets are assembled by hand. This file is assembled as a
     * 32-bit object, so `movq 48(%rsp), %rax` is a 64-bit displacement with a
     * RIP-relative or absolute form the assembler cannot encode here -- it
     * rejects BFD_RELOC_X86_64_32S outright. Building the address with a
     * 32-bit lea and pushing/popping it is the same instruction sequence the
     * assembler would have emitted.
     *
     * The offsets are 48, 56 and 64 -- RIP-or-error-code, CS-or-RIP,
     * RFLAGS-or-CS -- and the reason all three are printed with the same
     * "err/EIP CR2 EIP CS" heading as the 32-bit stub above is that the gate
     * does not say which of the two layouts it produced. A long-mode gate
     * pushes RFLAGS, CS and RIP, plus an error code for the vectors that have
     * one; the CPU never tells the handler whether it did, so the reporter
     * prints the word that could be either first and lets the reader decide.
     *
     * This used to read 56 and 64 and skip 48 altogether, which is off by one
     * word for every vector that pushes no error code -- #UD, #GP(0), and any
     * software `int n` -- so the word labelled EIP was CS and the word labelled
     * CS was RFLAGS, and RIP was never printed at all. Measured, with an
     * `int $0xFE` immediately after the long-mode jump:
     *
     *   0000000000000000 0000000000000008 0000000000000006
     *   CR2 = 0, then 8 (CS, not RIP) and 6 (RFLAGS, not CS)
     *
     * The 8 and the 6 are correct values being reported under the wrong
     * labels, which is the most expensive kind of wrong: the report looks
     * plausible. The four-field layout below is the same one the 32-bit stub
     * uses, so the message string describes both of them. */

    leaq    48(%rsp), %rax
    movq    (%rax), %rax        /* error code, or RIP */
    call    exc_hex64

    movq    %cr2, %rax
    call    exc_hex64

    leaq    56(%rsp), %rax
    movq    (%rax), %rax        /* RIP, or CS */
    call    exc_hex64

    leaq    64(%rsp), %rax
    movq    (%rax), %rax        /* CS, or RFLAGS */
    call    exc_hex64

    movl    $exc_msg, %edi
    movq    %rdi, %rdi
    call    exc_puts64

    jmp    exc_halt64

    .globl exc_halt64
exc_halt64:
    cli
    hlt
    jmp    exc_halt64

/* 64-bit hex, low 64 bits, four digits a nibble-group at a time is overkill
 * here: the values that matter are RIP and CR2, and 16 digits is enough to
 * place either. */
    .globl exc_hex64
/*
 * Polled serial output for the 64-bit reporter.
 *
 * This is a function rather than a macro on purpose. A macro that defines a
 * numeric local label captures every `1b` in the code that follows it, so in a
 * caller's loop the `jnz 1b` at the bottom binds to the macro's poll instead of
 * the loop head and the routine never terminates. Giving the label a `.L`
 * prefix does not help either: GAS then reads `.LoutcN` as a local-label
 * directive. `\@@` works but is easy to get wrong, and the whole problem goes
 * away if the poll is a callee.
 *
 * Polling matters. The 16550 transmit FIFO is 16 bytes deep and QEMU discards
 * writes made while it is full, so an unpokked burst is not slow output, it is
 * missing output -- and the part that goes missing is the tail, which is
 * exactly where the value being reported would have been.
 *
 * The character travels in DIL, not AL: reading the line status register has
 * to target AL, so a poll that keeps the character in AL overwrites it with the
 * status byte and transmits that instead. The status byte has THRE set, so it
 * is 0x60, and the reporter emits a stream of backticks and none of the
 * message -- which is worse than no reporter, because the console looks busy.
 *
 * Argument: the character in DIL. Clobbers RAX, RDX, RCX.
 */
    .globl    exc_putc64
exc_putc64:
    movl    $0x3FD, %edx
1:    inb    (%dx), %al
    testb    $0x20, %al
    jz    1b
    movl    $0x3F8, %edx
    movzbl    %dil, %eax
    outb    %al, (%dx)
    ret

    .globl    exc_hex64
exc_hex64:
    movq    %rax, %rbx
    movl    $16, %ecx
1:    rolq    $4, %rbx
    movq    %rbx, %r9
    andl    $0xF, %r9d
    cmpl    $10, %r9d
    jb    2f
    addl    $('a' - '0' - 10), %r9d
2:    addl    $'0', %r9d
    movl    %r9d, %edi
    call    exc_putc64
    decl    %ecx
    jnz    1b
    movl    $' ', %edi
    call    exc_putc64
    ret

    .globl    exc_puts64
exc_puts64:
    movq    %rdi, %rsi
1:    lodsb
    testb    %al, %al
    jz    2f
    movzbl    %al, %edi
    call    exc_putc64
    jmp    1b
2:    ret

    .code32

    .section .rodata
exc_msg:
    .asciz    "\r\n[stage2] exception: err/EIP CR2 EIP CS\r\n"

/*
 * The table itself, emitted after the stub rather than before it.
 *
 * The gate offset has to be split into a low and a high 16-bit half, and
 * GAS can only do that arithmetic on a symbol it has already resolved. A
 * definition placed before the stub makes exc_stub a forward reference, and
 * `.set STUB_OFF, exc_stub` then fails to assemble -- or, worse, silently
 * evaluates against an assumed value. Ordering it after the stub makes the
 * reference backward and the masking is done by the assembler, which knows
 * the address.
 *
 * A forward reference is fine for `movw $idtr, %si` at the top of the file,
 * because a 16-bit immediate is emitted with a relocation for the linker to
 * fill in. Only the bit splitting needs the value at assembly time.
 */
    .section .rodata
    .align 8
    .globl    boot_idt
idt:
boot_idt:
    /*
     * One gate, four words, in the order a 32-bit gate stores them:
     *   offset[15:0] | selector | type/attributes | offset[31:16]
     *
     * exc_stub_lo and exc_stub_hi are defined in stage2.ld, where the linker
     * can split the address. See the comment there for why this is not done
     * with `.set` and a mask.
     *
     * selector 0x08 is GDT index 1, the code segment. Index 2 at 0x10 is the
     * *data* segment in both the boot-time GDT above and the one stage2.c
     * builds for the long-mode switch, so a gate that selects it makes the
     * processor refuse the entry: jumping to a data descriptor is #GP(0x10),
     * and the fault that the gate was there to report is the one that ends
     * up unreported. The symptom is not an exception at all -- it is a
     * kernel that faults, gets sent straight back into another fault, and
     * triple faults, which from outside looks identical to a kernel that
     * never ran.
     *
     * type 0x8E is present, DPL 0, a *64-bit* interrupt gate rather than a
     * trap gate: an interrupt gate clears IF on entry, so the reporter cannot
     * be re-entered by the timer while it is printing.
     */
    /*
     * Sixteen bytes per gate, not eight.
     *
     * In 64-bit mode the CPU reads a 16-byte IDT entry and requires the
     * upper eight bytes to be zero; a non-zero bit anywhere in offset[63:32]
     * raises #GP before the handler is ever entered. An eight-byte gate
     * repeated back to back leaves the following gate's offset and selector
     * sitting exactly where those reserved bytes are, so the bits are never
     * zero and no exception can be delivered at all.
     *
     * The failure is silent and self-repeating. The #GP is raised while the
     * processor is fetching the handler's entry, so the fault is reported at
     * the address the malformed entry implies -- which is the gate's own
     * offset shifted up by 32 bits -- rather than anywhere near the code that
     * faulted. Nothing is printed, because the thing that would print it is
     * the thing that could not be entered. The next fault lands on the same
     * unusable entry, and the machine loops on #GP until it triple faults and
     * QEMU exits, which from the outside is indistinguishable from a kernel
     * that hung.
     *
     * Sixteen bytes is safe for the 32-bit phase too: a gate there is still
     * read as eight bytes, and the extra eight are simply not looked at. One
     * table serves both modes, and the IDT limit covers the full 4 KiB.
     */
    .rept    256
    .word    exc_stub_lo
    .word    0x0008
    .word    0x8E00
    .word    exc_stub_hi
    .long    0
    .long    0
    .endr
idtr:
    .word    256 * 16 - 1
    .long    idt

    .section .trampoline
    .align 16
/*
 * Trampoline state.
 *
 * All in .bss, and below 64 KiB, because the 16-bit side of the BIOS round trip
 * addresses them by absolute 16-bit displacement and a 16-bit address cannot
 * reach any higher. stage2.ld asserts the upper bound.
 *
 * The vector word is the exception and is not here: it lives at the fixed
 * BIOS_VECTOR_ADDR, because the `int` instruction encodes its address at
 * assembly time.
 *
 * The saved values live in memory rather than in registers because the round
 * trip crosses a mode change on the way out and a full BIOS handler on the way
 * back. A register holding them would be reinterpreted by the time it was
 * needed, and a handler is free to clobber every register it is handed.
 */
    .globl    bios_saved_cr0
bios_saved_cr0:
    .long    0
    .globl    bios_saved_esp
bios_saved_esp:
    .long    0
    .globl    bios_saved_eax
bios_saved_eax:
    .long    0
    .globl    bios_saved_ebp
bios_saved_ebp:
    .long    0
    .globl    bios_saved_ds
bios_saved_ds:
    .word    0
    .globl    bios_saved_es
bios_saved_es:
    .word    0
/*
 * The real-mode ES the caller asked for, written on the 32-bit side and read
 * back after the mode switch. In memory rather than in a register for the same
 * reason as the rest of the trampoline state: the mode switch is what makes the
 * register unusable, and a handler is free to destroy every register it is
 * handed.
 */
    .globl    bios_es_seg
bios_es_seg:
    .word    0
/*
 * Where the real-mode side parks the firmware's return values. Two words
 * rather than one because the two services that matter disagree about where
 * they answer: EAX is enough for a service that flags success in AH, and not
 * enough for one that walks a list and reports its position in EBX.
 */
    .globl    bios_ret_eax
bios_ret_eax:
    .long    0
    .globl    bios_ret_ebx
bios_ret_ebx:
    .long    0

/*
 * ECX and EDX, saved for the geometry query rather than returned. See the stores
 * in the real-mode exit path; bios_call leaves EAX and EBX to the caller and
 * these two for the caller to look up directly.
 */
    .globl    bios_ret_ecx
bios_ret_ecx:
    .long    0
    .globl    bios_ret_edx
bios_ret_edx:
    .long    0

/*
 * Tripwires for the round trip. See the stores in the real-mode exit path.
 * Both are cleared by the C side before each call and set here after the
 * firmware's iret returns, so a call that does not come back normally is
 * detectable from the caller rather than only as a hang.
 */

/*
 * The 32-bit loader stack.
 *
 * Not defined here at all: stage2.ld sizes and places it, at STACK32_ADDR with
 * stack32_top at STACK32_TOP. The numbers are 0xE000 and 0x20000, so the stack
 * is 0x12000 -- 72 KiB -- and stage2_top sits exactly on BOUNCE_ADDR, which is
 * the one arrangement that is safe at that boundary: the stack grows down from
 * stack32_top, so the bytes it uses are all below it and the bounce window,
 * which begins at stack32_top itself, is never touched.
 *
 * Keeping the region out of .bss is what stops the growth from being a
 * question about link order. A stack grows down from stack32_top, so anything
 * the linker placed below stack32 is what the stack eats first; with stack32 at
 * the bottom of .bss its first 3.5 KiB of growth went straight through .data
 * and into .rodata, and the first thing it overwrote was serial_puthex's digit
 * table -- so every number the loader printed came out blank, with no fault to
 * attribute it to, because the memory being overwritten was perfectly good
 * code. The measured high-water mark is under 1552 bytes.
 *
 * The trampoline state is in .trampoline, which the linker places low and
 * separately. That is the same concern from the other side: the trampoline
 * words are read and written on every single BIOS call, and a stack that ran
 * through them would corrupt the state of the call in progress.
 */
    .section .bss
    .globl stage1_boot_drive
stage1_boot_drive:
    .byte    0
