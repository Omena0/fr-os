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
 *   0x0000_0000 - 0x0000_7BFF  BIOS data area, real-mode IVT
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

/* ------------------------------------------------------------ stage 2 ------ */
#define STAGE2_ADDR           0x00008000u
#define STAGE2_STACK_TOP      0x00007E00u  /* real-mode stack, grows down */

/* ----------------------------------------------------- bootstrap paging ---- */
#define BOOT_PT_ADDR          0x00070000u
#define BOOT_PT_BYTES         0x00010000u

/* ------------------------------------------------------------ handoff ------ */
#define E820_ADDR             0x00090000u
#define E820_MAX_ENTRIES      128u
#define BOOTINFO_ADDR         0x00091000u

#endif /* BOOT_LAYOUT_H */
