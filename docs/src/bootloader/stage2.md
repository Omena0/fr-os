# Stage 2 Bootloader

Stage 2 is `Fr Boot`'s second stage. It is a single binary built by
`src/boot/stage2.ld` in three modes — 16-bit real, 32-bit protected, 64-bit long
— with the mode-specific code in hand-written assembly and everything else in C
compiled with `gcc -m32` for 16/32-bit sections and `x86_64-elf-gcc` for the
64-bit section.

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

These are at `stage2.c:1795-1796`, **before the first firmware call** and
therefore before anything that can be interrupted by one. They are *not* the
first thing `stage2_main()` does: `serial_init()` runs at :1753 and the banner
at :1798, because the first diagnostic has to have a known baud rate. Both of
those are plain port I/O with no BIOS involvement, so the ordering requirement is
"before the first `bios_call`", not "before anything". An earlier version of this
page and of audit section 0.1 both said the stronger thing; both were wrong, and
the source comment at stage2.c:1757-1758 says it correctly.

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
   output port, set bit 1, write it back, re-enable the keyboard.

   The reply is only read if the controller reported one. Reading port `0x60`
   with OBF clear returns whatever the last keyboard byte was, and writing
   *that* to the output port can reset the CPU. So the value read back is also
   checked before it is written: a genuine read of a running machine's output
   port always has **bit 0 set**, and no scancode byte has that property, so a
   reply with bit 0 clear is rejected as "not a port value" rather than written
   back.

   **Bit 0 is written back SET, and the polarity is the opposite of what the name
   suggests.** In the 8042 output port, 1 means "running" and 0 means "reset the
   machine" — QEMU models it exactly this way (`hw/input/pckbd.c`:
   `outport_write()` calls `qemu_system_reset_request()` when `!(val & 1)`), and
   its default outport is `0xCF` = reset-inactive | A20 | `0xCC`. The code
   therefore sets `status | 0x02`: bit 1 on, bit 0 **forced on**, every other
   bit passed through untouched, because bits 4 and 5 mirror the output-buffer
   flags and rewriting them would corrupt the keyboard. Copying port 0x92's
   polarity here reboots the machine instead of enabling A20.

   After the write the register is read back and bit 1 required. The latch is
   shared with port 0x92, and which of the two a chipset actually wires to the
   A20 line is not knowable from here, so the fast gate's read-back below is a
   second witness.
2. **The port 0x92 fast gate.** A read-modify-write that sets bit 1 **and clears
   bit 0**, then a read-back of bit 1. Bit 0 is the opposite control to the
   8042's: in port 0x92 a 1 is the CPU *reset request* and a 0 is normal, so it
   is explicitly driven low. A "plain read-modify-write of bit 1" that left bit
   0 alone would leave a reset request standing.
3. **INT 15h, AX=2401h.** Tried with `BX=0` (8042) and then `BX=1` (port 0x92);
   which of the two a given BIOS honours is not knowable from stage 2, so both
   are attempted.

The test all three share is `a20_is_open()`: write `0x00` to `0x100500`, write
`0x5A` to `0x500`, and if `0x5A` came back out of `0x100500` the gate is closed;
then write `0xA5` to `0x100500` and if `0x500` no longer reads `0x5A` it is
closed. Both locations are ordinary RAM that nothing else owns and that the
kernel image has not yet reached. A closed gate makes `0x500` and `0x100500` the
same address, so this is a three-write test with no lasting effect.

If none of the three opens the gate — or if the gate is already open by
firmware — stage 2 says which and what. **It does not carry on.** A machine whose
A20 gate will not open cannot run this loader, and the alternative produces a
kernel image written over the interrupt vector table with every access in
between landing somewhere valid and nothing reporting anything at all.

Note that the "already open by firmware" case is checked *last*, after all three
methods have been tried, and it accepts either the alias test or port 0x92 bit 1
as evidence — some chipsets only answer the alias test once the gate is open
through whatever mechanism the firmware used before stage 2 ran.

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

A four-entry GDT is built in C and installed, and CR0.PE is set with a far jump
into 32-bit protected mode. Decoded from the literals at `stage2.c:1706-1709`
(byte 5 is the access byte, byte 6 holds the flags nibble in its low four bits
and `limit[19:16]` in its high four):

| entry | value | access | flags | limit | what it actually is |
|---|---|---|---|---|---|
| 0 | `0` | — | — | — | null |
| 1 (0x08) | `0x00AF9A000000FFFF` | `0x9A` P=1 DPL=0 code exec/read | `0xF` G=1 D/B=1 **L=1** AVL=1 | `0xAFFFF` (11 MiB) | a **64-bit** code segment, base 0 |
| 2 (0x10) | `0x00CF93000000FFFF` | `0x93` P=1 DPL=0 data RW | `0xF` G=1 D/B=1 (L ignored for data) | `0xCFFFF` (13 MiB) | flat data, base 0 |
| 3 (0x18) | `0x0000FA000000FFFF` | `0xFA` P=1 **DPL=3** code exec/read | `0x0` G=0 D/B=0 **L=0** | `0xFFFF`, G=0 (64 KiB) | a **16-bit** code segment — see Known Gaps |

Two things about this table are worth stating because they are easy to get wrong
by reading the comments instead of the bytes:

  * **Entry 1 has `L=1`, so it is a 64-bit code descriptor, not a 32-bit one.**
    `L` lives in the low bit of the flags nibble, and the flags nibble is `0xF` —
    all four bits set. It is used as the CS that the far jump lands on while the
    CPU is still in 32-bit protected mode, which is worth a second look before
    anyone "simplifies" the literal; see Known Gaps.
  * **Neither entry 1 nor entry 2 is 4 GiB.** `stage2.c:1707` comments entry 1
    `/* 0x08 64-bit code, base 0, 4G */`, but `limit[19:16]` is `0xA`, not `0xF`,
    so the encoded limit is `0xAFFFF` — 11 MiB, with granularity applied. The
    same applies to entry 2 at 13 MiB. Both are comfortably larger than the
    loader image, which `stage2.ld` caps at 24 KiB, so nothing can fault on it;
    it is the comment that is wrong, not the boot.

The GDT and IDT pseudo-descriptors are written **by C**, not by the assembler:
`.word stage2_gdt_limit` looks like it stores a limit, but `stage2_gdt_limit` is
a label, so the assembler emits a relocation and the linker fills in the label's
own address — LGDT would then load a limit that is an address and a base
pointing at the two variables.

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

There is **no direct map in stage 2.** The kernel builds its own when it starts,
and says so on every boot — `vmm: direct map 0-4 GiB, 2 MiB pages`, or `1 GiB
pages` on a CPU that reports PDPE1GB. Stage 2's only job is
to make the kernel window exist.

### Step 5 — Load Kernel ELF

Stage 2 reads the kernel ELF from disk at `KERNEL_LBA` (64) in 512-byte sectors
through the 4 KiB bounce window at `BOUNCE_ADDR` (`0x20000`). One firmware call
moves **up to eight** sectors: `bios_read_bounce()` caps the batch at
`BOUNCE_BYTES / 512` and also at the rest of the current track, because a CHS
read may not cross a track boundary. The cap is the bounce window that matters —
the track bound alone allows 63 sectors (32 KiB), and the firmware writes all of
it to whatever buffer it was handed, overrunning the disk address packet and the
VBE scratch block above it and reporting success. Nothing faults; the next
sector's parameters are simply gone. The image is copied to
`KERNEL_LANDING_ADDR` (`0x100000`) because INT 13h cannot address above 1 MiB.
For each `PT_LOAD` segment the file bytes are copied to
`KERNEL_LANDING_ADDR + p_offset`, and the bytes between `p_filesz` and
`p_memsz` — `.bss` — are zeroed here because the kernel's allocator is not
running yet.

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
0x000000 – 0x0009FBFF   conventional RAM, low 1 MiB (MBR, stage1 image,
                          stage 2's real-mode scratch, IVT/BIOS vectors)
0x008000 – 0x00DFFF     stage 2 image (.text, .trampoline, .rodata, .data,
                          .bss) — at most 24 KiB, enforced by the linker
0x00E000 – 0x01FFFF     32-bit loader stack, 72 KiB, grows down from 0x20000
0x020000 – 0x020FFF     firmware bounce window, up to eight 512-byte sectors
                          per firmware call (BOUNCE_BYTES = 4 KiB)
0x021000 – 0x0210FF     INT 13h disk address packet (DAP_ADDR)
0x021100 – 0x021FFF     VBE scratch, 0xF00 bytes (VBE_SCRATCH_ADDR)
0x022000 – 0x022FFF     one program header reassembled out of the bounce window
                          (PHDR_SCRATCH_ADDR)
0x090000 – 0x090BFF     E820 map, up to 128 entries
0x091000 – 0x0910E7     struct bootinfo
0x100000 – 0x2CFFFF     the kernel image landing zone, once loaded
                          (KERNEL_MAX_BYTES = 0x1D0000)
0x2D0000 – 0x2DFFFF     bootstrap page tables
```

**Do not take the physical-layout comment at the top of `src/boot/boot_layout.h`
as the authority for any of this.** It is a map of the loader *before* the
bounce buffer was moved from 0x10000 to 0x20000, so it still places the bounce
window, the DAP, the VBE scratch and the program-header scratch a page lower
than they are, still describes the loader stack as living at 0x20000-0x2FFFF
(it is at 0xE000-0x1FFFF, growing down from the bounce window's bottom), and
gives the kernel landing zone as 1.875 MiB where the constant is 1.8125 MiB.
This table is derived from the constants in that same header; audit finding
**#178** is the writeup.

The 32-bit loader stack's top is deliberately the same address as the bounce
window's bottom. A downward-growing stack writes below its top, so the two touch
without overlapping, and `stage2.ld` asserts both `__bss_end < STACK32_ADDR` and
`stack32_top <= BOUNCE_ADDR`.

## Design Targets

The intended stage 2 design includes:

- **`LGDT` and `LIDT` in the m16&32 form**: emitted with the `0x66` prefix in `.code16`, giving a 16-bit limit and a 32-bit base. Tables may sit anywhere in the first 4 GiB; `.bss` growth is bounded by the full 32-bit address space, not by a 64 KiB assert.
- **`e_entry` bounds-checked**: validated against the loaded segments, and `e_type` checked before use.
- **Consistent GDT literals**: stage 2 uses the same selectors as the kernel's `gdt.h`, and the GDT entries themselves are verified for correct L, D/B, and G bits.
- **32-bit/64-bit compatibility handling**: the far jump into protected mode uses a descriptor that is valid in the current mode.

## Related Documents

- [overview.md](overview.md)
- [stage1.md](stage1.md)
- [memory-map.md](memory-map.md)
- [kernel-handoff.md](kernel-handoff.md)
- [build.md](build.md)