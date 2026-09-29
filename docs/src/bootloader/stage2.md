# Stage 2 Bootloader

## Entry Conditions

Stage 2 is entered from stage 1 at physical address `0x8000`, in 16-bit real mode:

- `DL` = BIOS drive number
- Stack at `0x7000:0000`

## Responsibilities

Stage 2 is written in a mix of x86 assembly and C (compiled with a 16-bit cross-compiler for the early real-mode sections, then a 64-bit cross-compiler for the long-mode section).

The sequence of operations is strictly ordered:

### Step 1 — Enable A20 Line

The A20 line must be enabled to access memory above 1 MB. Stage 2 tries three methods in order:

1. BIOS INT 15h, AX=2401h (BIOS A20 enable)
2. Port 0x92 fast A20 gate
3. Keyboard controller method (8042)

### Step 2 — Query Physical Memory Map (E820)

BIOS INT 15h, EAX=E820h is called repeatedly until the carry flag is set, building a table of memory regions. Each entry contains:

- Base address (64-bit)
- Length (64-bit)
- Type (1=usable, 2=reserved, 3=ACPI reclaimable, 4=ACPI NVS, 5=bad)

The table is stored at `0x500` (below stage 2) and passed to the kernel via `BootInfo`.

### Step 3 — Load GDT and Enter Protected Mode

A minimal GDT with null, 32-bit code, and 32-bit data descriptors is loaded. CR0.PE is set. A far jump flushes the instruction pipeline and enters 32-bit protected mode.

### Step 4 — Enter Long Mode (64-bit)

1. Build a 4-level page table (PML4 → PDPT → PD → PT) in low memory.
   - Identity map the first 4 GB (for stage 2 code to keep running).
   - Higher-half map starting at `0xFFFF_8000_0000_0000` (direct physical map).
   - Kernel window map at `0xFFFF_FFFF_8000_0000`.
2. Load CR3 with PML4 address.
3. Set EFER.LME, EFER.NXE.
4. Set CR0.PG, CR4.PAE.
5. Far jump into 64-bit code segment.

### Step 5 — Load Kernel ELF

Stage 2 reads the kernel ELF binary from disk (fixed sector offset, stored in the bootloader configuration block embedded in the binary). The ELF loader:

1. Validates ELF magic and type (ET_EXEC or ET_DYN).
2. Iterates `PT_LOAD` segments.
3. Copies each segment to its virtual load address (in the higher-half kernel window).
4. Zeroes the BSS region.

The kernel entry point (`e_entry` from the ELF header) is stored for the jump.

### Step 6 — Build BootInfo and Hand Off

`BootInfo` is filled with: E820 map, framebuffer descriptor (queried via VESA BIOS extensions in real mode earlier), ACPI RSDP address (scanned from BIOS ROM region), and kernel load base.

The stack is switched to the kernel's early boot stack. Control transfers to `kernel_main(BootInfo *)`.

## Memory Used by Stage 2

```
0x0500 – 0x6FFF    E820 memory map table
0x7000 – 0x7BFF    Stack (stage 1 and 2)
0x8000 – 0x1FFFF   Stage 2 binary
0x20000 – 0x23FFF  PML4, PDPT, PD, PT (boot page tables, 4 pages)
```

## Related Documents

- [overview.md](overview.md)
- [stage1.md](stage1.md)
- [memory-map.md](memory-map.md)
- [kernel-handoff.md](kernel-handoff.md)
- [build.md](build.md)
