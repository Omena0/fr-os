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
 *   0x0002_0000 - 0x0002_FFFF  stage2 image + loader stack, up to 64 KiB
 *   0x0009_0000 - 0x0009_0BFF  E820 memory map, 128 entries
 *   0x0009_1000 - 0x0009_1FFF  struct bootinfo
 *   0x0001_2000 - 0x0001_2FFF  program header reassembly scratch
 *   0x0010_0000 - 0x002C_8FFF  kernel image landing zone, 1.875 MiB
 *   0x002D_0000 - 0x002D_FFFF  bootstrap page tables
 *
 * Three of these addresses are load-bearing in ways that are easy to undo by
 * accident, so they are recorded here rather than left to be rediscovered.
 *
 * The E820 map and bootinfo stay below 1 MiB because the firmware writes the
 * map through a real-mode segment:offset pair, and even with A20 enabled that
 * names 20 bits of address and no more. 0x210000 returned a map that was
 * entirely the caller's poison -- the buffer was out of reach. 0xF0000 is the
 * VESA option ROM window and returned fragments of ROM code. 0xE0000 returned
 * two zeroed entries and no more: a real answer to a real call, and not the
 * map.
 *
 * The landing zone is above 1 MiB, not below it, and that inverts the usual
 * arrangement for a reason. It is sized by p_memsz rather than p_filesz,
 * because the difference is .bss, which the kernel owns from physical zero
 * once it runs. This kernel's last PT_LOAD is 128 bytes of file data with a
 * 1.5 MiB memsz, so a window sized by the file was 320 KiB and put the page
 * tables, the E820 map and bootinfo in the middle of the kernel's own memory.
 * Nothing reported it: the copy succeeded, the loader carried on, and the
 * kernel was handed a page table that had already been overwritten.
 *
 * The zone starts at 0x100000 rather than 0x20000 for the same reason from the
 * other end. The kernel needs 0x1C9000 contiguous bytes, which cannot fit
 * below the E820 map at 0x90000 -- so the zone goes above it rather than the
 * map being pushed somewhere the firmware cannot reach.
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
 * cannot address above 1 MiB.
 *
 * The window is sized by p_memsz, not p_filesz, and the distinction is not
 * academic. The last PT_LOAD segment of this kernel is 128 bytes of file data
 * with a 1.5 MiB memsz: the difference is .bss, which the kernel is entitled
 * to use from physical zero on. Measuring the file alone gave a 320 KiB window
 * and put the stage2 scratch structures -- page tables at 0x70000, the E820
 * map at 0x90000, bootinfo at 0x91000 -- in the middle of the kernel's own
 * memory. Nothing reported the collision: the copy succeeded, the loader
 * carried on, and the kernel was handed a page table it had already
 * overwritten.
 *
 * KERNEL_MAX_BYTES is therefore a floor checked against the real memsz
 * extent, and the loader fails loudly rather than writing past the window.
 * INT 13h reads still land at 0x10000, so an address above 1 MiB is only ever
 * a destination the loader writes with ordinary stores.
 *
 * KERNEL_LANDING_ADDR itself now lives in boot.h, next to KERNEL_VIRT_BASE,
 * because the kernel's own page tables have to describe the same mapping and
 * must not be told where it is by a second copy of the number.
 */
#define KERNEL_MAX_BYTES      0x001D0000u  /* 1.8125 MiB: see above */

/* -------------------------------------------------- BIOS transfer window -- */
#define BOUNCE_ADDR           0x00020000u
#define BOUNCE_BYTES          0x00001000u
#define DAP_ADDR              0x00021000u
/*
 * Scratch for one program header while it is being assembled out of the bounce
 * window. It cannot live in the landing zone: stage2 reads the ELF header from
 * there, and a header that straddles a sector boundary is reassembled in place
 * before the loop advances -- which for the first header is a write on top of
 * the magic stage2 has already checked, leaving "kernel is not an ELF image"
 * for a kernel that is one.
 */
#define PHDR_SCRATCH_ADDR     0x00022000u
#define VBE_SCRATCH_ADDR      0x00021100u
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

/*
 * The loader stack lives above the bounce buffer, not in the gap between the
 * stage2 image and 64 KiB.
 *
 * That gap was 0xC800-0xF000, 10 KiB, with the firmware transfer window at
 * 0x10000 treated as immovable because it has to be reachable from real mode.
 * But only the *16-bit* half of the loader has that constraint: the BIOS
 * trampoline state, which it addresses by absolute 16-bit displacement. The
 * stack is only ever walked by 32-bit code, so nothing requires it below 64
 * KiB, and it does not have to share the gap with the image.
 *
 * Sharing it was the actual problem. The stage2 image has no fixed size, and
 * one of its sections is the bootstrap IDT, which is 4 KiB because a long
 * mode gate is 16 bytes and there are 256 of them. That put the top of
 * .rodata within a few hundred bytes of 0xC800, and every addition anywhere in
 * the loader pushed it over. When it did, the stack started inside the image:
 * the hex digit table, which is the last thing in .rodata, was the first thing
 * overwritten, and every number the loader printed came out as NUL bytes. The
 * linker's ASSERT caught it, but only when the growth crossed the line -- on
 * builds that stayed under it the collision was silent and the symptom looked
 * like a serial driver fault.
 *
 * Moving the firmware window up to 0x20000 and the stack above it gives the
 * image 0x8000-0xC800 to itself and the stack 56 KiB, so neither is close to
 * the other and neither has to be tuned.
 */
#define STACK32_ADDR          0x0000E000u
#define STACK32_TOP           0x00020000u

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
#define BOOT_PT_ADDR          0x002D0000u
#define BOOT_PT_BYTES         0x00010000u
/* How much of that window is zeroed before it is filled in. It has to cover
 * the last table in use, not the nominal window size: the page tables occupy
 * 0x0000-0x9FFF and the 4 KiB page table for the kernel window starts at
 * 0xA000, so zeroing only 0x8000 bytes would leave it holding whatever the
 * firmware left in memory. */
#define BOOT_PT_ZERO_BYTES    0x0000C000u

/* ------------------------------------------------------------ handoff ------ */
/* E820_ADDR and BOOTINFO_ADDR come from boot.h, which the kernel also reads,
 * so the two images cannot disagree about where the handoff record lives. */
#define E820_MAX_ENTRIES      128u

/* The two handoff addresses are shared with the kernel; see boot.h. */
#include "boot.h"

#endif /* BOOT_LAYOUT_H */
