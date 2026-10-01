# Stage 2 Bootloader

Stage 2 is `Fr Boot`'s second stage. It is a single binary built by
`src/boot/stage2.ld` in three modes — 16-bit real, 32-bit protected, 64-bit long
— with the mode-specific code in hand-written assembly and everything else in C
compiled with `gcc -m32`.

## Entry Conditions

Stage 2 is entered from stage 1 at physical address `0x8000`, in 16-bit real mode:

- `DL` = BIOS drive number, which stage 1 leaves at `0x7DFC` (`STAGE1_DRIVE_ADDR`)
- a real-mode stack already set up, top at `0x7E00` (`STAGE2_STACK_TOP`)

## Responsibilities

The sequence of operations is strictly ordered. The numbering matches the
`stage2_main()` body in `src/boot/stage2.c`.

### Step 0 — Mask the 8259s

```c
outb(0x21, 0xFF);   /* master: all IRQs masked */
outb(0xA1, 0xFF);   /* slave:  all IRQs masked */
```

This is first, before the UART is even reprogrammed, and it is not optional.

The BIOS leaves both PICs identity-mapped with IRQ0 *unmasked*. Nothing remaps
them until the kernel, so until this was added an 18.2 Hz timer tick was
delivered as **vector 8** for the whole of stage 2. Every disk read is a
32-bit → real-mode → firmware → 32-bit round trip that runs with `CR0.PE` clear
while stage 2's own IDT is still the installed one — and a real-mode interrupt
with a non-zero IDTR dispatches through *that* IDT, not through the IVT at
physical zero. Gate 0 pointed at the 32-bit reporter, so the tick executed
64-bit instructions as 16-bit, the decode walked into `exc_halt64`, and the
loader appeared to die at a random point with no diagnostic.

`cli` does not help: it runs after the trampoline returns, the tick lands
*during* the call, and the firmware's own `iret` restores IF.

Stage 2 is single-threaded and has no interrupt handler worth re-entering, so
nothing is lost. The kernel remaps and unmasks the PICs itself in
`pic_remap()`. Note the consequence: until a timer driver exists, **no interrupt
of any kind is ever delivered**.

### Step 1 — Enable A20 Line

The A20 line must be enabled to access memory above 1 MiB. `a20_enable()` tries
three methods, in this order, and judges each one by an alias test rather than by
its return value:

1. **The 8042 keyboard controller's output port.** Disable the keyboard, read the
   output port, set bit 1, write it back, re-enable the keyboard. The reply is
   only read if the controller reported one — reading port `0x60` with OBF clear
   returns whatever the last keyboard byte was, and writing *that* to the output
   port toggles the CPU reset line. Bit 0 is written back **clear** for the same
   reason.
2. **The port 0x92 fast gate.** A plain read-modify-write of bit 1, then a
   read-back of bit 1.
3. **INT 15h, AX=2401h.** Tried with `BX=0` (8042) and then `BX=1` (port 0x92);
   which of the two a given BIOS honours is not knowable from stage 2, so both
   are attempted.

The test all three share is `a20_is_open()`: write a byte to `0x100500`, write a
different one to `0x500`, and check that neither write appeared at the other
address. Both locations are ordinary RAM that nothing else owns and that the
kernel image has not yet reached. A closed gate makes `0x500` and `0x100500` the
same address, so this is a three-write test with no lasting effect.

If none of the three opens the gate — or if the gate is already open by
firmware — stage 2 says which and what. **It does not carry on.** A machine whose
A20 gate will not open cannot run this loader, and the alternative produces a
kernel image written over the interrupt vector table with every access in
between landing somewhere valid and nothing reporting anything at all.

On the reference machine the 8042 path does not answer and the fast gate does,
so the log reads:

```
[boot2] A20 enabled via port 0x92 fast gate
```

### Step 2 — Query Physical Memory Map (E820)

INT 15h, EAX=E820h is called repeatedly to build a table of memory regions. Each
entry contains:

- Base address (64-bit)
- Length (64-bit)
- Type (1=usable, 2=reserved, 3=ACPI reclaimable, 4=ACPI NVS, 5=bad)

Two conventions exist for this call and a loader written to one and run against
the other gets nothing at all, silently:

| | signature | entry index | destination | "another entry?" | next index |
|---|---|---|---|---|---|
| Ralf Brown / SeaBIOS | EAX, and DX:BP | EBX | ES:DI | carry clear | EBX, 0 on last |
| Specification form | EBX | none | EDX, linear | AX bit 19 **set** | none |

`bios_call()` hands back the firmware's **EBX:EAX**, so the signature arrives in
the *high* half of the 64-bit return value in both conventions. Reading it out of
the low half cannot match. `e820_call()` therefore tries the SeaBIOS form first,
falls back to the specification form, and manufactures an index for the latter
because that convention does not return one. Both are reduced to a single "index
of the next entry, 0 for last" so the caller has one loop and one stop test.

The table is stored at **`0x00090000`** (`E820_ADDR`, `src/include/boot.h`) with
at most 128 entries (`E820_MAX_ENTRIES`), which is exactly `128 * 24 == 0xC00`
bytes. It is passed to the kernel in `struct bootinfo`; the kernel re-checks every
range against its own accounting before trusting it.

### Step 3 — Load GDT and Enter Protected Mode

A GDT with null, 32-bit code, 32-bit data, and a user code/data pair at DPL 3 is
built in C and installed. CR0.PE is set and a far jump flushes the instruction
pipeline into 32-bit protected mode. The GDT and IDT pseudo-descriptors are
written **by C**, not by the assembler: `.word stage2_gdt_limit` looks like it
stores a limit, but `stage2_gdt_limit` is a label, so the assembler emits a
relocation and the linker fills in the label's own address — LGDT would then load
a limit that is an address and a base pointing at the two variables.

The bootstrap IDT has 256 gates, all pointing at the **32-bit** reporter. They are
repointed at the 64-bit reporter as the last act before the far jump, so no gate
is ever entered by a CPU of the wrong width.

### Step 4 — Enter Long Mode (64-bit)

1. Build the bootstrap page tables in a 64 KiB pool at `0x002D0000`
   (`BOOT_PT_ADDR`): a PML4, a PDPT and, at offset `0xA000` inside the pool, one
   4 KiB page table.
   - Identity map 0 – 4 GiB with 2 MiB pages, so stage 2's own code and the
     firmware keep working.
   - Map the kernel window `0xFFFFFFFF80000000` – `+16 MiB` to physical
     `KERNEL_LANDING_ADDR` (`0x100000`) `+offset` with 4 KiB pages. The first
     2 MiB has to be 4 KiB because `KERNEL_LANDING_ADDR` is not 2 MiB-aligned and
     a 2 MiB page must be.
   - The window is reached through **PML4 511 / PDPT 510**. 511 is a real 1 TiB
     region starting at `0xFFFFFFFFC0000000`; 510 is the 1 TiB region the kernel
     is actually in. Both indices are derived from `KERNEL_VIRT_BASE` by
     `PML4_ENTRY_OF`/`PDPT_ENTRY_OF` so stage 2 and the kernel cannot disagree.
2. Fill `gdtr64` in C and load it.
3. Repoint every IDT gate at the 64-bit reporter.
4. Set CR4.PAE, EFER.LME and EFER.NXE; enable XCR0 for x87 + SSE + AVX.
5. Load CR3, set CR0.PG, and far-jump into 64-bit code.

There is **no direct map in stage 2.** The kernel builds its own when it starts
(`vmm: direct map 0-4 GiB, 2 MiB pages` in the boot log). Stage 2's only job is
to make the kernel window exist.

### Step 5 — Load Kernel ELF

Stage 2 reads the kernel ELF from disk at `KERNEL_LBA` (64) in 512-byte sectors,
one firmware call per sector, each through the 4 KiB bounce window at
`BOUNCE_ADDR` (`0x20000`). The image is copied to `KERNEL_LANDING_ADDR`
(`0x100000`) because INT 13h cannot address above 1 MiB. For each `PT_LOAD`
segment the file bytes are copied to `KERNEL_LANDING_ADDR + p_offset`, and the
bytes between `p_filesz` and `p_memsz` — `.bss` — are zeroed here because the
kernel's allocator is not running yet.

The header is checked for the `\x7fELF` magic, `ELFCLASS64`, and `EM_X86_64`, and
anything else is a `fail()`. `e_entry` is then taken from the header verbatim.

### Step 6 — Build Bootinfo and Hand Off

`struct bootinfo` (`src/include/boot.h`, magic `0x4F53424F4F543031` /
`"OSBOOT01"`) is filled at `BOOTINFO_ADDR` (`0x00091000`) with: the magic and
version, the kernel's physical and virtual base and entry point, the E820 map's
address and entry count, the framebuffer descriptor, the boot drive, and a
command line.

Its physical address is passed to the kernel entry point in **RDI**. There is no
agreed register convention beyond that: RSP is still the loader's own 32-bit
stack, CR0.WP and CR4.PGE are not set, and no other register is meaningful.

Control transfers to the kernel's `_start` at the address in RDI. Nothing
returns; if it ever did, `fail()` reports the fact.

## Memory Used by Stage 2

```
0x000000 – 0x0009FBFF   conventional RAM, low 1 MiB (MBR, stage 1 image,
                         stage 2's real-mode scratch, IVT/BIOS vectors)
0x008000 – 0x00DFFF     stage 2 image (.text, .trampoline, .rodata, .data,
                         .bss) — at most 24 KiB, enforced by the linker
0x00E000 – 0x01FFFF     32-bit loader stack, 72 KiB, grows down from 0x20000
0x020000 – 0x020FFF     firmware bounce window, one 512-byte sector at a time
0x021000 – 0x021FFF     program-header scratch (DAP, VBE scratch)
0x090000 – 0x090BFF     E820 map, up to 128 entries
0x091000 – 0x0910E7     struct bootinfo
0x100000 – 0x2C7FFF     the kernel image, once loaded
0x2D0000 – 0x2DFFFF     bootstrap page tables
```

The 32-bit loader stack's top is deliberately the same address as the bounce
window's bottom. A downward-growing stack writes below its top, so the two touch
without overlapping, and `stage2.ld` asserts both `__bss_end < STACK32_ADDR` and
`stack32_top <= BOUNCE_ADDR`.

## Known Gaps

These are real and unfixed; they are listed here rather than discovered later.

- `hang_puts()`, the one routine whose job is to explain a failed transition,
  clobbers the character with the UART line-status byte before transmitting it,
  so it emits a stream of `0x60` and none of the message.
- The stage 2 banner still prints `abcdefg` on every boot.
- `LGDT` is emitted in the m16&16 form. It works only because the GDT sits below
  64 KiB.
- A20 is enabled unconditionally at boot even when the firmware already opened
  the gate.
- `e_entry` is not bounds-checked against the loaded segments, and `e_type` is
  not checked at all.
- The stage 2 GDT's index 3 is labelled "64-bit user code, DPL 3" but its flags
  byte is `0x00`, which is a 16-bit code segment. Nothing in stage 2 uses it;
  the kernel builds its own GDT.

## Related Documents

- [overview.md](overview.md)
- [stage1.md](stage1.md)
- [memory-map.md](memory-map.md)
- [kernel-handoff.md](kernel-handoff.md)
- [build.md](build.md)
