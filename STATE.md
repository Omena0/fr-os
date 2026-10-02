# STATE.md — Fr OS

Everything a new session needs. Written by the agent that worked this tree for
most of one day. **Read this before touching anything: most of the "obvious"
conclusions about this codebase are wrong, and that is the single most useful
thing in this file.**

---

## 1. What the project is

A hobby x86-64 OS in `/home/omena0/Github/os`, booting under QEMU from a
stage1/stage2 bootloader into a freestanding kernel, with userspace, libc and an
init process in progress.

**Branding** (defined once in `src/include/version.h`):
`Fr OS` (project) · `Fr Core` (kernel) · `Fr Boot` (bootloader) ·
`Fr Init` (init) · `Fr Libc` · `Fr Userland`.

**Tree**: `src/{boot,kernel,libc,userspace,include}`, `src/config.mk`,
`tools/`, `tests/`, `docs/`, `MEGA_AUDIT.md` (177 findings, now a work queue).

**Build**: `flock /tmp/fr-build.lock make clean >/dev/null 2>&1 && flock /tmp/fr-build.lock make -j8`
(~1 s). `make check` runs `verify-isa-test`.

**Boot**:
```
cp build/os.img /home/omena0/.fr-tmp/<you>.img     # you WILL hit "write lock" otherwise
timeout -s TERM 25 qemu-system-x86_64 -cpu max \
  -drive file=/home/omena0/.fr-tmp/<you>.img,format=raw,if=ide,index=0,media=disk \
  -m 4G -smp 1 -machine pc,acpi=off -no-shutdown -device VGA -display none \
  -chardev file,id=s0,path=/home/omena0/.fr-tmp/<you>.log -serial chardev:s0
```
Add `-d int -D /home/omena0/.fr-tmp/<you>-int.log` for the fault vector, CR2 and
error code.

---

## 2. THE BLOCKER — read this first

**The kernel boots through vmm → pmm → idt → PIT, and stops inside
`gdt_flush()` at its far return.** PID 1 never starts, so no userspace runs.

Last known-good output:
```
idt: 256 entries at ffffffff8002ec30, 4095 bytes
pic: remapped to 20-30, both lines masked
pit: ch0 divisor 11931 = 100.0 Hz, IRQ0 on vector 33
ABCD                      <- markers A-D, E never prints
```

### The lead: `gdt_flush` re-enters itself

A gdb run (break at `gdt_flush`, then `finish`) produced:
```
Breakpoint 1, gdt_flush () at interrupt_entry.S:214   <- first hit
== entry: rsp=0xffffffff801b8fb8 cs=0x8
   0xffffffff80010a6e <gdt_flush>:  mov    $0x41,%al
   0xffffffff80010a70 <gdt_flush+2>: out    %al,$0xe9
   0xffffffff80010a72 <gdt_flush+4>: mov    %rdi,%rax
   0xffffffff80010a75 <gdt_flush+7>: lgdt   (%rax)

Breakpoint 1, gdt_flush () at interrupt_entry.S:214   <- SECOND hit
== returned, rip=0xffffffff80010a6e                       <- its own first instruction
```
**The breakpoint hit twice and `finish` returned to `gdt_flush`'s entry.**
Infinite recursion there exhausts the stack silently and would explain every
symptom: no output past the E820 dump, a garbage RIP, dozens of boot cycles.

**Caveat, and it matters:** gdb's `finish` is known to misbehave across an
unusual control transfer like `lretq`, so this may be a gdb artefact rather than
real recursion. **Settle it with a counter, not with gdb** — that is the one
cheap, falsifiable experiment left:
```asm
	/* in gdt_flush, first instruction */
	incq	gdt_flush_entries(%rip)     /* or: movl $1,%eax; outl %eax,$0xE9 */
```
If it re-enters, the counter tells you immediately. **Do that before generating
another hypothesis.**

### Second observation about that run — check this too
The first instruction is `mov $0x41,%al; out %al,$0xe9`. **Port 0xE9 is QEMU's
debug-exit port.** Writing it is normally harmless without
`-device isa-debug-exit`, but it is an unusual thing to do at the top of a
function and the marker macro is worth removing once the blocker is closed.

### Excluded already, by measurement (do not re-derive these)

| hypothesis | how excluded |
|---|---|
| far-return frame wrong | gdb single-step: RIP at `rsp+16`, CS at `rsp+8`, RFLAGS at `rsp` |
| timer tick landing inside the frame | `cli` across `gdt_reload`, PIT already running → **no change** |
| faults undeliverable through stage2's IDT | **fixed** — `idt_init()` moved before `gdt_reload`; `IDT=ffffffff8002ec30` |
| GDT descriptors malformed | `_Static_assert` round-trip on every descriptor in `src/include/gdt.h` |
| `LTR` refusing the TSS | **fixed** — 64-bit TSS layout, byte 7, `HIGH` narrowed |
| RSP alignment at `lretq` | entry 8 mod 16 (per SysV), after 3 pushes 0 mod 16 — requirement met |

### Current `gdt_flush`
```asm
	movq	%rdi, %rax
	lgdt	(%rax)
	movw	$KERNEL_DATA_SELECTOR, %ax ; → %ds/%es/%fs/%gs/%ss
	pushq	$0x202			/* RFLAGS */
	pushq	$KERNEL_CODE_SELECTOR	/* CS */
	pushq	.Lgdt_flush_resume	/* RIP */
	lretq
.Lgdt_flush_resume:
	ret
```
Disassembly is `68 02 02 00 00 / 6a 08 / 68 <rip> / 48 cb` — correct.

**Known smell:** `gdt_flush` has a **three-argument C prototype**
(`gdt.c:95`: `gdt_flush(gdt_pointer, code_selector, data_selector)`) but the
assembly **hardcodes both selectors and never reads `%rsi`/`%rdx`**. Harmless
today — the only caller passes exactly those two — and a trap the moment
anything calls it with a different segment. Either use the arguments or drop
them from the prototype.

---

## 3. Fixed this session (all committed)

Bootloader / CPU bring-up:
- **PICs are now masked** at the top of `stage2_main`. The BIOS leaves IRQ0
  unmasked, so an 18.2 Hz tick landed inside a real-mode BIOS round trip with
  stage2's IDT installed and dispatched 32-bit reporter code as 16-bit garbage.
  This was the "loader is mysteriously slow / dies at a random point" bug.
- **`GDT_ENTRY` never wrote byte 6** of a descriptor — it folded the flags
  nibble into the access byte. Every kernel code segment had `L=0` and `lgdt`
  made each CS load `#GP`. Now `GDT_ENTRY(access, flags, base, limit)`, with
  `GDT_FLAG_*` in byte-6 positions (`G` bit 3, `L` bit 1, `DB` bit 2).
- **TSS descriptor byte 7 was built from the limit**, not `base[31:28]`. And
  `GDT_TSS_DESC64_HIGH` carried `base[24:55]`; it must carry only
  `base[63:32]`.
- **The TSS was a 32-bit layout** (4-byte IST slots, 80 bytes); 64-bit mode
  needs 8-byte slots and 108 bytes.
- `GDT_ACCESS_RING3` was `(3<<6)`; DPL is bits 6-5, so it is `(3<<5)`.
- **GS base**: `WRMSR` to `IA32_GS_BASE` writes the *hidden* base only, and it
  `#GP`s under KVM. The kernel now uses `WRGSBASE` (with `CR4.FSGSBASE` set by
  the loader in `stage2_long.S`), verified by reading the base back with
  `movq %gs`. **Boots on TCG and KVM.**
- **`CPUID.1:ECX` bits 27/28 were swapped** (27 = AVX, 28 = OSXSAVE). Three
  independent measurements agree `ECX.27` is clear here, so `.Lno_avx` is taken
  and **XCR0 is never written on this machine**.

Kernel correctness:
- **`panic.S` labelled every register one slot out.** `cs` printed RFLAGS,
  `rflags` printed CR2, `cr2` printed CR3, `cr3` printed CR4, `cr4` was never
  written. All offsets now pinned by `_Static_assert(offsetof(...))`.
  `cs` is still never written — it has no MOV-source form in 64-bit mode and
  `push cs` is not encodable; the receiving stub must store it.
- **`copy_from_user()` copied in the wrong direction** —
  `memcpy(page + off, NULL + done, ...)` read from address 0 and wrote into the
  user page, so `read()` returned success with an uninitialised buffer.
- **`ret_to_user()` never wrote RAX**, so a forked child entered ring 3 with
  kernel residue instead of 0.
- **`build_missing_tables()` only ever created a PD and a PT.** Nothing else
  created PDPT or PML4 entries, so every `vmalloc()` returned NULL, taking out
  `vma_alloc`, `kstack_alloc` and every `kmalloc` over 4096. Now walks down
  from the PML4.
- **`vmm_translate()`'s `0` sentinel** — indistinguishable from a real mapping
  of physical frame 0. Added `vmm_lookup_page()` with a presence flag; all three
  callers converted (`process.c` ×2, `mm.c`).
- **TLS thread pointer** was `vaddr + memsz` instead of
  `ALIGN_UP(vaddr + memsz, p_align)`. **Nothing faults** — local-exec TLS uses
  *negative* displacements, so a 4-byte error lands on the already-writable
  `.tdata` page and corrupts `__libc_tls_sentinel`.
- **PT_LOAD contents were dropped entirely** (`file_off = p_offset -
  (seg_vaddr - seg_start)`, short by the same in both offset and length), so
  `.init_array`/`.data` never loaded and libc constructors silently did nothing.
- **`.tbss` zeroing destroyed `.init_array`** — a NOBITS section overlaid on
  what follows.
- **`idt_init()` moved before `gdt_reload()`** so faults are deliverable.

Tooling / build:
- **`make` did not reliably rebuild `stage2.c.o`** after header edits. Root
  cause was two independent bugs (three of four loader objects had no `-MMD`
  at all; and make cannot see a dependency when a header mtime is *rewound*,
  which `cp -a` does not cause but rsync/tarball extraction does). Fixed with a
  content digest over all headers + `Makefile` + `config.mk` as a real
  prerequisite. No-op make: 0.026 s.
- **`verify-isa-test` no longer gates `all`** — it checks the checker, not the
  tree, and a stale expectation in it was stopping the kernel being built at
  all. `make check` runs it. The gate that *does* gate every build —
  `verify-isa` on `kernel.elf`, `init.elf` and `hello.elf` — is unchanged and
  now has `PROFILE=kernel|user`.
- `tools/ownership.py` — claim/release/view/check with flock, TTL expiry
  (`3h`), auto-expiry treated as free, force-take. **Release when you finish**;
  a stale claim blocks the next person. `ownership.txt` is deprecated.

---

## 4. THE METHOD — the most important section

**Of fourteen fixes attempted, eleven produced no diagnostic at all. Of the
three that did, two reported the wrong fault.**

The tools that found every real bug:
- `objdump -d build/kernel.elf` — reading the **linked artefact**
- `-d int` for fault vector / CR2 / error code
- `-d exec,nochain,in_asm` for the hot block (histogram the addresses; 735 MB
  of one function is how the `serial_putc` spin was found)
- **differential testing against host libc** (`snprintf`)
- gdb single-stepping, QMP `pmemsave`/`screendump`
- **`_Static_assert` so the compiler checks the encoding for you**

The tool that wasted the day: **deriving what the code "should" produce and
"fixing" it when it disagreed.** I got the descriptor layout wrong **twice in
opposite directions** and edited correct code both times. The asserts I added
*afterwards* caught each one within seconds. **Treat any derivation you write
as a hypothesis to be checked, never as evidence.**

Corollary: **a write to somewhere valid says nothing.** The TLS off-by-4, the
`vmm_translate` sentinel and the zeroed `serial_puthex` digit table are all
"correct-looking writes to a mapped address". Silent corruption looks exactly
like working code.

---

## 5. ENVIRONMENT TRAPS (each one cost real time)

- **`/tmp` hit its quota.** A 3.4 GB `-d exec` trace. Under that condition
  `printf x > /tmp/x` returns rc=1 leaving a **0-byte file**, and every QEMU
  whose serial log went to `/tmp` produced a 0-byte log while exiting rc=0 —
  which reads exactly like a regression. **This is how I once reported "0
  exceptions" from an empty log.** Use `/home/omena0/.fr-tmp/`.
- **`export TMPDIR=/home/omena0/.fr-tmp`** or gcc fails writing temp assembly.
- **`-serial chardev:s0` uses a colon.** `chardev=s0` gives a 0-byte log and a
  one-line QEMU error that is easy to miss.
- **Always `timeout -s TERM`, never the default SIGKILL** — SIGKILL loses
  QEMU's serial buffer and truncates the log.
- **Always `make clean`.** A plain `make` silently kept stale objects.
- **`gdb` works only inside a single bounded shell command together with
  QEMU** (`qemu ... -S -s & sleep 4; gdb -q -batch -x s.gdb; kill %1`), with
  `file build/kernel.elf` as the first gdb line. Backgrounding them separately
  produces no output at all.
- **QMP needs `qmp_capabilities` executed first**, and `pmemsave`/`screendump`
  write to a **file**.
- **Commit protocol**: stage your own files and commit in **one** call
  (`git add -- <files> && git commit`). Never `git add -A`/`.`/`-u`/bare
  `commit -a` — they sweep in other agents' in-flight work. Never
  `checkout`/`restore`/`reset`/`stash`/`clean`/`rebase`/`merge`. Committing is
  allowed and encouraged.

---

## 6. MEGA_AUDIT.md

177 findings, now a **work queue**: *a finding is available iff its text is
present.* Deleting a block is the claim. Section 0 lists fixes with the
mechanism **and why it was silent**; A0 registers what was lost (66 findings
whose text no longer exists — **unrecoverable**, it was already down to 121 at
its first commit); A1 records corrections to findings that were **wrong**
(six were: #4, #29/#30, #36, #32, #97, and #15 being half wrong).

**The header warns that line numbers outside what was re-derived are
unchecked** — commit `1401355` moved `interrupt_entry.S` by 43 lines and rotted
several citations. Nine were fixed; **all 112 were not audited.** Re-derive a
finding's line before acting on it.

Finding **0.27** is the current blocker (recorded so it is not lost).

---

## 7. KNOWN OPEN, IN PRIORITY ORDER

1. **The blocker** (§2). Count `gdt_flush` entries first.
2. **The `cli`/`sti` pair** around `gdt_reload` is committed but is **not** the
   fix — it made no difference. Keep it; it is correct anyway.
3. `verify-isa` self-test: 4 of 19 checks fail. Three look like wrong
   *expectations* in the fixtures; **`user-plus-v3` reporting an AVX object as
   clean may be a real gate hole.** Unowned.
4. `sys_execve` (`process.c:1343`) returns its error unlogged — only
   `exec_load_and_run` was instrumented.
5. `sched.c` never calls `task_save_fs_base`/`task_load_fs_base`, so the FS base
   does not travel with a task across a switch.
6. `process.c` never sets `t->fs_base` from `mm->tls_ptr` on exec. `ret_to_user`
   needs `call task_load_fs_current` before its `iretq` for **fork** (a child
   never re-runs `crt1`, so nothing else calls `arch_prctl` for it).
7. `ret_to_user` clears RAX/RCX/R8-R10 but leaves R11 (the frame pointer)
   deliberately — worth a second opinion.
8. `init` will need TLS working before its `getline`/`malloc`.

---

## 8. WHAT NOBODY SHOULD RE-DERIVE

- The GDT/TSS descriptor layout is **correct and `_Static_assert`-pinned**.
- The far-return frame in `gdt_flush` is **verified correct by gdb
  single-stepping**. Do not "fix" it again.
  - `direct map 0-4 GiB, 1 GiB pages` and the E820 map with
  `e820[6] fd00000000-ffffffff reserved` as the highest usable entry are both
  correct.
- A **zero-byte serial log means a bad command, a full filesystem, or a wrong
  path — not a healthy guest.** Check `df`, check the colon, check the path.

---

## 9. GIT STATE at hand-off

Branch `main`, clean tree. Recent commits:
```
09c236e kernel: mask interrupts across the descriptor reload
f77026d kernel, docs: TLS alignment and PT_LOAD fixes; audit reconciled
fb261ef kernel: finish the sentinel and panic fixes properly
e87ebf4 kernel: install the IDT before the first GDT reload
```
Commit messages in this repo carry the mechanism and the verification, and
say plainly what is **not** fixed. That convention is worth keeping.