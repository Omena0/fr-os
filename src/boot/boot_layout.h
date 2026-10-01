/*
 * boot_layout.h — disk geometry and the early physical memory map.
 *
 * Shared by the bootloaders and the kernel. The disk image is intentionally
 * unpartitioned: each stage loads from a fixed LBA, which keeps the boot path
 * free of any filesystem dependency and lets stage2 read the kernel as a plain
 * ELF64 image.
 *
 * The low-memory map below is the contract between stage1, stage2, and the
 * kernel. Two constraints shape it:
 *
 *   - INT 13h/AH=42h addresses its destination as a 16-bit segment:offset
 *     pair, so every BIOS transfer must land below 1 MiB. The kernel's physical
 *     load address therefore cannot be written directly; stage2 bounces every
 *     byte through a buffer below 1 MiB.
 *   - A single INT 13h/AH=42h call may not cross a 64 KiB boundary. The bounce
 *     helpers transfer at most 16 KiB per call, which makes that impossible to
 *     violate by construction.
 *
 * Physical layout while stage2 runs:
 *
 *   0x0000_0000 - 0x0000_04FF  real-mode IVT and BIOS data area
 *   0x0000_0500 - 0x0000_7BFF  free low memory (BIOS vector word)
 *   0x0000_7C00 - 0x0000_7DFF  stage1 (MBR)
 *   0x0000_8000 - 0x0000_FFFF  stage2 code + data, executed in place
 *   0x0001_0000 - 0x0001_0FFF  BIOS bounce buffer, 4 KiB per transfer
 *   0x0001_1000 - 0x0001_10FF  INT 13h disk address packet
 *   0x0001_1100 - 0x0001_1FFF  INT 10h/VBE scratch structures
 *   0x0002_0000 - 0x0006_FFFF  kernel image landing zone, 320 KiB
 *   0x0007_0000 - 0x0007_FFFF  bootstrap page tables
 *   0x0009_0000 - 0x0009_0BFF  E820 memory map, 128 entries
 *   0x0009_1000 - 0x0009_1FFF  struct bootinfo
 */
#ifndef BOOT_LAYOUT_H
#define BOOT_LAYOUT_H

/* --------------------------------------------------------------- disk ------ */
#define STAGE2_LBA            1u     /* stage1 loads stage2 from here */
#define STAGE2_MAX_SECTORS    63u    /* 63 * 512 = 32256 bytes of stage2 */
#define KERNEL_LBA            64u    /* stage2 loads the kernel ELF from here */

/* ------------------------------------------------------- kernel landing ---- */
/*
 * stage2 copies the kernel image here before paging exists, because INT 13h
 * cannot address above 1 MiB. The window is generous enough for a kernel that
 * has grown a real amount of code.
 */
#define KERNEL_LANDING_ADDR   0x00020000u
#define KERNEL_MAX_BYTES      0x00050000u  /* 320 KiB */

/* -------------------------------------------------- BIOS transfer window -- */
#define BOUNCE_ADDR           0x00010000u
#define BOUNCE_BYTES          0x00001000u  /* 4 KiB: comfortably under 64 KiB */
#define DAP_ADDR              0x00011000u
#define VBE_SCRATCH_ADDR      0x00011100u
#define VBE_SCRATCH_BYTES     0x00000F00u

/*
 * Where the BIOS trampoline's vector number lives.
 *
 * A fixed absolute address rather than a .bss symbol, and the reason is an
 * assembler one that costs an afternoon if it is rediscovered. The trampoline
 * issues `int` with the memory operand form, whose encoding is the one-byte
 * opcode followed by a 16-bit *offset* that names the word holding the vector
 * number. GAS will not assemble that form at all, and when the operand is
 * written out as raw bytes `.word bios_call_vector` does not do what it looks
 * like: it emits the symbol's own address, not the value stored there. The call
 * went out as `int $0xbb20` and never reached the firmware.
 *
 * GAS has no dereference operator for a symbol in an expression, so there is no
 * spelling of `.word` that reads the word's contents. Giving the word a fixed
 * address instead makes `.word` a plain constant, which is what it does emit.
 *
 * The offset is relative to the segment the `int` executes in, and the
 * trampoline far-jumps to CS = 0 before it fires, so the offset is also the
 * linear address and has to be below 64 KiB. 0x5000 is the low 32 KiB of
 * conventional memory that nothing in the boot path claims: the IVT and BDA end
 * at 0x0500, the real-mode stack lives at 0x7E00, stage1 at 0x7C00, stage2 at
 * 0x8000, and the transfer window starts at 0x10000. stage2.ld asserts the
 * bound, so a slot moved above 64 KiB is a link error rather than an `int`
 * that silently reads some unrelated low word and calls whatever vector that
 * word happens to name.
 *
 * Note there is no `& 0xFFFF` on the operand that encodes this: in an x86 GAS
 * expression `&` is the high-byte register prefix, not a bitwise AND, and the
 * masked expression folds to 0 -- which turns the call into `int $0x0`.
 */
#define BIOS_VECTOR_ADDR      0x00005000u

/*
 * Far pointer to the firmware handler for BIOS_VECTOR_ADDR: the offset the IVT
 * holds at 4 * vector, followed by the segment, as two 16-bit words.
 *
 * The pointer is resolved on the 32-bit side, before the mode switch, and left
 * here so that the 16-bit side can make the call with a single absolute operand
 * and no register of its own. There is no register it could use: every one of
 * AX, BX, CX, DX, SI and DI is about to be handed to the firmware as an input,
 * and 16-bit arithmetic on one of them corrupts the call rather than the
 * address. See bios_call.
 *
 * Resolving it early also means the real-mode side never reads the IVT itself,
 * which it could not do correctly anyway without a register to hold the address
 * in. Physical zero is readable from the flat 32-bit segments with paging off,
 * so the IVT entry is fetched while it is still just four bytes of memory.
 */
#define BIOS_IVT_PTR          0x00005004u

/*
 * Interrupt vector numbers, as numbers.
 *
 * "INT 15h" is hexadecimal 0x15, which is decimal 21. Writing the call as
 * bios_call(15, ...) asks for vector 0x0F, and it fails in a way that is very
 * hard to read: the trampoline resolves vector 0x0F's IVT entry, far calls
 * whatever handler the firmware has there, and the return value is the answer to
 * a question nobody asked. INT 15h/AH=E820h at vector 0x0F lands on
 * INT 0Fh/AH=4F00h, the VESA controller query, which answers with its own
 * signature and leaves the 24-byte E820 entry buffer holding the caller's 0xA5
 * poison -- so the loader prints 0xA5A5A5A5A5A5A5A5 as a base address and
 * carries on. The disk read at vector 0x0D is not a disk service at all and
 * fails, and the VBE calls at vector 0x0A do the same.
 *
 * These are the numbers, written in hex, so that the digits in the comment at
 * each call site and the digits here are the same digits.
 */
#define BIOS_INT_VIDEO         0x10u
#define BIOS_INT_DISK          0x13u
#define BIOS_INT_E820          0x15u

/* ------------------------------------------------------------ stage 2 ------ */
#define STAGE2_ADDR           0x00008000u
#define STAGE2_STACK_TOP      0x00007E00u  /* real-mode stack, grows down */

/*
 * Stack the BIOS trampoline hands to the firmware.
 *
 * SS:SP has to be a real-mode pair, so SP must stay under 64 KiB, and the
 * firmware pushes a full interrupt frame plus its own register save onto it
 * before doing anything else. 0x7E00 is above stage1's image and below the
 * transfer window, so nothing the firmware writes there can collide with the
 * structures stage2 is actively using. It is the same address stage2's own
 * real-mode entry uses; the two are never live at once, since the entry path
 * has already switched to the 32-bit stack by the time a BIOS call happens.
 */
#define BIOS_RM_STACK_TOP     0x00007E00u

/*
 * GDT selector for the 16-bit code segment the BIOS trampoline far jumps to.
 *
 * This is the load-bearing step of the round trip. Clearing CR0.PE does not
 * reload CS, so the code segment register keeps the 32-bit descriptor it was
 * last loaded with and the CPU keeps decoding with that descriptor's D bit --
 * real mode or not. Only a far jump replaces the cached descriptor, and only a
 * far jump taken while PE is still set resolves its operand through the GDT
 * rather than as a raw segment value.
 *
 * So the trampoline loads this descriptor first, and clears PE afterwards, in
 * code that is already being fetched as 16-bit. The matching .quad is the
 * fourth entry of stage2_gdt in stage2_entry.S; the two have to agree, and
 * nothing in the build checks that, so a change to one has to be made to the
 * other.
 */
#define BIOS_RM_CODE_SEL      0x0018u

/*
 * stage2's .bss must end below this line.
 *
 * The bounce buffer, the disk address packet and the VBE scratch block sit at
 * 0x10000..0x11FFF, and stage2's own .bss is placed by the linker immediately
 * after .rodata. Nothing reserves the gap between the two, so a .bss that grew
 * past 0x10000 would land on top of them: the 32 KiB loader stack did exactly
 * that, and the failure only shows up as a BIOS transfer whose destination has
 * been overwritten by the loader's own return addresses. stage2.ld asserts on
 * this so the collision is a link error rather than a boot-time mystery.
 */
#define STAGE2_BSS_LIMIT      0x00010000u

/* ------------------------------------------------------- stage 1 handover -- */
/*
 * Where stage1 leaves the BIOS-supplied boot drive number for stage2.
 *
 * The MBR is 512 bytes and is already within a few bytes of full, so the
 * handover cannot be a value pushed on the stack: adding the push costs four
 * bytes of code, which does not fit, and the resulting image silently stops
 * booting rather than failing to assemble. It is a fixed offset instead,
 * chosen inside the zero padding the linker inserts before the 0x55AA
 * signature.
 *
 * 0x7DFC is offset 508: past the end of stage1's code and data (which end at
 * mbr_tail, 499 today) and clear of the signature at 510. Because it lives in
 * the padding, storing to it needs no room reserved in the image at all --
 * stage1 writes the byte at run time and the linker still emits 512 bytes.
 * The only requirement is that stage1's code and data stay below offset 508,
 * which the `.space` guard already enforces against the 510 limit.
 *
 * The two images are linked separately and share no symbol table, so this is
 * the one piece of memory both can agree on without agreeing on a name.
 */
#define STAGE1_DRIVE_ADDR     0x00007DFCu

/* ----------------------------------------------------- bootstrap paging ---- */
#define BOOT_PT_ADDR          0x00070000u
#define BOOT_PT_BYTES         0x00010000u

/* ------------------------------------------------------------ handoff ------ */
#define E820_ADDR             0x00090000u
#define E820_MAX_ENTRIES      128u
#define BOOTINFO_ADDR         0x00091000u

#endif /* BOOT_LAYOUT_H */
