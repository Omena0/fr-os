================================================================================
SOURCE TREE AUDIT — /home/omena0/Github/os   @ b1df707
Read-only. No file in the repository was created, edited, or deleted.
Build + boot verification performed on a copy at /tmp/kilo/os-audit2.

                        --- STATUS AS OF 2026-10-01, ~23:40 local ---
This file was written against revision b1df707 and was accurate then. A great
deal has been fixed since. It is kept verbatim, in full, because the reasoning
in each finding is still worth having; what has changed is that every finding
now carries a `>> STATUS:` line saying whether it is still open, fixed, or was
never right. Nothing has been deleted.

Read the STATUS line, not the finding. A finding without one does not exist here:
all 177 have one. A `FIXED` line names the file:line or the boot-log line that
proves it. Where a status could not be established from the tree, the line says
so rather than guessing.

Statuses in use:
  FIXED            verified fixed in the working tree; the line cites the proof
  FIXED (…PARTIAL) partly fixed; the line states exactly what remains
  OPEN             still reproduces; the line cites where
  OPEN (…PARTIAL)  partly addressed; the line states what remains
  VERIFIED CORRECT the finding's own "do not re-flag" items, re-checked and
                   still true
Two statuses are deliberately not "fixed": a status is only FIXED when a file
and line, or a boot log, can be pointed at.

WARNING: this file is listed in .gitignore and is not tracked by git. It does
not survive a clone. See finding #162.
================================================================================

--------------------------------------------------------------------------------
0. FIXED SINCE THIS AUDIT WAS TAKEN
--------------------------------------------------------------------------------

The 177 findings below are the state at b1df707. These are the defects that were
*not* in it — found afterwards, mostly by instrumenting a boot that would not
finish — and which no finding in the list records. Each one is here with the
mechanism and, more usefully, with why it was silent. Eleven of the fourteen
produced no diagnostic of any kind, and of the three that did, two reported the
wrong fault. That is the recurring theme and it is worth stating plainly: on this
tree the most expensive bugs are the ones that do not fail, they print something
plausible.

Reading this section is how you avoid re-introducing any of them.

### 0.1  The 8259s were never masked (the "loader is slow / dies at random" bug)

**Mechanism.** BIOS leaves both 8259s identity-mapped with IRQ0 *unmasked* — the
master IMR at 0x21 reads 0xFE, one bit clear. Nothing remaps them until the
kernel, and until this was fixed nothing unmasked anything either. So an 18.2 Hz
timer tick was delivered as **vector 8** for the whole of stage2.

`bios_call()` is a 32-bit -> real-mode -> firmware -> 32-bit round trip. It runs
with `CR0.PE` clear while stage2's own IDT is still the one installed. A
real-mode interrupt with a non-zero IDTR does **not** go through the IVT at
physical zero: it dispatches through that IDT. Gate 0 was `exc_stub`, the
**32-bit** reporter, so a tick landed inside a BIOS call and executed 64-bit
instructions as 16-bit. The decode walked off the end of the reporter into
`exc_halt64`.

**Fix.** Two lines at the top of `stage2_main()`, before anything else
(src/boot/stage2.c:1657-1658):

    outb(0x21, 0xFF);		/* master: all IRQs masked */
    outb(0xA1, 0xFF);		/* slave:  all IRQs masked */

**Why it was silent.** `cli` does not help — it runs *after* the trampoline
returns, and the tick lands *during* the call; the trampoline `pushfw`es flags
with IF set so the firmware's `iret` restores them, and SeaBIOS's own disk
handler executes `sti`. The reporter that should have logged the fault was
itself the thing that crashed, and a crash with no output looks exactly like a
hang. The only visible symptom was *where the log stopped*, which moved around
with sector count and looked like a slow disk. At 18.2 Hz against ~1400 sector
reads the first tick landed during the second `PT_LOAD`, so the truncation point
was where the first tick happened to fall — which is why it read as
nondeterministic rather than as an interrupt bug.

Masking both PICs loses nothing: stage2 is single-threaded, and the kernel
remaps and unmasks them itself in `pic_remap()`. It also means the system now
has **no interrupt source at all** until a PIT driver exists — see finding #60.

### 0.2  `copy_from_disk()` never called `bios_read_bounce()`

**Mechanism.** The copy loop computed a sector count and a chunk size and then
copied out of the bounce window without ever asking the firmware to fill it, so
the "kernel payload" was whatever stale bytes were there.

**Why it was silent.** There was no error path to take: the function had none.
Worse, the stale bytes were a *previous* successful read, so the destination
looked plausible — the first sector read correctly, because it was the sector
that had just been read into the window.

### 0.3  `make` did not rebuild `stage2.c.o` after a header edit

**Mechanism.** The boot-C rule had `-MMD -MP -MF $@.d`, the `.d` file existed and
correctly listed `src/boot/boot_layout.h`, and Makefile:238 `-include`d every
`.d` it could find — and `make` still did not rebuild. `build/stage2.elf` was
produced with `BOUNCE_ADDR` compiled as 0x10000 while the header said 0x20000
and the object file's mtime was *newer* than the header's.

**Why it was silent.** Nothing: the dependency graph looked correct and was not.
The observable damage was enormous and entirely misleading — see 0.4. The
workaround is the one that is now policy for this tree: **always `make clean`
before believing a build result.**

### 0.4  The loader image had grown into its own stack

**Mechanism.** A direct consequence of 0.3. The firmware's bounce buffer was
compiled at 0x10000 while the linker script (which *did* rebuild, being derived
rather than compiled) had moved the 32-bit loader stack to [0xE000, 0x20000). The
bounce buffer therefore landed inside the loader's stack, and **every sector read
overwrote the loader's return addresses**, zeroing `serial_puthex`'s digit table
on the way past.

**Why it was silent.** The symptom was real mode with `CS` base 0x80 and EIP in
`exc_halt64`, which reads as "the CPU went back to real mode on its own" and sent
two separate investigations after mode-switch bugs in the trampoline. Both were
wrong. This is the clearest example in the whole history of a build-system fault
presenting as a memory-management fault.

### 0.5  The higher-half window walked through the wrong PML4/PDPT pair

**Mechanism.** `0xFFFFFFFF80000000` is `2^63 - 2^31`, so the walk is **PML4 511,
PDPT 510**. 511 is a real 1 TiB region (it starts at `0xFFFFFFFFC0000000`) and
256 is the *first* address of the higher canonical half — 512 GiB below the
kernel — so every plausible-looking constant is wrong.

**Why it was silent.** The wrong index is not an out-of-range write. Nothing
faults at the store; every read-back check passes; the kernel's tables simply
describe a region it does not occupy, and the first high-half *access* faults.
That moved the failure from "the CR3 write" to "the next instruction", which is
a completely different place to look. Both halves now derive the indices from
`KERNEL_VIRT_BASE` through `PML4_ENTRY_OF`/`PDPT_ENTRY_OF` (boot.h:86-87) so they
cannot drift apart again.

### 0.6  IDT entries are 16 bytes in long mode

**Mechanism.** A gate descriptor is 16 bytes in 64-bit mode and the upper 8 must
be zero. With 8-byte entries the CPU reads the next gate's low half as the high
half of this one, and **every** delivery is `#GP`.

**Why it was silent.** It is not silent — but the `#GP` is taken through the
loader's own IDT, whose gates are 16 bytes and mostly zero, so the second-order
fault decodes as a plausible-looking entry and the machine resets instead of
reporting. This and 0.7 together are why no exception raised between the
long-mode jump and `idt_init()` was ever reported.

### 0.7  `exc_stub64` was reached as 16-bit code

**Mechanism.** The bootstrap gates pointed at the 32-bit reporter. A fault taken
while the CPU was decoding 16-bit ran 64-bit instructions as 16-bit; the decode
walked into `exc_halt64`.

**Why it was silent.** Same shape as 0.1: the reporter is the thing that dies, so
the failure it was supposed to explain is the failure you get instead. `pushl`
became a 16-bit push, the frame offsets were wrong by a factor of two, and the
first instruction after the entry point was already `cli; hlt`.

### 0.8  `KERNEL_LANDING_ADDR` is not 2 MiB-aligned, so the window cannot be 2 MiB pages

**Mechanism.** The image lands at `0x100000`. A 2 MiB page must be 2 MiB-aligned
by definition, so the first 2 MiB of the kernel window has to be a 4 KiB page
table. It now is: one 4 KiB PT at `BOOT_PTP_OFF` inside the loader's own
64 KiB bootstrap page-table pool at `BOOT_PT_ADDR`.

**Why it was silent.** A 2 MiB page whose base is not 2 MiB-aligned is not a
silent corruption in the usual sense — it is a fault on the first access, after
the window looks correctly mapped in a page-table dump. It is listed here because
the fix is a geometric fact, not a bug, and the next person to "simplify" the
window back to 2 MiB pages will reintroduce it.

### 0.9  `gdtr64` held the *addresses* of the limit and base, not their contents

**Mechanism.** `.word stage2_gdt_limit` looks like it stores a limit. It stores a
label, so the assembler emits a relocation and the linker fills in the label's
own address: LGDT then loaded a *limit that was an address* and a *base that
pointed at the two variables*. `LGDT` cannot take its operand from a register and
the assembler can only emit an address, so the six bytes have to be written by
hand — by C, once the GDT exists (stage2.c:1591-1592).

**Why it was silent.** The base pointed somewhere real (the `.data` holding the
two variables) and the limit was a small plausible number, so LGDT succeeded and
the far jump into 0x08 then resolved against a GDT whose bytes are `0`, `limit`,
`base` — i.e. a descriptor whose fields are assembled out of the loader's own
variables. The first selector load raised `#GP`, through an IDT whose gates
needed a CS to return, so nothing printed.

### 0.10  `12(%ebp)` in `.code64` rebased onto RDI

**Mechanism.** In 64-bit code, a `12(%ebp)` displacement assembles with an
address-size override that GAS encodes using **RDI**, not RBP. The entry code
therefore read a kernel pointer's low half out of a register holding something
else entirely.

**Why it was silent.** It produced a *number*. A plausible-looking wrong number is
the hardest kind of wrong, and here it fed straight into the zero/sign-extension
bug below, so the two compounded into "the kernel jumps to 2 GiB" instead of
"the kernel reads the wrong register".

### 0.11  The kernel entry address needed sign extension

**Mechanism.** `0xffffffff80000180` loaded into a 32-bit destination zero-extends
to `0x0000000080000180`. `movslq` is the fix (stage2_long.S:363-364).

**Why it was silent.** It jumped to a real, mapped, low address — 2 GiB — and
the failure there looked like a page-table problem in a region nobody had
mentioned.

### 0.12  `kmain.S` tested EFER bit 8 instead of bit 10

**Mechanism.** EFER.LMA is bit **10**. The code read EFER, shifted EAX left by 8
and tested bit 8, which is EFER.URE (SVM). LMA is set by the *hardware* the
instant paging is enabled in long mode, so the assertion could never pass.

**Why it was silent.** It was an assertion, and an assertion that can never pass
is either a guaranteed panic or dead code. Here it was dead code in a file whose
surrounding assertions had all been passing, so nothing drew attention to it.
`btl $10, %eax` is the fix (kmain.S:96).

### 0.13  `BOOTINFO_PHYS_ADDR` pointed at the E820 map, not at the bootinfo

**Mechanism.** `BOOTINFO_ADDR` is 0x91000; `E820_ADDR` is 0x90000, exactly one
4 KiB page below. `kmain.S` loaded the wrong constant **after** stage2 had already
put the right address in RDI.

**Why it was silent.** The magic check then validated the E820 map's first eight
bytes against `BOOTINFO_MAGIC` and always failed — a panic, but one that reads as
"the bootloader handed me garbage", which points at the loader rather than at a
hardcoded literal in the kernel's own entry stub. Both constants now live in
boot.h and neither side has its own copy.

### 0.14  AVX in a build whose XCR0 nobody controlled

**Mechanism.** The kernel was compiled `-mavx2 -mfma -mf16c -mxsave`. The only
code that ever wrote XCR0 was the *loader*, at stage2_long.S:210-212, for its own
long-mode entry — and the kernel then inherited that state implicitly, with
nothing on the kernel side asserting it. `vmovdqu` against an XCR0 that does not
have the YMM bits is `#UD`; against one that does, it works by accident.

**Why it was silent.** Depending on the firmware, it was *either* an immediate
`#UD` at the first vectorised instruction or nothing at all. On this machine the
firmware happened to leave XCR0 at 3 and the loader added bit 2, so it was
"working" — which is worse, because the obvious test passed.

The kernel is now explicitly GPR-only (config.mk:65: `-mno-avx -mno-avx2 -mno-fma
-mno-f16c -mno-mmx -mno-80387 -msoft-float`). That is the right posture for a
context switch that saves 512 bytes of x87+XMM0-15 and nothing else — but note
that **USER_CFLAGS still passes `-mavx2 -mfma -mf16c -mxsave`**, and the kernel
still does `fxsave`/`fxrstor` only. Two preempted userspace processes with live
YMM state still lose the upper halves silently. That is finding #63, and it is
open.

--------------------------------------------------------------------------------
A. RUNTIME / BUILD VERIFICATION (empirically established)
--------------------------------------------------------------------------------

7.  [INFO] Clean build of b1df707 succeeds with zero errors and 18 warnings; the
    >> STATUS: OPEN (amended) — the clean build now produces 13 warnings (11 compiler + 2
       ld), not 18, and the composition changed: stage2.c 7 (a20_is_open's
       -Warray-bounds and an unused `count_here`), cpu_features.c 2, console.c 1,
       unistd.c 1 (`noreturn` function does return = #129). The kprintf/kmalloc/pmm
       warnings the audit listed are gone. The remaining ones are still real
       defects.

    warnings are real defects, not noise. Reproduce with
    `make clean && make 2>&1 | grep -E 'warning|error'`. See #14, #37, #40,
    #54, #60, #62, #72, #79, #83, #84, #90, #95, #100, #102.

8.  [INFO] No source file under src/ contains TODO/FIXME/XXX/HACK. Only four
    >> STATUS: VERIFIED CORRECT — still zero matches for TODO/FIXME/XXX/HACK anywhere under
       src/.

    "not implemented" strings, all in userspace. The tree's incompleteness is
    therefore invisible to a grep — see section H.

--------------------------------------------------------------------------------
B. BOOT CHAIN  (src/boot/*, src/include/boot.h, tools/disk.py, tools/initrd.py)
--------------------------------------------------------------------------------

14. [HIGH] src/boot/stage2.c:733-736 / stage2_entry.S:596-619 — the diagnostic
    >> STATUS: OPEN (partial) — `fail()` now prints `diag_site` and a call count
       (stage2.c:192-227), so a stalled loader names the service it was inside;
       `diag_check`/`diag_report` are gone. But `bios_post_sp` and
       `bios_iret_magic` still exist only as prose in stage2_entry.S:602, so "the
       firmware's iret returned to the wrong address" is still indistinguishable
       from "the loader stopped".

    that would have explained finding #1 does not exist. `bios_post_sp` and
    `bios_iret_magic` are described in stage2_entry.S:596-619 and appear nowhere
    in src/. `diag_check()` is `(void)where;`, `diag_report()` (stage2.c:255,
    unused, and the source of a -Wunused-function warning) is never called, and
    `diag_site` is written but never read. There is no way to tell "the loader
    stopped" from "the firmware's iret returned to the wrong address".

15. [HIGH] src/boot/stage2_entry.S:47-62 — `lidt`/`lgdt` are emitted in the
    >> STATUS: OPEN — half fixed. LIDT is now the bare `0f 01 1c`, which really is the
       m16&32 form. LGDT is still `lgdt (%si)` in .code16, which GAS assembles to
       `0f 01 14` = m16&16: a 16-bit limit *and* a 16-bit base (verified: `gcc -m32
       -c` on the one-line reproducer emits exactly `0f 01 14`). The comment at
       stage2_entry.S:59-60 claiming LGDT takes an m16:32 operand in 16-bit mode is
       still wrong.

    m16&16 form, not m16&32: the bytes are `0f 01 1c` / `0f 01 14`, with no
    0x67/0x66 prefix. Confirmed in build/stage2.bin at offsets 0x08 and 0x0d.
    The comment at :48-56 claims "the bare 0f 01 1c is the m16&32 form: 6
    bytes, the full 32-bit base" — it is not; it loads a 16-bit limit AND a
    16-bit base. It works only because both tables sit below 64 KiB (gdt
    0xb060, idt 0xc1d8), and stage2.ld asserts BIOS_VECTOR_ADDR, BIOS_IVT_PTR
    and the trampoline words but *not* idt/gdt. Failure: .rodata growing past
    0xF000 (permitted — the only bound is `ASSERT(__bss_end <= 0x10000)`)
    silently truncates the IDT base and the first exception decodes a gate from
    unrelated memory. Nothing faults and nothing reports.

16. [MEDIUM] src/boot/boot_layout.h:194 + stage2_entry.S:499,549-550 — the
    >> STATUS: OPEN — `BIOS_RM_STACK_TOP` is still 0x7E00 (boot_layout.h:197) and
       `STAGE1_DRIVE_ADDR` is still 0x7DFC (:280), so the trampoline's first
       `pushfw`/`lcallw` still lands on the drive byte. Only 512 bytes of headroom
       exist before it reaches 0x7C00.

    real-mode trampoline stack top is 0x7E00 and it grows *down* into the MBR:
    `pushfw` then `lcallw *BIOS_IVT_PTR` writes 0x7DFE and 0x7DFC, and
    STAGE1_DRIVE_ADDR is 0x7DFC (boot_layout.h:277). The comment at
    boot_layout.h:188-190 ("0x7E00 is above stage1's image ... so nothing the
    firmware writes there can collide") reasons backwards. Harmless today only
    because the drive byte is consumed at stage2_entry.S:106 before the first
    BIOS call. Only 512 bytes of headroom exist before it reaches 0x7C00.

17. [MEDIUM] src/boot/stage2.c:1285-1289 — the ring-3 code descriptor is
    >> STATUS: OPEN — stage2.c:1570 still installs `gdt[3] = 0x0000FA000000FFFFULL`, flags
       byte 0x00, i.e. a 16-bit code segment, and still labels it `/* 0x18 64-bit
       user code, DPL 3 */`. stage2 never enters ring 3 so it is latent, but the
       label is a false claim and gdt.h's "cannot drift apart" is still untrue.

    `gdt[3] = 0x0000FA000000FFFFULL`, described as "64-bit user code, DPL 3".
    Its flags byte is 0x00, i.e. G=0, B=0, L=0: a 16-bit code segment.
    src/include/gdt.h assigns index 3 to `GDT_USER_CODE32_FLAGS` (0xCF, no
    LONG_MODE) and states "Both the stage2 loader and the kernel build their
    GDTs from them, so the privilege levels and long-mode flags cannot drift
    apart" — stage2.c:22 includes gdt.h and then hardcodes all four literals, so
    they have in fact drifted. Loading 0x18/0x1B as CS in long mode raises    #GP(0x1B).

18. [MEDIUM] src/boot/stage1.S:189-196 — the sectors-per-track divisor inherits
    >> STATUS: OPEN — stage1.S:182 sets `movb $0x08, %ah` and :196 still does `movw %ax,
       bx_spt` with nothing clearing AH in between (the `xorb %ah,%ah` at :201 only
       covers `bx_heads`). `divw bx_spt` at :287 therefore divides by `0x0800 |
       spt`. It happens to work because SeaBIOS returns AH=0 on success; that is
       luck, not logic.

    AH from the BIOS status byte. `movb $0x08, %ah` is still live at
    `movw %ax, bx_spt`, so bx_spt = 0x0800 | spt (e.g. 2111, not 63) unless    INT 13h/AH=08h happens to return AH=0. Every LBA->CHS conversion in
    chs_read is then wrong with CF clear — wrong sectors read, silently. The
    heads path gets this right (`xorb %ah,%ah` at :201).

21. [LOW] Dead code and unused constants in the boot chain: stage2.c:63 io_wait()
    >> STATUS: OPEN (partial) — `io_wait()` now has callers (stage2.c:284,305) and
       `diag_report`/`diag_check` no longer exist. `DAP_ADDR` (boot_layout.h:98)
       and `VBE_SCRATCH_BYTES` (:109) are still defined-but-unused, and the
       duplicate stage2.ld ASSERTs were not re-checked.

    (never called), :255 diag_report(), :282 diag_check(), :960 `need` computed
    then `(void)need` at :985, stage2_entry.S:371-377 bios_saved_cr0/ds/es    written and never read, boot_layout.h:95 DAP_ADDR, :106
    VBE_SCRATCH_BYTES, :225 STAGE2_BSS_LIMIT, :281 BOOT_PT_BYTES, plus two
    byte-identical duplicate ASSERTs at stage2.ld:150 and :189.

25. [MEDIUM] src/boot/stage2.c:1349-1361 — `_pad0` and `cmdline[1..127]` are
    >> STATUS: OPEN (amended) — `_pad0` no longer exists (boot.h changed), so that half of
       the finding is stale. The rest stands: stage2.c:1676 sets `bootinfo->version
       = 1` from a literal instead of `BOOTINFO_VERSION`, and only `cmdline[0]` is
       written (:1691) — `cmdline[1..127]` is whatever was at 0x91000, and main.c
       copies the whole struct.

    left uninitialised at 0x91000 and main.c:124 copies the whole struct, so
    uninitialised bootloader memory crosses into the kernel. `bootinfo->version`
    is set from a literal 1 rather than BOOTINFO_VERSION.

--------------------------------------------------------------------------------
C. KERNEL MEMORY MANAGEMENT  (pmm.c, mm.c, vmm.c, kmalloc.c, percpu.c)
--------------------------------------------------------------------------------

26. [CRITICAL] src/kernel/vmm.c:338-365 + mm.c:251-254 —
    >> STATUS: OPEN — vmm.c:399-426 `build_missing_tables()` still creates only a PD and a
       PT, and still does `if (!pdpt) return -1;`. Nothing in the tree can ever
       create a PDPT or PML4 entry for a user address space, so demand paging and
       vmalloc remain non-functional.

    `build_missing_tables()` can only create a PD and a PT; there is no code path
    that ever creates a PDPT or a PML4 entry. `if (!pdpt) return -1;` when
    `walk(pgd, virt, 3, NULL)` is absent. Two consequences:
      (a) User address space: mm_create() installs only dst[256] and dst[511];
          PML4[0..255] are zero. A user address such as 0x40000000 resolves to
          PML4[0] = 0, the walk fails, vmm_handle_page_fault returns -ENOMEM
          (mm.c:756-759). Demand paging is non-functional for *every* user
          address, so execve fails at the first user_memory_write.
      (b) vmalloc: VMALLOC_AREA 0xFFFFC00000000000 is PML4 384 / PDPT 256, and
          PML4[384] is not present in the kernel PML4 either. vmalloc() therefore
          always returns NULL, which kills vma_alloc() (every mm_add_vma),          kstack_alloc() (task.c:97) and every kmalloc() > 4096.

35. [HIGH] src/kernel/vmm.c:49 vs pmm.h:14-15, memory-layout.md:37 — the direct
    >> STATUS: OPEN (doc side fixed) — `DIRECT_MAP_BYTES` is `(4ULL << 30)` (vmm.c:54) and
       `phys_to_virt()` is `PHYS_DIRECT_MAP + phys` (vmm.h:186), so only the low 4
       GiB is backed. docs/src/architecture/memory-layout.md:37 said "up to 64 TB"
       and has been corrected to 4 GiB. src/kernel/include/pmm.h:14-15 still claims
       ZONE_HIGH is "Reachable through the direct map" — that header text is wrong
       and is outside the documentation scope.

    map is 4 GiB (DIRECT_MAP_BYTES) but ZONE_HIGH allocations are permitted as
    a fallback. `phys_to_virt()` is only backed below 4 GiB, so any consumer that
    dereferences a >4 GiB frame faults: GFP_ZERO's memset (pmm.c:396),
    pt_alloc_zeroed (vmm.c:326), slab_new (kmalloc.c:175), clone_table    (process.c:656). pmm.h:14 and memory-layout.md:37 ("up to 64 TB") are both
    false. Latent on a 4 GiB reference machine.

40. [MEDIUM] src/kernel/kmalloc.c:170 vs pmm.h:32,39 — `slab_new()` passes
    >> STATUS: OPEN — pmm.h:32 `GFP_ZERO 8u` and pmm.h:39 `PG_SLAB (1u << 3)` are still
       numerically identical and `slab_new()` still passes `PG_SLAB` where a gfp_t
       is expected.

    `PG_SLAB` where a GFP flag is expected, and the two constants are
    numerically identical: `PG_SLAB = (1u << 3) == GFP_ZERO == 8u`. Every slab
    allocation therefore requests GFP_ZERO and memsets a full page through the
    direct map (pmm.c:385-401) immediately before slab_new overwrites it — a
    pointless full-page store per slab growth, plus a latent type confusion.

41. [MEDIUM] src/kernel/vmm.c:664-677 vs vmm.h:337-343 — `kstack_alloc()`'s
    >> STATUS: OPEN — vmm.c:725-738 still allocates `size + PAGE_SIZE`, maps all of it, and
       returns `base + PAGE_SIZE`. The 'guard' is an ordinary writable page.
       vmm.h:361-363 still claims "the guard is an unmapped hole below each stack,
       and the mapping below the guard is PROT_NONE", which is false. vmm.c's own
       comment is now honest; the header's is not.

    "guard page" is an ordinary mapped, writable page: it just adds PAGE_SIZE to
    the returned pointer. vmm.h:340-343 claims "the guard is an unmapped hole
    below each stack". A kernel-stack overflow scribbles into the neighbouring
    vmalloc block.

42. [MEDIUM] No NX anywhere. vmm.c:193,204-205,216-218,255-257 map the direct
    >> STATUS: OPEN — the only PTE_NX in vmm.c is the derived bit in `vmm_map_page` (:453).
       The direct map, the kernel window and the boot tables are all mapped RWX.
       Boot log: `cpu: no NX support; user/kernel separation is weaker than
       intended`.

    map, the kernel window and the boot tables RWX. EFER.NXE is enabled
    (vmm.c:302-304) and vmm_map_page correctly derives PTE_NX from VM_EXEC
    (vmm.c:392), but none of the kernel's own mappings set it. Every
    phys_to_virt() alias of physical memory — including the frames backing user
    VMAs — is executable.

43. [MEDIUM] src/kernel/vmm.c:338-362,394,408 — page-table updates invalidate
    >> STATUS: OPEN — `invlpg(virt)` at vmm.c:455 still flushes the entry in whichever
       address space CR3 currently names, and `build_missing_tables()` still
       flushes nothing after writing a PDPT/PD entry.

    the wrong TLB. `invlpg` flushes the entry in the address space named by the
    *currently loaded* CR3, not in `pgd`; build_missing_tables writes PDPT/PD
    entries and flushes nothing at all. Benign on the fault path
    (current_mm() guarantees read_cr3() == mm->pgd there); not benign for
    user_memory_write -> vmm_map_page from process.c:448, which runs while the
    CPU is still on a different PML4. Combined with recycled PML4 pages
    (mm.c:355), a later task can inherit a stale TLB entry for a PML4 physical
    address since reallocated to another process.

45. [MEDIUM] src/kernel/process.c:649-670 — `clone_table()` mishandles a 1 GiB
    >> STATUS: OPEN — process.c `clone_table()` still handles `level == 1` (PTE leaf) and
       `level == 2 && PTE_PS` (2 MiB) and sends everything else down the `else`. At
       `level == 3` a PTE_PS entry is a 1 GiB leaf and is walked as a page table.
       The `return 0` paths also still abandon the frames allocated so far.

    leaf. At level == 3 a PTE_PS entry is a 1 GiB page; it falls into the
    `else` and the function walks a *data* address as a page table, producing a
    corrupt child PML4. The level==2 PTE_PS case allocates an order-9 block and
    memcpy's 2 MiB of page *contents* when a single PTE copy would do. Every
    `return 0` after a successful pmm_alloc_page abandons the frames allocated so
    far. Unreachable today (user space is exclusively 4 KiB leaves) but one
    VM_HUGE implementation away from live corruption.

46. [LOW] Per-CPU base handling is only safe because CR4.FSGSBASE is never
    >> STATUS: OPEN — kmain.S:72 and percpu.c:43 still program MSR_GS_BASE to
       `percpu_data`, and CR4.FSGSBASE is still never set. There is still no
       `assert(!(read_cr4() & (1<<16)))` before the first `swapgs`.

    enabled. kmain.S:62-73 and percpu.c:43 both program MSR_GS_BASE to
    percpu_data, making the `swapgs` in syscall_entry.S a no-op (see #79). If
    FSGSBASE is ever enabled, ring 3 can wrgsbase its own GS base and the
    following swapgs installs it as the kernel's, giving a CPL0 arbitrary-GS-base
    primitive. Add `assert(!(read_cr4() & (1<<16)))` before the first swapgs.

47. [LOW] There is no AP bring-up path anywhere in the tree. percpu_setup() is
    >> STATUS: OPEN — still no AP bring-up path; `percpu_setup(0)` is still CPU 0 only, and
       MAX_CPUS/kmags are still sized for a path that does not exist.

    called for CPU 0 only (main.c:147), so run.sh's `-smp 18` leaves 17 vCPUs in
    the firmware's INIT/SIPI wait and this_cpu_id() is only ever evaluated on
    the BSP. MAX_CPUS 256 and kmags[256][10] (1.33 MB of BSS) are sized for a
    bring-up path that does not exist.

48. [LOW] src/kernel/mm.c:333-337 documents a fork leak that does not exist:
    >> STATUS: OPEN (comment is false; source side) — sys_fork still copies only
       PML4[0..255] (process.c:763), so the fork leak mm.c:330-337 describes does
       not exist. That comment is source, not documentation, and is outside this
       scope. No document in docs/ repeats the claim, so there is nothing to
       correct here.

    "a forked child, whose PML4 was deep-copied by process.c including the direct
    map, leaks that copy." sys_fork copies only PML4[0..255] (process.c:761-772).

49. [LOW] Duplicated, mutually inconsistent BUDDY_MAX_ORDER: pmm.c:35 and
    >> STATUS: OPEN (doc fixed) — the duplicate `BUDDY_MAX_ORDER` in pmm.c is gone;
       pmm.h:94 is now the single definition, value 11.
       docs/src/memory/buddy-allocator.md said 11 levels / orders 0-10 / max 4 MB /
       `free_list[11]`; the code has 12 levels, orders 0-11, max 8 MiB and
       `free_list[12]`. The document has been corrected.

    pmm.h:89 both `#define BUDDY_MAX_ORDER 11` (legal, identical), while
    docs/src/memory/buddy-allocator.md:9,25 specifies 11 levels / orders 0-10 /
    max 4 MB and the code has 12 levels to order 11 (8 MiB). One of the three
    is authoritative and nothing enforces it.

51. [INFO] Dead code in the memory path, confirmed unreachable:
    >> STATUS: OPEN (partial) — `page_array_pages`, `zone_bitmap`, `reserved` and
       `arena_start` are gone from pmm.c entirely. `pt_free`, `vmm_map_large`,
       `vmalloc_aligned`, `kstring_init` and `pml4_idx_at` are still defined with
       no callers.

    pmm.c:43,770 page_array_pages (written, never read); pmm.c:643,798 `reserved`
    and pmm.c:645,798 `arena_start` (set, never used — compiler-flagged, and the
    source of finding #29); pmm.c:109,162-179,764-780 the entire `zone_bitmap[]`
    (written by zone_set, read only by zone_test which has no callers — it costs3x bitmap_bytes, ~384 KiB on a 4 GiB machine, for nothing; also    -Wunused-function); pmm.c:374 `if (!zone->free_list[0].next) continue;` is
    both an unlocked read and a no-op, since list_init sets next = head so it is
    never NULL — the correct test is free_bitmap; vmm.c:151-155
    `pml4_idx_at[5] = {0,0,0,0,0}` plus `(void)pml4_idx_at`; vmm.c:331 pt_free,
    vmm.c:424 vmm_map_large, vmm.c:577 vmalloc_aligned — no callers;
    kstring.c:587 kstring_init() — no callers.

52. [INFO—VERIFIED CORRECT, do not re-flag]
    >> STATUS: VERIFIED CORRECT — no item in this block is contradicted by the current
       tree. Note that buddy-allocator.md:63-65 is still the doc the block says is
       wrong (the ctz without the zero-mask guard).

    - Buddy free_pages accounting is exact (no double decrement).
    - `first_order_at_least` (pmm.c:249-251) tests mask == 0 before
      __builtin_ctz, so the TZCNT concern in its comment does not arise.
      (buddy-allocator.md:63-65 shows the ctz *without* the guard and would be UB
      as written — the code is right, the doc is wrong.)
    - present_words = total/64 + 1 correctly covers the trailing partial word,
      and the word-count design genuinely prevents OOB bitmap dereferences.
    - E820 interpretation fails closed: only E820_USABLE clears
      frame_reserved, the default memset is 0xFF, and frame_reserved_p fails
      closed for out-of-range indices. ACPI-reclaimable, ACPI-NVS, E820_BAD and
      gaps are all reserved by construction.
    - walk()'s level semantics, the PTE_PS early-out, PT_ENTRY_OF, and
      vmm_map_page's flag derivation (VM_WRITE/VM_USER/!VM_EXEC -> NX) are right.
    - boot_pt_pool is __attribute__((aligned(PAGE_SIZE))) so CR3 is 4 KiB-aligned,
      and PAE + EFER.NXE are set before the CR3 write.
    - The direct map's own geometry is correct (PML4[256] is the right index for
      0xFFFF800000000000; both the 1 GiB and 2 MiB encodings are right).
    - vma_punch's split logic (at most one straddler, tail allocated before
      mutation, -ENOMEM leaves the list untouched) is correct.
    - Lock ordering is consistent: mm->lock -> vmalloc_lock, and
      c->lock (kmalloc) -> zone lock (pmm); no path takes them in reverse, and
      vma_alloc is correctly hoisted out of mm->lock.
    - spinlock_acquire/release/irqsave and the rwlock readers-word      implementation are correct, and irq_save genuinely does pushfq/popq/cli,
      so a lock taken with interrupts disabled is safe against handler re-entry.

--------------------------------------------------------------------------------
D. PROCESS / SCHEDULING / SYSCALL / ELF
--------------------------------------------------------------------------------

61. [CRITICAL] src/kernel/kmain.S:62-73 + percpu.c:43 — ring 3 has the kernel's
    >> STATUS: OPEN — percpu.c:43 still programs MSR_GS_BASE to `percpu_data`, and
       CR4.SMEP/CR4.SMAP (cpu_features.h:71-72) are still never written. Live
       confirmation from this session's boot log: `percpu: wrote=ffffffff80191300
       msr=ffffffff80191300 gsread=0` followed by `percpu: gs base verification
       failed`.

    per-CPU pointer in GS. Both MSR_GS_BASE (0xC0000101) and
    MSR_KERNEL_GS_BASE (0xC0000102) are programmed to `percpu_data`, which makes
    the syscall-path swapgs a no-op. In long mode a GS-relative access resolves
    to MSR_GS_BASE *regardless of CPL*, and neither CR4.SMEP nor CR4.SMAP is ever
    enabled (both are defined in cpu_features.h:71-72, neither is written to CR4).
    Failure: any process executes `movq %gs:0x0,%rax; movq 0x10(%rax),%rbx` to
    read percpu_data[0].current (offset 16, a `struct task *`) and then writes
    its own value there. That is a direct ring-3 -> ring-0 kernel-pointer-write
    primitive. privilege-levels.md:138-140 claims SMAP and SMEP are enabled;
    neither is.

62. [CRITICAL] src/kernel/process.c:727-740 + interrupt_entry.S:260-277 —
    >> STATUS: OPEN — interrupt_entry.S `ret_to_user()` still pushes only
       SS/CS/RFLAGS/RIP/RSP and issues `iretq`; no register is zeroed, so the child
       of fork() still enters ring 3 with whatever the kernel's call chain left in
       RAX.

    `ret_to_user()` never sets RAX. fork_child_start's comment says "RAX = 0 is
    the entire contract of fork's return value in the child", but ret_to_user
    only pushes SS/CS/RFLAGS/RIP/RSP and iretqs; no register is ever loaded with
    0. The child enters ring 3 through task_trampoline -> fn(arg), so RAX holds
    arbitrary kernel residue. Failure: `pid_t pid = fork();` in the child branch
    takes the "parent" path, and any `if (fork() == 0)` idiom diverges
    arbitrarily. Linux zeroes the caller-saved registers on first entry.

63. [CRITICAL] src/config.mk:81 vs src/kernel/context.S:96-104 — USER_CFLAGS
    >> STATUS: OPEN — src/config.mk:81 still passes `-mavx2 -mfma -mbmi -mbmi2 -mf16c
       -mxsave` to USER_CFLAGS, and context.S still does `fxsave`/`fxrstor` only
       (512 bytes, XMM0-15). YMM upper halves still survive a preemption un-saved.
       The build log shows the libc objects still compile with those flags.

    enables `-mavx2 -mfma -mf16c -mxsave`, but the context switch only does
    `fxsave`/`fxrstor` (512 bytes: x87 + XMM0-15). fxsave does not save bits
    128-255 of YMM0-15. build/libc.a contains 24 vmovdqu/vmovdqa/vpbroadcast
    instructions from GCC auto-vectorisation. Failure: two preempted AVX2
    processes have their YMM upper halves silently clobbered across the switch —
    vector spills and computation results corrupted with no fault. Note
    config.mk:26-29's justification ("This kernel never programs XCR0") is
    *stale*: stage2_long.S:200-205 does `xgetbv; orl $0x7; xsetbv`, so VEX does
    execute — which is why this is a context-switch bug rather than a #UD bug,
    and also why the libc agent's/the kernel's own AVX reasoning diverges.
    task.h:262 calls fpu_state a "64-byte aligned FXSAVE area" and task.h:211
    sets TASK_FPU_STATE_SIZE 4160u, which is not a power of two despite the
    comment saying it is. Fix: drop the AVX2 flags from USER_CFLAGS (smallest,
    matches the kernel's GPR-only posture), or move to xsave/xrstor with an    XSAVE header and per-task XCR0.

64. [CRITICAL] src/kernel/syscall.c:968-969 — IF is never re-enabled on the
    >> STATUS: OPEN — syscall.c:968 still sets SFMASK with the comment "the handler runs
       with interrupts off and re-enables them deliberately", and there is still no
       `irq_restore`/`sti` anywhere on the syscall path. IF is 0 for the duration
       of every syscall.

    syscall path. SFMASK sets bits 9,10,16,8 with the comment "the handler runs
    with interrupts off and re-enables them deliberately"; it does not. There is
    no irq_restore/sti anywhere in syscall.c or syscall_entry.S, and every
    spinlock_irqsave/unlock_irqrestore pair inside the handlers saves and
    restores IF=0. **IF is 0 for the entire duration of every syscall**, so
    interrupts are deferred until the process is back in ring 3. The observable
    symptom is in tty.c:240-272: `if (!irqs_enabled()) return 0;`, so a blocking
    read(0,...) on an empty ring returns 0 (EOF) immediately. libc's getchar()
    and fgets() see end-of-file on every call.

65. [HIGH] src/kernel/sched.c:744-775 — sched_stop_current() leaves CR3,
    >> STATUS: OPEN — sched.c `sched_stop_current()` still does not call
       `syscall_set_kernel_stack()`, `tss_set_kernel_stack()` or `write_cr3()`, so
       CR3, TSS.RSP0 and `syscall_cpus[cpu].kstack_top` are still left pointing at
       the dead process's freed stack and page-table root.

    TSS.RSP0 and syscall_cpus[cpu].kstack_top pointing at the dead process. It
    sets rq->current/rq->idle/per_cpu(current) and calls context_restore, but
    unlike context_switch_to (sched.c:657-678) it does not call
    syscall_set_kernel_stack(), tss_set_kernel_stack() or write_cr3().
    task_exit_current calls it *after* task_release_resources (task.c:396),
    which frees the kernel stack (task.c:332-336) and, via mm_put, the page-table
    root. Failure: after any process exits the CPU runs on a freed PML4 and the
    next ring-3 interrupt writes its frame into freed memory — a ring-3-triggered
    kernel heap write primitive.

66. [HIGH] src/kernel/task.c:389-413 — task_exit_current() can context-switch out
    >> STATUS: OPEN — task.c `task_exit_current()` still calls `sched_wake(t->parent)`
       before `sched_stop_current()`, and `sched_wake()` can reach `schedule()`.

    mid-teardown, so sched_stop_current() never runs. `sched_wake(t->parent)` at
    :400 can call schedule() (sched.c:603-610), and with t->detached = true
    schedule() does not requeue the exiting task (sched.c:729) — it is left on no
    queue at all, so control never returns to sched_stop_current(). Its
    context_rsp dangles into the freed kernel stack.

67. [HIGH] src/kernel/interrupt_entry.S:85-94 — the interrupt stub does not save
    >> STATUS: OPEN — interrupt_entry.S:86-93 still pushes
       %rdi,%rsi,%rdx,%rcx,%r8,%r9,%r10,%r11 and nothing else. RAX is still not
       saved and `struct interrupt_frame` still has no rax field.

    RAX. The file comment justifies excluding R12-R15/RBX/RBP as callee-saved
    (correct), but RAX is not callee-saved and an interrupt is not a function
    call. Every handler reached from interrupt_dispatch is C, so any value the
    interrupted ring-0 code holds live in RAX is destroyed — and every tick    lands in ring 0. Linux's SAVE_REGS pushes nine registers including RAX.
    struct interrupt_frame has no rax field either, so report_exception
    (idt.c:380-385) cannot print it. interrupt.h:94-97 repeats the incomplete
    rationale.

68. [HIGH] src/kernel/interrupt_entry.S:181-187 — ring-0 interrupts enter
    >> STATUS: OPEN — the ring-0 frame is still 112 bytes against a `FRAME_SIZE` of 120, so
       `report_exception`'s `f->rsp` is still 8 bytes off and the callee still
       enters with RSP 0 mod 16 instead of 8.

    interrupt_dispatch with RSP 8 bytes off alignment. Ring 3: the CPU aligns
    RSP to 16, then pushes 40 + 16 + 64 = 120 bytes, so RSP is 0 mod 16 at the
    `call` — correct. Ring 0: no forced alignment, 32 + 16 + 64 = 112 bytes, so
    RSP is 8 mod 16 at the call, and the callee enters with RSP 0 mod 16,
    violating the SysV rule. Harmless only because the kernel is built
    `-mno-sse -mno-sse2`; it silently breaks the moment SSE is enabled.
    Related: FRAME_SIZE is asserted as 120 (interrupt.h:139), which only holds
    for the ring-3 frame — a ring-0 frame is 112 bytes, so
    report_exception's `f->rsp` reads 8 bytes above the real stack.

69. [HIGH] src/kernel/syscall.c:655-663 — brk() is not confined to the heap and
    >> STATUS: OPEN — syscall.c brk() still starts at `max_vaddr` and still removes VMAs
       below the requested break, and the shrink path still does not re-zero.

    shrink does not unmap. mm->brk starts at max_vaddr (the top of the ELF image,
    elf.c:352-353), so `brk(0x1000)` calls mm_remove_vma over the process's own
    text and data VMAs and returns success. Failure: the next instruction
    executes in a VMA-less page -> #PF -> no VMA -> panic() (see #72). A single
    brk() from ring 3 halts the machine. Second bug: brk down-then-up restores
    the VMA but not zeroed memory, because the fault path finds the page already
    present (mm.c:735-739) and returns 0 without re-zeroing, so the process
    reads back stale bytes where POSIX requires zeros.

70. [HIGH] src/kernel/sched.c:989-994 — sched_sleep_ns() has a real lost-wakeup
    >> STATUS: OPEN — `t->wake_tick` is still written after the node is linked and the lock
       dropped.

    window. `t->wake_tick` — the sort key the expiry walk compares against
    (sched.c:853) — is written AFTER the node is linked and AFTER the lock is
    dropped. A tick landing in that window removes t from the sleep list, then
    sched_wake() early-returns because t->state is still TASK_RUNNING
    (sched.c:594), and the task then executes sched_block_current() and is never
    woken. On the first sleep wake_tick is 0 and thereafter it is a stale past
    tick, so `cand->wake_tick <= now` is always true in the window: the race is
    live for *every* nanosleep. Also, `t` is read before sleep_lock is taken and
    reused without re-list_init, so a second sleep by an already-sleeping task
    corrupts the list.

71. [HIGH] src/kernel/sched.c:398-406 — global_push() has no
    >> STATUS: OPEN — `global_push()` still has no `if (t->in_global) return;` guard.

    `if (t->in_global) return;` guard. sched_migrate gates on `!t->on_rq`, but a
    task on the migration list has on_rq == false, so a second sched_migrate()
    (or one racing sched_set_affinity's global_push at sched.c:493) re-links an
    already-linked node and produces a self-referential loop in global_queue.
    global_pop then hands the same task out twice, rq_enqueue_locked links
    t->rq_node into two MLFQ levels simultaneously, and sched_first_entry on a
    self-looped list never terminates global_pop's emptiness test.

72. [HIGH] src/kernel/idt.c:429-443,465-471 — any unrecoverable ring-3 exception
    >> STATUS: OPEN — `process_deliver_signal` and `process_terminate_from_fault` are still
       `__attribute__((weak))` with no definition anywhere, so a single `ud2` in
       ring 3 still panics the machine.

    panics the kernel. `process_deliver_signal` and `process_terminate_from_fault`
    are declared __attribute__((weak)) in interrupt.h:214,225 and have **no
    definition anywhere in the tree**, so both resolve to NULL. A single `ud2` in
    ring 3 (#UD), a #GP from a bad segment load, or an unrecoverable #PF reaches
    panic("unhandled exception"). This is the mechanism by which #63, #64 and #69
    become machine-wide halts, and it is trivially reachable from any user
    program. It also contradicts architecture/security-model.md:5-9, which lists
    malicious userspace code as in-scope.

73. [HIGH] src/kernel/sched.c:1048-1070 — sched_set_policy() dequeues before
    >> STATUS: OPEN — `sched_set_policy()` still dequeues before the -EBUSY admission test.

    rejecting, losing the task. The -EBUSY admission-control path at :1065
    returns after rq_dequeue_locked has set t->on_rq = false; if t was a queued
    (not running) task nothing ever puts it back, so the task is silently lost
    and a parent blocked in wait4() hangs forever. Related:
    `home->deadline_util += util` is never decremented by any exit or policy
    change, so admission degrades permanently. Latent (zero callers today).

74. [HIGH] src/kernel/sched.c:237-246 — EDF insertion places the new task *after*
    >> STATUS: OPEN — EDF insertion still stops at the first node with `o->abs_deadline <=
       t->abs_deadline`.

    an earlier-deadline task. The walk stops at the first node with
    `o->abs_deadline <= t->abs_deadline` and then inserts before it. List
    [A(10), B(20)], insert t(15) -> stops at A -> [t(15), A(10), B(20)], and
    rq_pick_locked pops the head, so t runs ahead of A. Fix:
    `if (o->abs_deadline >= t->abs_deadline) break;`

75. [HIGH] src/kernel/elf.c:230-277 — the ELF loader does not bound-check
    >> STATUS: OPEN — elf.c still has no MM_USER_LIMIT check, no overflow check on
       `seg_vaddr + p_memsz`, and no p_align congruence check; `vmm_map_page()`
       (vmm.c:428-457) still has no user-half bound, so a userspace mapping can
       still reach the shared kernel PDPT.

    segments against the user half before writing. The only "does it fit" checks
    (elf.c:206-209) are no-ops with load_bias == 0; seg_vaddr + p_memsz is not
    overflow-checked and p_offset/p_vaddr congruence against p_align is not
    checked. `seg_end > covered_end` is unsigned, so a wrapped seg_end that lands
    below covered_end skips mm_add_vma entirely — including the only check that
    keeps this in bounds (mm.c:495-496). Execution then reaches
    user_map_zero_page -> vmm_map_page, and vmm_map_page has **no**
    MM_USER_LIMIT check (vmm.c:345-380): build_missing_tables will allocate a
    fresh PD/PT and overwrite the entry in the *shared* kernel PDPT
    (mm->pgd[511] is kernel_pgd[511], copied by mm_create at mm.c:255),
    corrupting the kernel's own mapping from userspace. Latent today (execve
    only ever loads the kernel-embedded blob) but live the moment a filesystem
    exists.

76. [HIGH] No W^X enforcement anywhere. elf.c:144-157 maps PF_W|PF_X straight
    >> STATUS: OPEN — no W^X enforcement anywhere; `vmm_map_page` only sets NX when VM_EXEC
       is absent.

    through; sc_mmap (syscall.c:711-716) and sc_mprotect (:776-781) let a
    process request PROT_WRITE|PROT_EXEC on the same range with no rejection;
    vmm_map_page only sets PTE_NX when VM_EXEC is *absent*, so a W+X mapping has
    NX clear and is genuinely executable and writable.
    architecture/security-model.md:56-60 claims "All non-text pages are marked
    non-executable via page table NX bits" — no NX bit is set on any of the
    kernel's own mappings (see #42).

77. [MEDIUM] src/kernel/vmm.c:543-565 — vmalloc_aligned()'s split can corrupt
    >> STATUS: OPEN — vmm.c `vmalloc_aligned()` still has the two failure-ordering paths,
       and `block_insert` still fails only at `VMALLOC_MAX_BLOCKS` 2048 (vmm.c:513)
       with no coalescing on free.

    the block array on block_insert failure. (a) The tail insert at virt+size
    can succeed and the final used-block insert then fail, leaving a used block
    with no owner and a truncated b->end — a leak plus a double free on vfree;
    (b) the goto out_fail after the tail insert leaks the head block b with
    b->start still pointing at the head, so the next vmalloc hands out the same
    range twice. block_insert only fails at VMALLOC_MAX_BLOCKS (2048,
    vmm.c:452), and vmalloc never coalesces on vfree (vmm.c:634-660) — so once
    2048 fragmented blocks exist, every subsequent vmalloc, including vma_alloc
    and therefore every mmap, fails permanently with no recovery.

78. [MEDIUM] src/kernel/include/cpu_features.h:31-33 vs cpu_features.c:171 —
    >> STATUS: OPEN — cpu_features.c:171 still reads NX from `basic_edx` bit 20 and never
       consults `extended_edx` (:87). Boot log: `nx=0` and `cpu: no NX support`.
       The leaf 0x80000007 invariant_tsc source and the family/model extended-field
       decode are unchanged too.

    NX is read from CPUID leaf 1 EDX bit 20, which is *reserved*. NX/XD is
    CPUID 0x80000001 EDX bit 20, read into extended_edx at cpu_features.c:87 but
    never consulted. So `cpu_has(CPU_FEATURE_NX)` is permanently false and
    cpu_features.c:222-223 always logs "no NX support" — a permanent false
    alarm that will silently disable NX if anything ever gates on it. Two more
    decode errors in the same file: invariant_tsc is taken from leaf 0x80000001
    EDX bit 8 (the correct source is leaf 0x80000007 EDX bit 8), and family/model
    drop the 8-bit extended-family and 4-bit extended-model fields
    (cpu_features.c:75-76). cpu_features.h:109 `has_1gb_pages` is never assigned    at all (leaf 0x80000001 is never queried), so vmm.c:202-208's 1 GiB branch is
    dead and main.c:272 always prints 1g_pages=0.

79. [MEDIUM] src/kernel/sched.c:587-600 — sched_wake() mutates the target task
    >> STATUS: OPEN — `sched_wake()` still mutates mlfq_level/io_boost/exec_budget before
       taking rq->lock.

    (mlfq_level, io_boost, exec_budget) *before* taking rq->lock, while
    sched_aging_pass (sched.c:814-841) writes the same three fields under it.
    Safe by accident on UP with interrupts disabled; not by construction.
    sched_wake is also called from pipe_release (syscall.c:569,578),
    sys_exit_group (process.c:522) and task_exit_current (task.c:400) — the
    latter two while task_all_lock is held.

80. [MEDIUM] src/kernel/idt.c:566,581-589 maintains per_cpu(preempt_count) but
    >> STATUS: OPEN — per_cpu(preempt_count) is still maintained and never read before
       switching.

    nothing reads it before switching. sched_add (sched.c:511), sched_wake
    (:605) and sched_tick (:956) all call schedule() unconditionally, and
    per_cpu(in_scheduler) is set once (sched.c:1121) and never read. So a wakeup
    raised from inside an interrupt handler — the keyboard path, tty.c:635 ->
    keyboard_irq -> ... -> sched_wake — will schedule() out of interrupt context
    on the current task's kernel stack once #64 and #57 are fixed.

81. [MEDIUM] src/kernel/sched.c:917-948 — dead branch: inside    `else if (t->policy != SCHED_IDLE)`, the nested    `else if (t->policy == SCHED_IDLE)` can never be taken. The idle task never
    >> STATUS: OPEN — the dead `else if (t->policy == SCHED_IDLE)` branch and the
       `rq_has_higher_locked(rq, rq->current)` preemption test are unchanged.

    takes the tick's need_switch path. Not a hang (the idle_thread poll loop at
    sched.c:143-165 does the work), but the intended code is unreachable. Also
    at sched.c:511-512 and :604-606 the preemption test is
    `rq_has_higher_locked(rq, rq->current)` — "is *anything* on this rq above the
    current task" — which ignores whether the just-woken task t is the thing    that outranks, so waking a level-0 task next to another level-0 task
    preempts needlessly.

82. [MEDIUM] Affinity is never enforced at any of the three places the docs claim
    >> STATUS: OPEN — affinity is still unenforced at all three points; there is still no
       balancer, no work stealing and no writer of VECTOR_IPI_RESCHEDULE.

    (sched.c:428-452,483-496,569-611). sched_wake (:594-600) always enqueues on
    this_cpu_id()'s rq and never consults t->cpumask, contradicting
    cpu-affinity.md:37-41 ("Wakeup: ... placed on a CPU in its affinity mask");
    sched_add (:500-517) likewise, contradicting "Initial placement";
    sched_set_affinity only moves a task when t->on_rq, and a *running* task has
    on_rq == false, so pinning yourself to CPU 0 and yielding leaves you
    requeued on the old CPU forever. There is no load balancer and no work
    stealing, and nothing writes VECTOR_IPI_RESCHEDULE (idt_init points 240-255
    at unhandled_vector).

83. [MEDIUM] src/kernel/process.c:1153-1157 — sys_execve() uses non-reentrant
    >> STATUS: OPEN — sys_execve() still uses static staging buffers.

    static staging buffers (2 x MAX_EXEC_ARGS x MAX_ARG_LEN). Two CLONE_VM
    threads calling execve — which sys_exit_group explicitly anticipates at
    process.c:500-506 — will interleave and cross-contaminate argv/envp.

84. [MEDIUM] src/kernel/process.c:408-426 — copy_string_from_user() checks only
    >> STATUS: OPEN — process.c `copy_string_from_user()` still checks only `src >=
       USER_ADDRESS_MAX` and then walks up to `max - 1` bytes, so a `src` within
       254 bytes of the boundary still reaches kernel-range addresses through
       `user_page`. The bound and the loop still disagree.

    `if (src >= USER_ADDRESS_MAX) return -EFAULT;` and then walks up to 255    bytes, so for an `src` within 255 bytes of the boundary it dereferences up
    to 254 bytes of kernel-range addresses through user_page. Not exploitable
    (mm.c:695-696 rejects them), but the check and the loop disagree; the bound
    should be USER_ADDRESS_MAX - max.

85. [MEDIUM] src/kernel/syscall.c:968 + process.c:509-524 — sc_exit_group()
    >> STATUS: OPEN — sc_exit_group() still holds task_all_lock across the
       `sched_wake(t->parent)` loop.

    holds task_all_lock across a loop that calls sched_wake(t->parent), which
    takes rq->lock and can schedule(). Holding the global task-list lock across
    a potential context switch means the resumed task's first action on that
    path can block on task_all_lock. No inverse ordering exists today so it is
    not a deadlock, but it violates in spirit the shared-lock rationale
    documented at task.h:323-328.

86. [MEDIUM] src/kernel/syscall.c:465-490 — read(fd, buf, 0) on a pipe returns 0
    >> STATUS: OPEN — read(fd, buf, 0) on a pipe still takes and releases
       `p->write_waiter`.

    *and* takes and releases p->write_waiter, so a zero-length read spuriously
    wakes a writer. POSIX says it must have no side effects.

87. [LOW] interrupt_entry.S never clears DF on the interrupt path (only
    >> STATUS: OPEN — interrupt_entry.S still never clears DF on the interrupt path.

    ret_to_user:261 and SFMASK do). Harmless today because the kernel builds
    -mno-sse/-mno-sse2 and memcpy_scalar (kstring.c:293-310) is a plain u64-copy
    loop, with the only rep movsl in the kernel being the bootinfo copy in
    main.c before interrupts are enabled. Add `cld` to interrupt_common.

88. [LOW] interrupt_entry.S:270-271 — `addq $8,%rsp; popq %rbp` is a complete
    >> STATUS: OPEN — interrupt_entry.S:270-271 still has the `addq $8,%rsp; popq %rbp`
       no-op and the comment claiming it is what aligns the user stack.

    no-op (push then pop of an unrelated value), and the comment claiming it is
    what makes the user land 16-aligned is false. The real guarantee is
    ALIGN_DOWN(...,16) in build_user_stack (process.c:905). The end state is
    correct; the two instructions and the comment should go.

89. [LOW] interrupt_entry.S:288-303 defines .set FRAME_RDI/FRAME_SIZE but nothing
    >> STATUS: OPEN — FRAME_RDI/FRAME_SIZE are still defined in the assembly and hardcoded
       independently in interrupt.h.

    in the assembly references them, and interrupt.h:128-141 hardcodes the same
    numbers independently. The comment at :288 claims    "_Static_asserts them, so the two cannot drift" — the protection is
    one-directional and only catches edits to the C struct.

90. [LOW] syscall.c:877-890 — syscall_dispatch() sets
    >> STATUS: OPEN — syscall_dispatch() still sets `ret_to_user = 1` after the early
       returns.

    syscall_cpus[...].ret_to_user = 1 *after* the -ENOSYS / NULL early returns.
    Benign today (the flag is already 1 from syscall_init or the previous
    dispatch) but the invariant depends on history.

91. [INFO—VERIFIED CORRECT, do not re-flag]
    >> STATUS: VERIFIED CORRECT — nothing in this block is contradicted by the current
       tree.

    - context_switch() (context.S:41-68): push order is the exact reverse of the
      pop order, the callee-saved set is right, and
      offsetof(struct context, rip) == 48 is asserted.
    - build_initial_frame()'s 64-byte pad (task.c:55-62) is correct and the
      comment's reasoning is exact.
    - IDT gate encoding and DPLs (idt.c:176 ist_dpl packing, 0x8E everywhere,
      DPL 3 only on #BP), TSS layout (gdt.c:153-156, 80 bytes, rsp0 at 0x04,
      iomap_base = sizeof(struct tss) denying all ring-3 port I/O), and every
      TSS field offset being _Static_asserted.
    - syscall_dispatch()'s table bound check (syscall.c:877-878): rax >=
      SYSCALL_MAX before indexing, NULL slots return -ENOSYS.
    - Argument marshalling: arg0..arg5 -> rdi/rsi/rdx/r10/r8/r9 (syscall.h:124-129)
      matches libc/src/syscall.S, and the twelve pushes in syscall_entry.S do lay
      out struct syscall_regs at offsets 0/8/16/.../88 exactly as declared.
    - wait4's scan is correctly restricted to me->children under task_all_lock
      (process.c:559-575), so a process cannot reap another process's child.
    - pipe_release's drop/re-acquire discipline (syscall.c:564-579) frees exactly
      once.
    - vma_allows() gates on the *VMA's* protection and maps the new page with
      `vma->prot | VM_USER`, so demand paging cannot escalate PROT_READ to
      writable; user_page() passes real SDM error-code bits.
    - ELF .bss handling is right: pages past p_filesz are never mapped and the
      fault path allocates a memset page, copylen is clamped to the page-rounded
      segment, and e_entry is bounds-checked (elf.c:282-283).

--------------------------------------------------------------------------------
E. KERNEL CORE — CONSOLE / LOG / PANIC / KPRINTF / INIT ORDER / ENTRY
--------------------------------------------------------------------------------

92. [HIGH] src/kernel/klog.c:111-115 vs src/kernel/main.c:138,148 — klog_init()
    >> STATUS: OPEN — main.c:133 `klog_init()` still runs 10 lines before
       `cpu_features_init()` at :143, and klog.c:114 still copies
       `cpu_features.tsc_khz`, which is 0 at that point. Live confirmation:
       **every** klog line in /tmp/docs.log reads `0.000`.

    is called 10 lines *before* cpu_features_init(). It does
    `tsc_khz = cpu_features.tsc_khz`, which is still 0. The emit guard at
    klog.c:63 is `if (klog_initialised && tsc_khz)`, so it never fires and
    **every klog line for the life of the kernel prints timestamp 0.000**.
    cpu_features.c:198-215 emits six such lines. Reorder, or set tsc_khz in
    klog_init from cpu_features_init's result.

93. [HIGH] src/kernel/console.c:497-500 — the CSI 'K' (erase line) case writes to
    >> STATUS: OPEN — console.c's CSI 'K' case still writes straight to `vga_putc_at()`
       without testing `vga_enabled`, unlike the 'J' case. (console.c was mid-edit
       by another agent at the time of this check and did not compile.)

    the VGA text buffer unconditionally, without checking vga_enabled, unlike
    'J' at :489-495 which checks both backends. When a framebuffer backend is
    active, 0xB8000 has been repurposed by the graphics mode, so an erase-line
    escape corrupts the framebuffer.

94. [HIGH] src/kernel/console.c:561-568 — the escape parser is defeated for
    >> STATUS: OPEN — being fixed at the time of this check. `handle_escape()` now returns
       `bool` and every branch returns true, but the caller had not yet been
       updated and console.c:569 failed to compile (`control reaches end of
       non-void function`).

    every sequence. `handle_escape(*s)` is called to advance the state machine    and then `console_putc_attr(*s, ...)` prints the *same byte* anyway. Only
    ESC itself is skipped by the `continue`. So `ESC [ H` emits the literal
    characters '[' and 'H' to both VGA and serial, in addition to performing the
    cursor move. Fix: have handle_escape report whether it consumed the byte.

95. [HIGH] src/kernel/panic.c:74-78 — the "range check" before writing to the
    >> STATUS: OPEN — panic.c:76-77 still reads `if ((uintptr_t)0xB8000 < 0x100000)`, a
       compile-time-true comparison, under a comment claiming the address is
       range-checked.

    VGA buffer is a tautology. `if ((uintptr_t)0xB8000 < 0x100000)` is a
    compile-time constant comparison that is always true and therefore validates
    nothing. The comment above it claims "the address is range-checked rather
    than assumed". Failure: on a BIOS that *did* report a linear framebuffer,
    stage2 has set a graphics mode and 0xB8000 is either unmapped (a #PF inside
    the panic handler -> double fault -> triple fault -> reset, destroying the
    entire diagnostic) or repurposed (corrupts the framebuffer). The panic path
    is the one place that must never fault.

99. [MEDIUM] src/kernel/console.c:404-414 — if both backends are disabled
    >> STATUS: OPEN (partial) — the -Wint-in-bool-context warning is gone: console.c:451
       hoists `rows = vga_enabled ? VGA_ROWS : fb.rows` out of the comparison. The
       underflow is not: with both backends disabled `rows` is 0, so `cursor_y >=
       rows` is always true and `cursor_y = limit - 1` still wraps to 0xFFFFFFFF
       (:460-463).

    (vga_enabled false and fb.rows == 0), `cursor_y >= fb.rows` is    `cursor_y >= 0` — always true — so `limit = 0` and `cursor_y = limit - 1`
    underflows to 0xFFFFFFFF. The code then writes to VGA/FB positions derived
    from that. Also note the `?:` binds inside the `>=`, which is the intent but
    draws a compiler warning (-Wint-in-bool-context, console.c:404) that flags
    exactly the precedence trap a reader will get wrong.

100. [MEDIUM] src/kernel/console.c:530-536 — '\b' decrements the cursor but erases
    >> STATUS: OPEN (partial) — `\b` is now written to serial (console.c:613-614). Nothing
       is still blanked on screen, and the file header at console.c:7 still claims
       "a backspace erases on screen and in the serial stream".

    nothing on screen and is never written to serial. The file header claims "a
    backspace erases on screen and in the serial stream". Neither happens.

101. [MEDIUM] src/kernel/console.c:255-262 — the framebuffer writer assumes
    >> STATUS: OPEN — console.c's framebuffer writer still branches on 24 or 32 bpp and
       writes three bytes per pixel for anything else; bpp is still only checked
       for 0.

    bpp is 24 or 32. Any other value (15 or 16) falls into the three-byte
    branch and writes 3 bytes per pixel, corrupting the framebuffer. bpp is not
    validated at :271 beyond `bpp == 0`.

102. [MEDIUM] src/kernel/console.c:281-292 — the comment says "Derive the grid
    >> STATUS: OPEN — console.c:330 still declares `bytes_per_pixel` and never uses it; it
       is still in the build warning list.

    from the reported pitch rather than assuming 32 pixels of padding ... using
    the pitch is what makes text land in the right place on those", and then
    computes `uint32_t bytes_per_pixel = info->bpp / 8;` and **never uses it**
    (-Wunused-variable, console.c:286). The grid is derived from width/height
    only. The comment describes behaviour the code does not implement.

103. [MEDIUM] src/kernel/console.c:654-655 — `static char cell_store[1]; (void)
    >> STATUS: OPEN — console.c:735-736 still has `static char cell_store[1];
       (void)cell_store;`.

    cell_store;` is a dead placeholder with no purpose.

105. [MEDIUM] src/kernel/kprintf.c:201-205,214-220 — width and precision are
    >> STATUS: OPEN — kprintf.c:259 still does `width = width * 10 + (*p++ - '0')` on a
       signed int with no overflow check, and `emit_padded`'s `total` can still
       overflow.

    parsed with `width = width * 10 + ...` on a signed int with no overflow
    check, and emit_padded then computes `total = len + sign_len + pad`, which
    can overflow. The observable effect of a huge width is a multi-billion-iteration
    emit loop (a hang, not corruption), but it is signed-overflow UB and it is
    reachable from any kprintf format string that an attacker can influence.

107. [LOW] src/kernel/kprintf.c:180-197 — the length-modifier loop accepts `%+`,
    >> STATUS: OPEN — the flag loop still accepts and ignores `%+`, `% ` and `%#`, and
       `%hd`/`%hhd` still read a full int.

    `% ` and `%#` and silently ignores them, and handles only longness >= 2 / == 1
    for `%d`, so `%hd` and `%hhd` both read a full int. `%z` is mapped to
    longness = 1, which is 32-bit-wide on x86-64 by accident.

108. [LOW] src/kernel/panic.c:37-38 — `void panic_regs(struct panic_state *out);`
    >> STATUS: OPEN — panic.c:37 and :38 still declare `void panic_regs(struct panic_state
       *out);` twice, and `panic_lock` (:34) is still never read or written.

    is declared twice. panic.c:34 `panic_lock` is never read or written; the
    file header's "stops other CPUs with a global flag" is panic_cpu (:35), not
    panic_lock.

109. [LOW] src/kernel/kmain.S:38-41 — `.extern __stack_chk_fail` is *described*
    >> STATUS: OPEN (partial) — the `.extern __stack_chk_fail` comment is gone. `.extern
       boot_pml4_phys` (kmain.S:41) is still declared and, as far as this tree
       shows, still unused.

    in the comment as "referenced so a stack-protector build cannot silently
    lose its canary check on this file", but there is no reference to it
    anywhere in the file (and -fno-stack-protector is used anyway).
    `.extern boot_pml4_phys` at :41 is declared and never used.

111. [MEDIUM] src/kernel/tty.c:241-272 — a *blocking* tty_read returns 0 (EOF) when
    >> STATUS: OPEN — tty.c's blocking `tty_read` still returns 0 when nothing is buffered
       and interrupts are off.

     no data is buffered and interrupts are off, rather than blocking or
     returning an error. POSIX requires a blocking read to block. The comment
     acknowledges "Returning 0 is the wrong answer for a caller that asked to
     block" and then returns 0. Combined with #64, this is the *only* code path
     taken, so read(0,...) is always an immediate EOF. There is also no
     scheduler sleep — the wait is a bare `hlt()` loop, acknowledged at :233-238.

112. [INFO] tty_init() is never called. grep for tty_init across src/ finds only
    >> STATUS: OPEN — `tty_init()` still has no callers outside tty.c, so the 8042
       configuration, the IRQ1 handler registration and the PIC1 unmask never run.

     the definition and the klog line inside it, so the 8042 configuration, the
     keyboard IRQ handler registration (idt_set_handler, tty.c:635) and the PIC1
     IRQ1 unmask at :636-638 never execute.

113. [INFO] src/kernel/drivers/serial.c:44 programs divisor 1 (115200);
    >> STATUS: OPEN — the three baud rates still disagree: stage2.c:87 divisor 3 (38400),
       stage1.S:364 divisor 1 (115200), drivers/serial.c:44 divisor 1 (115200).
       Live confirmation: the same log contains stage1's output and stage2's output
       on one wire.

     src/boot/stage2.c:82 programs divisor 3 (38400); stage1.S:364 programs
     divisor 1. docs/src/bootloader/serial-output.md:18 claims 115200. So on
     real hardware stage1 and stage2 log at *different* speeds on the same wire,
     and every existing serial log is a mixture of two baud rates. QEMU ignores
     baud, which is why nobody has noticed.

114. [INFO] src/kernel/include/io.h:253 defines MSR_FS_BASE and it is never
    >> STATUS: OPEN — io.h:253 still defines MSR_FS_BASE and nothing references it.
       (errno.c:7 now documents the consequence accurately; see #117.)

     referenced again — see #118.

115. [INFO—VERIFIED CORRECT] kernel.elf and init.elf both get PT_GNU_STACK RW,
    >> STATUS: VERIFIED CORRECT — readelf still shows `GNU_STACK ... RW` for both
       kernel.elf and init.elf.

     i.e. no executable stack, despite .note.GNU-stack being /DISCARD/ed in
     link.ld:108 and KERNEL_LDFLAGS lacking -z noexecstack. The linker's RWX
     LOAD-segment warning applies to stage1.elf and stage2.elf only, and both
     already pass -z noexecstack.

--------------------------------------------------------------------------------
F. LIBC + USERSPACE
--------------------------------------------------------------------------------

117. [CRITICAL] There is no TLS setup anywhere. MSR_FS_BASE is defined
    >> STATUS: OPEN IN THE KERNEL, FIXED IN LIBC — the *mechanism* this finding
       described is gone: `errno.h:27` now declares `extern int __errno` as an
       ordinary global, `errno.c:18` defines a plain `int`, and `malloc.c:89` holds
       the tcache in a plain static. Nothing in the libc emits an %fs-relative
       access, so "#PF before main() for every userspace program" no longer
       happens. **Still open:** the kernel still provides no TLS. `elf.c:58` still
       defines `PT_TLS` and never handles it, `MSR_FS_BASE` (io.h:253) is still
       never written, and there is no `arch_prctl` in the ABI. Every header now
       says so, so the libc cannot accidentally depend on it again — but a libc
       built against a system that does have threads still cannot work here.

     (io.h:253) and never referenced again; elf.c:58 defines PT_TLS 7 and never
     handles it; and the built init.elf *does* carry a TLS segment
     (TLS0x007ff8 0x408ff8 0x0 0x00c R0x8, .tbss NOBITS).
     src/libc/src/errno.c:11 `__thread int __errno` and
     src/libc/src/malloc.c:72 `static __thread struct tcache *tcache` compile to
     `%fs`-relative accesses (disassembled: `mov %fs:0x0,%rcx`,
     R_X86_64_GOTTPOFF __errno). Failure: %fs base is 0, so crt1.c:103 ->
     __libc_init -> malloc(1) -> tcache_init() touches %fs:0x0 on its first
     instruction -> **#PF before main() for every userspace program**. Both
     errno.h's claim ("the kernel's ELF loader sets up the thread pointer at
     clone time") and malloc.c's per-thread cache design are unfounded.

120. [CRITICAL] src/libc/src/stdlib.c:250-263 — `insertion_sort` destroys the
    >> STATUS: OPEN — still reproduces. src/libc/src/stdlib.c:247-260 still aliases `key`
       to `base[i]` and only copies it back *after* the shifting loop has already
       overwritten it. Transcribed verbatim into /tmp/kilo/docchk/is.c and
       compiled: input `3 2 1` still yields `3 3 3`. The fix needs a temporary
       buffer *before* the loop.

     element being inserted. `char *key = base + i * size;` aliases base[i], and
     the first shift does `memcpy(base + j*size, base + (j-1)*size, size)` with
     j == i, overwriting *key*, which is never saved. Verified with a verbatim
     transcription: input "3 2 1" -> output "3 3 3". Failure:
     introsort_loop ends with insertion_sort (:388) and qsort returns immediately
     for nmemb <= 16 (:394-397), so **every small sort is silently wrong** and
     any large sort ends with a corrupted tail. Latent only because init.c does
     not use qsort.

123. [HIGH] src/libc/src/unistd.c:205-213 — sleep() returns uninitialised stack
    >> STATUS: OPEN — unistd.c:203-211 `sleep()` still declares `struct timespec rem;`
       uninitialised and returns `rem.tv_sec` on the success path. (`usleep()` now
       returns a constant 0, so it is unaffected.)

     memory. `struct timespec rem;` is never initialised, is only written by the
     kernel on -EINTR, and `return (int)rem.tv_sec;` reads it on the common
     success path.

124. [HIGH] src/libc/src/stdlib.c:182-201 — atoi/atol/atoll return uninitialised
    >> STATUS: OPEN — stdlib.c atoi/atol/atoll still declare an uninitialised local and
       ignore strtoxx's return value; strtoxx still writes `*result` only in its
       overflow and success branches.

     memory for non-numeric input. The wrappers declare an uninitialised local
     and ignore strtoxx's return value; strtoxx writes *result only in the
     overflow (:141-155) and success (:162-174) branches, not in the `!any`
     branch (:157-160) nor the invalid-base branch (:106-110). So atoi("abc"),
     atoi("") and strtol with a bad base all read uninitialised stack.

125. [HIGH] src/libc/src/printf.c:146-158 — `%.*s` reads the whole unterminated
    >> STATUS: OPEN — printf.c:154-155 still calls `strlen(s)` and only then clamps to the
       precision. `strnlen` exists at string.c and is still not used here.

     string. `len = strlen(s); if (has_precision && precision < len) len =
     precision;` — the bounded read still walks to the NUL first, so
     printf("%.*s", 4, buf) faults on a short buffer. glibc uses strnlen, which
     already exists in this tree at string.c:92.

126. [HIGH] src/libc/src/stdio.c:519-524 — fopen("...","a+") silently loses
    >> STATUS: OPEN — stdio.c:520-523 still overwrites `fflags` with `FREAD | FWRITE` in
       the `'+'` branch, clearing the FAPPEND set at :510, which makes the very
       next `if (fflags & FAPPEND)` dead.

     O_APPEND. The `'+'` branch sets `fflags = FREAD | FWRITE`, overwriting the
     FAPPEND set at :510, and the `if (fflags & FAPPEND)` test on the next line
     is dead because FAPPEND was just cleared. fflush then never re-issues the
     lseek(SEEK_END) at :135-136, contradicting stdio.c:14-16. Writes land at
     the shared offset and interleave with other appenders.

127. [HIGH] src/libc/src/stdlib.c:49-59 — exit() never flushes stdio, and the
    >> STATUS: OPEN — stdlib.c exit() still does not call `fflush(NULL)` and still carries
       the comment "placeholder - no stdio implemented yet", which is false:
       stdio.c exists and stdout is line-buffered.

     comment claiming "no stdio implemented yet" is false: stdio.c exists and
     stdout_file is _IOLBF (stdio.c:77). Any printf without a trailing newline
     is lost. init.c:484 survives only because it calls fflush(stdout) by hand.

129. [MEDIUM] src/libc/src/stdlib.c:59,64,70 and src/libc/src/unistd.c:199 —
    >> STATUS: OPEN — `sys_exit_group` is still declared plain `void` (stdlib.c:19,
       unistd.c:49) and all four callers are still declared _Noreturn. The build
       still emits `src/libc/src/unistd.c:197: warning: 'noreturn' function does
       return`.

     exit(), _Exit(), abort() and _exit() are declared _Noreturn but all four
     *return*, because sys_exit_group is declared plain `void` (stdlib.c:19,
     unistd.c:50, syscall.c:172). Four -Wnoreturn warnings in the build. If the
     syscall ever fails to terminate the process, control flow is undefined.
     Fix: mark sys_exit_group noreturn; consider -Werror=noreturn.

131. [MEDIUM] src/libc/src/stdlib.c:162-166 — strtoul("-1") returns 1, not
    >> STATUS: OPEN — stdlib.c:166 still applies `if (neg) acc = -acc;` only inside the
       signed branch, so strtoul("-1") still returns 1.

     ULONG_MAX. The sign is only applied in the signed branch
     (`if (neg) acc = -acc;`).

133. [MEDIUM] src/libc/src/string.c:250-255 — strtok is claimed to exist and does
    >> STATUS: OPEN — string.c:251 still claims strtok is a macro over strtok_r; there is
       still no such macro and still no strtok in string.h.

     not. The comment says "the non-reentrant strtok() is a macro over it"; there
     is no strtok macro and no declaration in string.h. init.c:99-101 correctly
     says strtok "is not part of the libc this kernel ships". Two in-scope files
     contradict each other; any ported program using strtok fails to compile.

134. [MEDIUM] src/libc/include/string.h:36-43 — the extern "C" block is closed at
    >> STATUS: OPEN — string.h's extern "C" block still closes before
       strcspn/strspn/strtok_r/strerror.

     line 37, so strcspn, strspn, strtok_r and strerror are declared outside it.
     C++ callers get mangled names and fail to link. stdio.h/stdlib.h/unistd.h
     close correctly.

135. [MEDIUM] Declared but never defined, so the failure is a *link* error rather
    >> STATUS: OPEN (partial) — `__assert_fail` still has a declaration in assert.h and
       still no definition anywhere in src/libc. The mmap/munmap/mprotect and
       getcpu gaps were not re-checked.

     than a compile error: sys/mman.h:28-31 declares mmap/munmap/mprotect but only
     sys_mmap/sys_munmap/sys_mprotect exist (syscall.c:104-124); sys/ioctl.h:17
     declares ioctl with no definition; unistd.h:89 declares getcpu with only
     sys_getcpu; assert.h:15 declares __assert_fail with no definition (and
     malloc.c:29 includes assert.h, so it links today only because assert() is
     never called).

136. [MEDIUM] src/libc/src/stdio.c:101-118 — write_all() reports success after a
    >> STATUS: OPEN — stdio.c `write_all()` still breaks out of the loop on a 0 return and
       still returns success.

     short write. A writer returning 0 is an error per POSIX, but the loop
     `break`s and the function `return 0`, so fflush clears wpos (stdio.c:150)
     and the caller believes the data landed.

137. [MEDIUM] src/libc/src/malloc.c:456 — the doc header advertises "realloc grows
    >> STATUS: OPEN — malloc.c:456's doc header still advertises in-place growth that the
       code does not implement; the shrink split can still produce
       `size_to_class(0)`, and splits are still never coalesced.

     in place when the next chunk is free"; there is no adjacency tracking at all
     (:458-459 says "For simplicity, allocate new and copy"). The shrink split at
     :445-455 can produce remainder->size == 0 -> size_to_class(0) == SIZE_MAX,
     which tcache_free silently drops (leak). Splits are never coalesced.
     malloc.c:304-309 also has an unlocked read-then-lock race on arena_list, and
     spin_lock (:90-99) is a bare __atomic_test_and_set with no backoff.

138. [MEDIUM] src/libc/src/malloc.c:477-494 — aligned_alloc does not enforce
    >> STATUS: OPEN — aligned_alloc still silently rounds a size that is not a multiple of
       the alignment, and `size + alignment + sizeof(void*)` is still unchecked.

     C11's "size must be a multiple of alignment" (it silently rounds, returning
     non-NULL where the standard requires NULL), and `size + alignment +
     sizeof(void*)` is unchecked for overflow.

139. [MEDIUM] src/libc/src/unistd.c:196-199 — _exit() calls sys_exit_group, not
    >> STATUS: OPEN — `_exit()` still calls sys_exit_group rather than sys_exit.

     sys_exit. _exit must terminate only the calling thread. sys_exit exists
     (syscall.c:166) and SYS_exit is wired in the kernel table. Latent (no
     threads today) but wrong.

140. [MEDIUM] src/libc/src/unistd.c:63 — `extern void *__libc_auxv;` conflicts
    >> STATUS: OPEN — unistd.c:63 still declares `__libc_auxv` as `void *` against crt1.c's
       `unsigned long *`.

     with crt1.c:31 `unsigned long *__libc_auxv;`. Different types across
     translation units is UB (C11 6.2.7); it links only because both are
     pointers. unistd.c:58-59 also declares sys_getgid/sys_getegid as returning
     uid_t while syscall.c:227,247 return gid_t.

141. [LOW] src/libc/src/stdlib.c:419-432 — abs/labs/llabs use `j < 0 ? -j : j`,
    >> STATUS: OPEN — abs/labs/llabs still use `j < 0 ? -j : j`.

     which overflows for INT_MIN / LONG_MIN / LLONG_MIN.

142. [LOW] src/libc/src/stdlib.c:235-239 — rand() returns 16 bits, not RAND_MAX.
    >> STATUS: OPEN — rand() still returns `(rand_seed >> 16) & RAND_MAX`, i.e. 16 bits,
       and `rand_seed` still defaults to 1.

     `rand_seed >> 16` is already 16 bits, so `& RAND_MAX` (0x7fffffff) is a
     no-op and the range is [0, 65535] — and the *good* low bits of the LCG are
     discarded. stdlib.c:498 also defaults rand_seed to 1, so rand() is
     deterministic across boots without srand.

143. [LOW] src/libc/src/stdlib.c:480,484,491,498-501 — setenv/unsetenv return -1
    >> STATUS: OPEN — setenv/unsetenv still return -1 without setting EINVAL, and strtol
       still consumes the `0x` prefix for base 16.

     without setting errno for an invalid name (POSIX requires EINVAL) and set
     ENOSYS (also not the specified value). stdlib.c:96 makes strtol("0x10",16)
     consume the 0x prefix, which glibc does not.

144. [LOW] src/libc/src/time.c:21-23 redefines CLOCK_REALTIME/CLOCK_MONOTONIC/
    >> STATUS: OPEN — time.c still redefines the clock ids after <time.h>, and
       `clock_gettime` still rejects the three ids time.h advertises.

     CLOCK_PROCESS_CPUTIME_ID after <time.h> already defines them (legal, no
     diagnostic). time.h:43-45 exposes CLOCK_THREAD_CPUTIME_ID,
     CLOCK_MONOTONIC_RAW and CLOCK_BOOTTIME, but clock_gettime (time.c:27-37)
     rejects all three with EINVAL — the header advertises clocks the
     implementation refuses.

145. [LOW] src/libc/src/stdio.c:450-466 — perror returns early when errno == 0;
    >> STATUS: OPEN — all six sub-items stand: perror on errno==0, fclose(stdout) not
       resetting the static, the `ungot` slot nothing sets, fopen's trailing
       garbage, the unchecked size*nmemb, and byte-at-a-time fwrite.

     glibc prints "Success". stdio.c:470-485 — fclose(stdout) closes fd 1
     permanently without resetting the static stream. stdio.c:257-270 — getc
     honours a stream->ungot slot that nothing ever sets, and there is no
     ungetc. stdio.c:499-524 fopen accepts trailing garbage ("rw" -> O_RDONLY).
     stdio.c:212-225,277-293 compute size*nmemb with no overflow check, and
     fwrite pushes bytes one at a time.

146. [LOW] src/userspace/init/init.c:26-33,340-345 — the comments are stale and
    >> STATUS: OPEN — init.c's stale comments are unchanged.

     the guards they justify are dead. init.c claims "time.h declares
     clock_gettime() but no clock id constants" (time.h:40-45 defines them) and
     "unistd.h declares getcpu"? (it declares it at unistd.h:89 but does not
     define it). init.c:84-96's DEL/backspace handling in echo_line is
     unreachable: getline never echoes and there is no console line discipline,
     so 0x7f/0x08 cannot appear. init.c:57-61 `int main(void)` is called as
     `main(argc, argv, envp)` from crt1.c:106.

147. [LOW] src/userspace/hello/hello.c:28 — `write(STDOUT_FILENO,
    >> STATUS: OPEN — hello.c's `write(..., 19)` on a 20-byte literal still drops the
       newline, and hello is still tracked but never built.

     "direct-write-stdout\n", 19)` on a 20-byte literal drops the newline, which
     defeats the interleaving check the file's own comment (:34-39) says the
     program exists to perform. hello.c is also tracked but never built — no
     Makefile rule references it, and Makefile:54-58's comment about "every other
     program is a separate ELF placed in the initrd for the ramfs to expose"
     describes machinery that does not exist.

148. [INFO—VERIFIED CORRECT, do not re-flag]
    >> STATUS: VERIFIED CORRECT — nothing in this block is contradicted by the current
       tree.

    - string.c:36-56 memmove is correct in all three cases; strlcpy/strlcat are
      correct including size == 0; strtok_r is correct. There is no off-by-one in
      the classic strncpy sense — this libc implements neither strncpy nor      strncat.
    - printf.c:284-372 correctly omits %n (falls to default: and is echoed
      verbatim) and handles a trailing '%' without running off the end.
    - printf.c:438 vsprintf passes SIZE_MAX-1 to vsnprintf; buffer_put's clamping
      keeps it in bounds, so it is unbounded by design but not unsafe.
    - calloc/reallocarray overflow checks (malloc.c:399,470) are correct.
    - syscall.S matches uapi/syscall.h's ABI (rax, rdi/rsi/rdx/r10/r8/r9) and
      __syscall6 correctly reads the 7th argument from 8(%rsp) post-call.
    - Every SYS_* the libc actually issues is present in the kernel's
      syscall_table (30 entries), and syscall.c:183-200's dup2-over-dup layering
      is correct including the ret == newfd early-out.

--------------------------------------------------------------------------------
G. BUILD SYSTEM, TOOLS, CI, REPO HYGIENE
--------------------------------------------------------------------------------

149. [HIGH] tools/disk.py:33-35,114-119,155-157 — the kernel may overwrite the
    >> STATUS: OPEN — tools/disk.py:33-35 still has KERNEL_LBA 64, KERNEL_MAX_SECTORS 4096
       and INITRD_LBA 4096 with only `kernel_sectors > KERNEL_MAX_SECTORS` checked,
       so a kernel of 4032-4096 sectors still silently clobbers the initrd.
       `KERNEL_MAX_SECTORS` still has no counterpart in boot_layout.h.

     initrd. KERNEL_LBA = 64, KERNEL_MAX_SECTORS = 4096 (so LBA 64..4159),
     INITRD_LBA = 4096, and place(KERNEL_LBA, kernel) runs before
     place(INITRD_LBA, initrd). The only bound check is
     `kernel_sectors > KERNEL_MAX_SECTORS`; nothing compares
     KERNEL_LBA + kernel_sectors against INITRD_LBA. A kernel of 4032-4096
     sectors (2.0-2.1 MiB) passes and silently clobbers the first 128 sectors of
     the initrd. KERNEL_MAX_SECTORS also has no counterpart in boot_layout.h, so
     the two files cannot cross-check it. Today it is fine (713 KB = 1393
     sectors) but the margin is only 3.5x.

150. [HIGH] tools/disk.py:40,76-77 vs tools/initrd.py:28 — the two tools disagree
    >> STATUS: OPEN — tools/disk.py:76 still computes `total = 16 + len(index) + len(body)`
       against a 24-byte `INITRD_HEADER_FMT`.

     on the initrd header size. `INITRD_HEADER_FMT = "<QIIQ"` is 24 bytes
     (8+4+4+8), but disk.py hardcodes `total = 16 + len(index) + len(body)`.
     initrd.py gets it right via struct.calcsize(). The written total_len is 8
     bytes short of the real bundle size. Dead today (the Makefile always passes
     --initrd, never --init) but the moment someone uses --init the bundle's
     self-description is wrong. Better: delete build_initrd() from disk.py so
     there is one implementation of the format.

151. [HIGH] Makefile:181-183 — KERNEL_VERSION_DEFS is computed and applied only to
    >> STATUS: OPEN — Makefile:181-183 still applies `KERNEL_VERSION_DEFS` only to the
       init.elf link at :138. Boot log confirms the consequence: `rev unknown` on
       the kernel banner, and `__DATE__ " " __TIME__` (which is why the banner
       reads `Oct  1 2026 23:10:10`, a local-time stamp that changes every time
       make runs).

     the init.elf link (Makefile:138). The kernel compile (:92,96,100,158) and
     link (:161) never see it, so kernel.elf falls back to version.h:15-24:
     KERNEL_GIT_REV "unknown" and KERNEL_BUILD_STAMP __DATE__ " " __TIME__.
     That defeats version.h:4-9's stated purpose and makes the kernel
     non-reproducible. Additionally Makefile:180 re-evaluates `date -u` on every
     invocation, so the stamp baked into a binary depends on when make last ran.

152. [HIGH] Makefile:201-215 — `make deps` fails on a clean tree. It does
    >> STATUS: OPEN — `make deps:` still does `@mkdir -p $(OBJ)` only, not `@mkdir -p $(dir
       $$o)`.

     `@mkdir -p $(OBJ)` only, not `$(dir $$o)`, so with no build/obj/kernel/ yet
     the first `-MF build/obj/kernel/main.c.o.d` fails and `@set -e` aborts the
     target.

153. [HIGH] Makefile:75-77 and :102-104 — -MMD -MP is missing for     $(OBJ)/boot/%.S.o and $(OBJ)/libc/%.S.o, while the kernel .S rule at :90-92
    >> STATUS: OPEN — Makefile:75-77 and :102-104 still omit -MMD -MP for the boot and libc
       `.S` rules while the kernel rule at :90-92 has them.

     has them. src/libc/src/syscall.S therefore has no header-dependency tracking
     at all.

154. [MEDIUM] Makefile:238 — `-include $(shell find $(OBJ) -name '*.d' ...)` forks
    >> STATUS: OPEN — Makefile:238 is still `-include $(shell find $(OBJ) -name '*.d'
       2>/dev/null)`.

     a find on every make invocation and finds nothing on a clean tree, so the
     first build has no header dependencies at all (compounding #152). Fix:
     `-include $(KERNEL_OBJ:.o=.o.d) ...`.

155. [MEDIUM] Makefile:44,147-150,159-162 — KERNEL_ENTRY_OBJ (kmain.S.o,
    >> STATUS: OPEN — Makefile:44 still lists KERNEL_ENTRY_OBJ explicitly in the link line
       while the same objects are also matched by the wildcards and land in libk.a.

     main.c.o) is linked explicitly *and* is matched by the KERNEL_C_SRC/
     KERNEL_S_SRC wildcards, so both are also in libk.a. The direct copies win
     and the archive members are simply never pulled. It links today only
     because of ordering. Fix: `$(filter-out $(KERNEL_ENTRY_OBJ),$(KERNEL_OBJ))`
     for the archive.

156. [MEDIUM] tools/initrd.py:72-74 — `int(mode_s, 8)` on a malformed
    >> STATUS: OPEN — tools/initrd.py:71 still does `int(mode_s, 8)` with no try/except,
       and still mixes `raise SystemExit(1)` with `return 1`.

     `--program name:9z=x` raises an uncaught ValueError (traceback, exit 1)
     instead of the friendly `print(...); return 1` used everywhere else in
     these tools. tools/initrd.py:59-64 also uses `raise SystemExit(1)` inside
     build() while main() returns 1, mixing two error styles.

157. [MEDIUM] tools/disk.py:43-47 — `pad_to_sectors(data, what)` never uses `what`
    >> STATUS: OPEN — tools/disk.py:43-47 still ignores `what` and never exits; `read()`
       still does not catch OSError.

     and never exits, despite the docstring "or exit if it needs more than the
     reserved space". The real bounds checks live in main(). tools/disk.py:50-52
     `read()` does not catch OSError, so a missing input yields a Python     traceback rather than the clean `error: ...` the rest of the file uses.

158. [MEDIUM] tools/psf2c.py:30-33 — the PSF1 branch reads
    >> STATUS: OPEN — tools/psf2c.py:32-34 still reads the PSF1 length as `raw[3] | (raw[4]
       << 8)`, where raw[4] is the mode byte; the real height/width at 24/28 are
       still unread and charsize is still used as the row count.

     `length = raw[3] | (raw[4] << 8)`, but raw[4] is the PSF1 *mode* byte; the
     length field is one byte. For the common Unicode-mode font (mode == 1) this
     yields length = 512 and then trips the truncation check with a misleading
     "truncated font data" error. PSF1 mode-0 fonts happen to work.
     tools/psf2c.py:22-29 never reads the real height/width at offsets 24/28 and
     uses charsize as the row count (correct only for square fixed-width fonts);
     the header comment at :25 misstates the PSF2 layout (the real header is 32
     bytes with four 4-byte magic/version/headersize/flags). tools/psf2c.py:67 is
     an f-string with no placeholders.

159. [MEDIUM] .github/workflows/pages.yml — CI builds and tests nothing. 25 lines:
    >> STATUS: OPEN — .github/workflows/pages.yml is still 25 lines of checkout plus `uses:
       Omena0/fr-docs@main`, still with no SHA pin while holding `id-token: write`
       and `pages: write`.

     one actions/checkout, then `uses: Omena0/fr-docs@main`. No make, no
     toolchain check, no run.sh, no disk image, no tests. A commit that cannot
     link still goes green. Separately, `uses: <repo>@main` with **no SHA pin**
     while the job holds `id-token: write` and `pages: write` (:8-10) means a
     third party's mutable branch executes with OIDC token-minting and
     Pages-write authority; it should be pinned to a commit SHA. Also
     `concurrency.group: "pages"` (:13) will likely collide with the called
     workflow's own group.

160. [MEDIUM] config.json:4-5 — "copyright_holder": "CHANGE_ME" is an untouched
    >> STATUS: OPEN (amended) — config.json still has "copyright_holder": "CHANGE_ME" and
       an empty project_url, and its src_dir/docs_dir/patterns combination is still
       self-contradictory. **New:** "project_name" is still "Os" and the sidebar
       still lists exactly four entries (index, installation, quickstart,
       configuration), none of which exist.

     template placeholder and "project_url" is empty. config.json and
     docs/config.json are byte-identical except project_name, with no
     synchronisation, and the internal src_dir "src" + docs_dir "." +
     patterns ["docs/src/*.md"] combination is self-contradictory.

162. [MEDIUM] .gitignore is three lines (/build, .vscode, .kilo). .venv/ is
    >> STATUS: OPEN (amended) — .gitignore is now four lines: `/build`, `.vscode`, `.kilo`,
       and `MEGA_AUDIT.md`. `.venv/`, `__pycache__/`, `*.pyc`, `*.log` and `*.img`
       are still uncovered. **New and important:** this audit document itself is
       now gitignored *and* untracked, so it does not survive a clone — see the
       note at the top of this file.

     untracked today only because the venv tool wrote its own .venv/.gitignore.
     .venv, __pycache__/, *.pyc, *.log and *.img are uncovered.

163. [LOW] Makefile:131-133, 155-158, 164-171 — $(INITRD) and $(DISK) have no
    >> STATUS: OPEN — Makefile still gives $(INITRD)/$(DISK) no dependency on
       tools/initrd.py or tools/disk.py, and Makefile:157 still writes to a
       hardcoded `build/initrd.c`.

     dependency on tools/initrd.py or tools/disk.py, and the initrd.c.o rule has
     none on tools/bin2c.py. Editing a tool does not trigger a rebuild.
     Makefile:157 also redirects to a hardcoded `build/initrd.c` instead of
     `$(BUILD)/initrd.c`, so changing BUILD in config.mk:15 leaves the generated
     file outside `make clean`'s reach.

164. [LOW] src/config.mk:10-13 + Makefile:113,123 — HOST_LD_32 already contains
    >> STATUS: OPEN — the stage2 link line still reads `ld -m elf_i386 -m elf_i386`.

     `-m elf_i386` and the Makefile passes it again, so the link lines read
     `ld -m elf_i386 -m elf_i386`.

165. [LOW] run.sh:61-68 — `make run-gdb` is broken. The script tests `${1:-}` for
    >> STATUS: OPEN — run.sh:61-68 still tests `${1:-}` and never shifts, so `--gdb` is
       forwarded to QEMU.

     `--gdb` and appends `-s -S`, but never `shift`s, so `"$@"` on line 68
     forwards `--gdb` to QEMU as an unknown option and QEMU exits.

166. [LOW] run.sh:47-51 — build/serial.log stays empty. `-serial mon:stdio`
    >> STATUS: OPEN — run.sh:48-50 still pairs `-serial mon:stdio` with an explicit
       `-device isa-serial` that lands on COM2, leaving build/serial.log empty.

     already claims ISA slot 0 (COM1, which the kernel drives); the explicit
     `-device isa-serial,chardev=serlog` lands on COM2. The log the comment on
     :44-46 calls "what a bug report can be built from" captures nothing. This is
     why finding #1 is invisible from `make run`.

167. [LOW] run.sh:21 — SMP=18 under TCG on a 2-4 core CI runner will thrash,
    >> STATUS: OPEN — run.sh:21 still defaults SMP=18 and run.sh:68 still hardcodes
       `qemu-system-x86_64`.

     contradicting the "a headless build agent can still exercise the boot path"
     claim on :8-9. run.sh:68 also hardcodes `qemu-system-x86_64` even though
     config.mk:88 defines QEMU.

168. [LOW] .markdownlint.json disables MD060/MD013/MD040 but nothing in CI runs
    >> STATUS: OPEN — .markdownlint.json is still unenforced; nothing in CI runs
       markdownlint.

     markdownlint (see #159) — dead config.

--------------------------------------------------------------------------------
H. DOCUMENTATION vs IMPLEMENTATION
--------------------------------------------------------------------------------

170. [HIGH] docs/src/bootloader/ describes a different bootloader. Concrete
    >> STATUS: OPEN (largely corrected) — the largest documentation finding; the whole
       `docs/src/bootloader/` section described a different bootloader. Corrected
       in this pass: `stage2.md` (rewritten: PIC masking, the real A20 method
       order and its verification, the two E820 conventions, E820_ADDR 0x90000,
       PML4 511 / PDPT 510, the 4 KiB first-2-MiB window and why it is forced,
       no direct map in stage 2, the real memory map, the real `struct bootinfo`,
       the `gdtr64` subtlety, phase-correct IDT gates, and a Known Gaps section),
       `serial-output.md` (the three different divisor values in a table instead
       of a single false 115200), `multiboot.md` (now says outright that the
       feature is absent), and `kernel/early-boot.md` (`struct bootinfo` has no
       `memory_map`, `memory_map_count` or `cpu_count`). **Still wrong:**
       `build.md` (sector 128, a `src/boot/config.h` that does not exist, NASM
       instead of GNU as/ld), `stage1.md` (446 bytes, INT 13h/AH=42h with a DAP,
       stack at 0x7000), `kernel-handoff.md` (BOOT_MAGIC 0xB007B007, a struct
       with a different field order, and an rsp/CR0.WP/CR4.PGE/RSI handoff table
       the loader does not perform), and `memory-map.md`.

     doc-vs-code disagreements, every one of which is a silent-corruption
     constant if anyone writes code against it:
     - build.md:44,57 "Sectors 128+ / reads kernel from sector 128" vs
       boot_layout.h:67 KERNEL_LBA 64 and disk.py:33 KERNEL_LBA = 64. build.md:52
       also names a src/boot/config.h that does not exist, and build.md:37 /
       stage1.md:66 claim NASM while the Makefile uses GNU as/ld.
     - stage1.md:16,29 "446 bytes", INT 13h AH=42h with a DAP, stack at
       0x7000:0000 — vs stage1.S:107 (the0x8000 jump is right), :318-324 (CHS,
       AH=02h), :47 (sp = 0x7c00) and boot_layout.h:194 (0x7E00). The DAP is
       gone; boot_layout.h:95 DAP_ADDR is dead.
     - stage2.md:26 and memory-map.md:14-16 put the E820 table at 0x500 and the
       boot page tables at 0x20000; the code uses boot.h:34 E820_ADDR 0x00090000
       and boot_layout.h:280 BOOT_PT_ADDR 0x002D0000. stage2.md:36-38 /
       memory-map.md:60-66 also describe a 64 GB direct map and a 512 MB kernel
       window; boot_page_tables_init maps only 0-4 GiB identity plus an 8 MiB
       kernel window — no direct map at all.
     - kernel-handoff.md:31-53 defines BOOT_MAGIC 0xB007B007 and a struct
       BootInfo with uint32_t magic, an embedded memory_map[128] and a
       different field order, vs boot.h:19 0x4F53424F4F543031 and boot.h:83-102.
       The table at :14-27 also promises rsp = 0xFFFF_FFFF_8010_0000, CR0.WP=1,
       CR4.PGE=1 and rsi = 0; the loader supplies none of these
       (stage2_long.S:320-324 leaves RSP in the low loader stack; CR0.WP and
       CR4.PGE are never set; RSI is whatever bios_call left).
     - kernel/early-boot.md:33-38 uses boot_info->memory_map/memory_map_count and
       :63 uses boot_info->cpu_count; struct bootinfo has neither.
     - serial-output.md:18 "Baud rate: 115200" vs stage2.c:82 divisor 3
       (38400); stage1.S:364 uses divisor 1, so the two stages log at different
       speeds on the same wire. Message prefixes are [BOOT1]/[BOOT2] vs the
       actual [boot]/[boot2].
     - multiboot.md documents an entire Multiboot2 -kernel dev path with a
       .multiboot section. grep finds no MB2 magic, no .multiboot section, and
       run.sh boots build/os.img only. The whole file describes a feature that
       does not exist.
     - memory-map.md:18 "0x8000 96 KB Stage 2 binary" — the real ceiling is
 32 KiB (63 sectors).
     The stage1 jump target (stage1.md:29's 0x8000) is the one thing stage1.md
     gets right.

171. [MEDIUM] The memory docs describe subsystems with no code, in language that
    >> STATUS: OPEN — every listed subsystem is still described in the present tense with
       no code behind it: GFP_HUGE, struct page.numa_node, the per-CPU SLAB
       magazine, the 128-entry per-CPU page cache, multi-page slabs, the
       depot-magazine swap. memory/overview.md's locking table is still wrong on
       both counts.

     reads as if they exist: overcommit-policy.md, page-reclamation.md,
     memory-compaction.md, numa-policies.md, huge-pages.md, userspace-malloc.md.
     Concretely absent: GFP_HUGE is defined (pmm.h:33) and ignored by
     pmm_alloc_pages; struct page.numa_node does not exist;
     memory/overview.md's locking table claims the per-CPU SLAB magazine is
     lock-free and the PMM has a 128-entry per-CPU page cache — neither exists
     (kmalloc.c:16-32 correctly says the magazine is not lock-free, and
     pmm_alloc_pages always takes the zone lock); slab-allocator.md describes
     multi-page slabs and a depot-magazine swap, but kcache_init_one
     (kmalloc.c:143-160) picks the *smallest* order holding 4 objects, so every
     class except 4096 uses a single 4 KiB order-0 slab.

174. [MEDIUM] architecture/overview.md:37-45 and docs/src/scheduling/*.md,
    >> STATUS: OPEN — all fourteen component sections still present tables of components
       with no code behind them. The listed absent symbols are still absent.

     syscalls/*.md, filesystem/*.md, ipc/*.md, networking/*.md, userspace/*.md,
     drivers/*.md present tables of components and behaviours — dynamic kernel
     modules with versioned symbols, ext4 with journaling and extents, VFS, page/
     inode/dentry caches, pipes as zero-copy ring buffers, shared memory, TCP/IPv4
     with a BSD socket API, hybrid kernel/userspace drivers, capabilities,
     seccomp-like syscall filtering, namespaces, stack canaries, a hardened
     allocator with guard regions, ASLR, a GUI windowing application, an init
     system with dependency ordering and restart policy — for which there is no
     code. Verified absent: sched_setaffinity/getaffinity, sched_setparam,
     sched_setattr, kill, clone, sigaction, cap_get/cap_drop, seccomp,
     unshare/setns, futex, socket, mlock, any SYS_fcntl handler, work-stealing,
     load balancing, the reschedule IPI, and any core-weight model. The
     incompleteness is invisible to grep because it is not annotated (#8).

175. [MEDIUM] architecture/posix-compliance.md, abi-stability.md and
    >> STATUS: OPEN — the versioned-ABI claims stand unchanged, and they are worse than the
       audit says: #116's entry-ABI mismatch is fixed but #117 (no TLS) is not, so
       no userspace binary built against this header can run on this kernel as
       shipped.

     syscalls/abi-versioning.md promise a versioned syscall ABI with stability
     guarantees across kernel updates within a major version. uapi/syscall.h does
     define a version field and the syscall table is a flat 30-entry array
     (syscall.c), but there is no version negotiation at entry, no
     compat/future layer, and the "code_references" the docs claim is
     documentation-only. Combined with #116 (process entry ABI mismatch) and
     #117 (no TLS), no userspace binary built against this header can run on
     this kernel as shipped.

176. [LOW] prompt.txt (6.2 KB, the generator prompt that specifies this OS) is
    >> STATUS: OPEN — prompt.txt is still tracked at the repo root.

     tracked at the repo root.
