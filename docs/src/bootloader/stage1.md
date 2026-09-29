# Stage 1 Bootloader

## Constraints

Stage 1 runs immediately after BIOS loads the MBR to physical address `0x7C00`. It has exactly **446 bytes** of usable space (the remaining 66 bytes are the partition table and boot signature).

The CPU is in 16-bit real mode. No C runtime is available. All code is hand-written x86 assembly.

## Responsibilities

Stage 1 does only what is necessary to load stage 2:

1. Set up segment registers (`DS`, `ES`, `SS`) to a known state.
2. Set up a real-mode stack at `0x7000:0000`.
3. Read stage 2 from the disk using BIOS INT 13h (disk read) services.
4. Jump to stage 2 entry point at `0x0000:8000`.

## Disk Read Strategy

Stage 2 is stored at a fixed location immediately following the MBR (LBA sector 1 onward). Stage 1 reads a fixed number of sectors (enough to hold the maximum expected stage 2 binary) using BIOS INT 13h extension (LBA mode):

```asm
; INT 13h, AH=42h — Extended Read Sectors From Drive
; DS:SI → Disk Address Packet
mov ah, 0x42
mov dl, [boot_drive]     ; drive number passed in DL by BIOS
lea si, [disk_address_packet]
int 0x13
jc  disk_error
```

The Disk Address Packet (DAP) specifies LBA start sector 1, count = STAGE2_SECTOR_COUNT, destination `0x0000:8000`.

## Error Handling

If the disk read fails (carry flag set), stage 1 prints a minimal error message via BIOS INT 10h (teletype mode) and halts:

```
BOOT ERROR: disk read failed
```

No recovery is possible; the system halts.

## Layout

```
0x7C00  Stage 1 code (≤ 446 bytes)
0x7DBE  Partition table (64 bytes, 4 entries × 16 bytes)
0x7DFE  Boot signature (0x55 0xAA)
```

## Build

Stage 1 is assembled with NASM:

```
nasm -f bin -o stage1.bin src/boot/stage1.asm
```

Output is exactly 512 bytes. The build system verifies this with `wc -c`.

## Handoff

At the end of stage 1:
- `DL` = BIOS drive number (preserved from BIOS entry)
- `CS:IP` = `0x0000:8000` (stage 2 entry)
- CPU: 16-bit real mode
- Stack: `0x7000:0000`

## Related Documents

- [overview.md](overview.md)
- [stage2.md](stage2.md)
- [build.md](build.md)
