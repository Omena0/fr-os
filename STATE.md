# STATE.md — Fr OS

Everything a new agent needs to become productive on this tree. Read this before
touching anything: most of the "obvious" conclusions about this codebase are
wrong, and that is the single most useful thing in this file.

---

## 1. What the project is

A hobby x86-64 OS in this repo, booting under QEMU from a stage1/stage2
bootloader into a freestanding kernel, with userspace, libc and an init process.

**Branding** (defined once in `src/include/version.h`):
`Fr OS` (project) · `Fr Core` (kernel) · `Fr Boot` (bootloader) ·
`Fr Init` (init) · `Fr Libc` · `Fr Userland`.

**Tree**: `src/{boot,kernel,libc,userspace,include}`, `src/config.mk`,
`tools/`, `tests/`, `docs/`.

**Build**: `flock /tmp/fr-build.lock make clean >/dev/null 2>&1 && flock /tmp/fr-build.lock make -j8`.
A plain `make` silently keeps stale objects; `make check` integrates the full
flow (verify-isa-test → boot-smoke → test-harnesses → test-runtime).

**Boot**:

```
cp build/os.img /home/omena0/.fr-tmp/<you>.img
timeout -s TERM 25 qemu-system-x86_64 -cpu max \
  -drive file=/home/omena0/.fr-tmp/<you>.img,format=raw,if=ide,index=0,media=disk \
  -m 4G -smp 1 -machine pc,acpi=off -no-shutdown -device VGA -display none \
  -chardev file,id=s0,path=/home/omena0/.fr-tmp/<you>.log -serial chardev:s0
```

Add `-d int -D /home/omena0/.fr-tmp/<you>-int.log` for fault vectors, CR2 and
error codes. Use `HEADLESS=1` to keep QEMU off the display.

**Run**: `./run.sh` (foreground, for COM1/stdin) or `./run.sh --headless`.

---

## 2. Environment traps (each one cost real time)

- **`/tmp` hit its quota.** A 3.4 GB `-d exec` trace. Under that condition
  `printf x > /tmp/x` returns rc=1 leaving a **0-byte file**, and every QEMU
  whose serial log went to `/tmp` produced a 0-byte log while exiting rc=0 —
  which reads exactly like a regression. **This is how "0 exceptions" got
  reported from an empty log.** Use `/home/omena0/.fr-tmp/`.
- **`export TMPDIR=/home/omena0/.fr-tmp`** or gcc fails writing temp assembly.
- **`-serial chardev:s0` uses a colon.** `chardev=s0` gives a 0-byte log and a
  one-line QEMU error that is easy to miss.
- **Always `timeout -s TERM`, never the default SIGKILL** — SIGKILL loses
  QEMU's serial buffer and truncates the log.
- **`gdb` works only inside a single bounded shell command together with
  QEMU** (`qemu ... -S -s & sleep 4; gdb -q -batch -x s.gdb; kill %1`), with
  `file build/kernel.elf` as the first gdb line. Backgrounding them separately
  produces no output at all.
- **QMP needs `qmp_capabilities` executed first**, and `pmemsave`/`screendump`
  write to a **file**.
- **Rebuild before reading any measurement.** `make clean && make -j8` every
  time; the `.d` files were correct and make still got it wrong because it
  cannot see a dependency when a header mtime is *rewound* (rsync/tarball
  extraction). That is fixed, but the habit is not.

---

## 3. The method — how bugs were actually found

Of fourteen fixes attempted, eleven produced no diagnostic at all. Of the three
that did, two reported the wrong fault.

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

Corollary: **a write to somewhere valid says nothing.** Silent corruption looks
exactly like working code.

---

## 4. Settled facts nobody should re-derive

These are pinned by the compiler (`_Static_assert`) and/or measured. Re-deriving
them is how time is lost.

- **LRETQ pops RIP and CS — 16 bytes — and does not restore RFLAGS.** Only
  `IRET` pops flags. Confirmed by the Intel `RET` pseudocode and by measurement
  under both TCG and KVM. Three words pushed for `LRETQ` is the classic
  32-bit idiom (EFLAGS, CS, EIP) carried over unexamined.
- **`ljmp $cs, $label` in 64-bit mode assembles to `EA` with a 16-bit IP
  offset.** Works in the bootloader only because the target is in the low
  64 KiB; unusable for a kernel at `0xffffffff8...`.
- **IDT gate types in long mode:** `0x0E` is the 64-bit *interrupt* gate and
  clears IF; `0x0F` is the 64-bit *trap* gate and does not. There is no
  gate-size bit — width is the IDT entry's, not the type field's. `struct
  idt_entry` here is already the 16-byte form.
- **A clear U/S in any upper-level page-table entry overrides the leaf PTEs**
  and makes the whole translation supervisor-only. Every level needs it, and
  existing levels need promoting as well as creating.
- **GDT/TSS descriptors are `_Static_assert`-pinned.** When an instruction makes
  the boot work, check first whether it is removing a *correct* instruction —
  loading a selector into GS refreshes the cached descriptor (required) and in
  doing so replaces the hidden base with the descriptor's (zero, for a flat
  segment). Deleting the instruction "fixes" the symptom and breaks the kernel.
- **A zero-byte serial log means a bad command, a full filesystem, or a wrong
  path — not a healthy guest.** Check `df`, check the colon, check the path.

---

## 5. Mistakes made here — do not repeat them

Every one of these happened to me on this tree. Listed as **what I did** and
**what to do instead**.

### Chasing a debugger's opinion of control flow instead of the machine's

A gdb run showed a breakpoint hit twice with `finish` returning to the same
entry; I concluded a function re-entered itself infinitely. It never did. One
`od` of port-0xE9 markers (A–E printed exactly once per boot) settled it. gdb's
`finish` misbehaves across `lretq` — the caveat was written down and then
ignored.
→ **When a story says the CPU did something structurally absurd, check it with
the cheapest independent channel first.** Port markers, a serial line, `-d int`.
One `od` is worth an hour of gdb. And when you hand over a lead, hand over *how
to falsify it* first.

### Reading a disassembly listing as if it were the encoding

`objdump` printed `push 0xffffffff80010aa2` for `ff 34 25 a2 0a 01 80`. That
reads like "push the immediate" and is not: the opcode is `PUSH r/m`, so it
pushes the **contents** at that address. objdump prints the memory operand's
*target*, which is exactly the address a push-immediate would have named.
→ **For anything where an addressing mode and an immediate produce the same
listing line, read the opcode/ModRM, not the mnemonic.** Same trap with
`mov $imm` versus `mov` through a displacement.

### Deriving what code "should" produce, then "fixing" correct code

The descriptor layout. I was certain byte 7 was `base[31:28]` — four bits —
because I had "fixed" it that way once, then reasoned my way back. Both times I
was wrong; the original eight-bit form was right, and the compiled immediates in
`kernel.elf` were correct while I edited them. **Twice, in opposite directions,
on the same question.**
→ **Disassemble. Then let the compiler check you: `_Static_assert`.** The asserts
caught each error within seconds and never missed one.

### Reading an empty result as a result

`grep -c pattern empty-file` prints **nothing**. I read that as "0 exceptions"
and reported it. It was a 0-byte file. A missing value is not a value: if a
command that should print something prints nothing, find out why before
believing anything downstream.

### Believing a command ran when it did not

`-serial chardev=s0` instead of `chardev:s0` — QEMU rejected it, wrote a
0-byte log, and I read that as "the guest printed nothing". Wrapping the
`qemu-system_x86_64` invocation in `flock` swallowed the run entirely.
→ **Run the command bare, look at what it prints, and read the error output.**

### Timing as proof of position

Boot markers used `klog`, which buffers. They never appeared, so I concluded
the crash was earlier than it was and sent an agent hunting the wrong function.
Direct `serial_puts` does not buffer.
→ **Know your logging.** If you need to know *where* something died, write
straight to the device.

### Overstating what was done

An agent read four "done" claims and found all four **partly** true: callers
still tested an old sentinel, a comment still said the offset the assert had
rejected, and one path was never instrumented at all.
→ **State verified and unverified separately.** "I added an API" is not "callers
use it". If an agent audits your claims, it is doing you a favour.

### Broad edits near code you did not own

A range edit deleted the entire CR0.PE set **and** the `ljmp` mode switch from
`stage2_entry.S` — the build stayed green. A green build proves the assembler
accepted the file, not that the file still does what it did.
→ **`git diff` after every edit, and check `ownership.py` before you edit.** Run `python3 tools/ownership.py check <file>` to see if the file is free.**

### Editing with shell string replacement instead of the file tools

Fragile `python -c` / `sed` replacements silently did nothing, or clobbered
neighbours.
→ **Use Read/Edit.** They fail loudly on a mismatch. Shell rewriting is fine
for *bulk mechanical* changes across many files; it is wrong for surgical ones.

### Papering over instead of fixing

When KVM `#GP`'d on `WRMSR 0xC0000101` I changed `run.sh` to default away from
KVM. The real fix (`CR4.FSGSBASE` + `WRGSBASE`) was better *and* faster.
→ **A default that avoids the failure hides the failure.**

### Announcing conclusions before measuring

I repeatedly told the board something was fixed before re-running the
measurement. Twice the "verification" was an empty log.
→ **Measure, then say it. If you cannot measure, say *inferred*.**

---

## 6. Ownership and parallel work

File ownership is managed via `tools/ownership.py` (claim, release, view,
check, clear, prune) with flock locking and TTL expiry (3h). One claim per
file, claimed before dispatch. What makes parallel work safe:

- Split the file list so no two agents can touch the same file.
- Give each agent this STATE.md verbatim — without the environment traps, two of
  three agents burned budget on the same traps.
- **Report, do not touch**, for anything found outside your claimed scope. This
  is how the IDT gate error was caught — it would have been overwritten by
  whichever agent went second.
- **Release when you finish**; a stale claim blocks the next person. Run
  `python3 tools/ownership.py release <file>` before committing.
- **Check what you have claimed before releasing**; 8 times out of 10 an agent
  only released a small portion of their claimed files when they were done,
  leaving hanging claims and potentially blocking work for others.

Tell agents that a comment, a finding, a commit message and another agent's
summary are **claims**, and the built artefact is evidence. The one that caught
my error had checked a *constant* against the spec rather than against my text.

---

## 7. When to edit this file

Edit STATE.md whenever you discover anything that:

1. **Cost you time** (add it to §2, §4 or §5).
2. **Saves someone else time** (a trap, a measurement, a `how to falsify it`).
3. **Becomes settled** (moves from hypothesis in §4 to a fact, or a fact gets
   corrected).
4. **Concerns a commit whose message doesn't carry the mechanism and the
   verification.** Prefer commit messages that say what is **not** verified.

Do **not** keep here things that change every session: git branch state, commit
shorthands, which tests are passing, which issue is currently the blocker, or
which agents hold claims. Those are captured in `git log`, `make check` output,
`git status` and `tools/ownership.py` respectively — stale copies of them do more
harm than good.

---

## 8. External tools

### `docsearch`

A UV-packaged BAAI/bge-small-en-v1.5 embedding-based semantic search utility for
local Markdown docs. Installs to `~/.local/bin/docsearch`. CLI:
`docsearch [OPTIONS] [COMMAND] [ARGS]`. Commands: `index <path>`, `rebuild
<path>`, `search <query>`, `info`. Options: `-i/--index`, `--device
[auto|cpu|cuda|mps]`, `--batch-size`, `--cache-dir`, `-v`. Direct query mode
works: `docsearch "query"`. `info` prints index metadata.

It *should* be on PATH; if not, run with the full path
`/home/omena0/.local/bin/docsearch`.
