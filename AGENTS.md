# AGENTS.md - Fr OS

Read this before touching the tree. `STATE.md` is older history from earlier
sessions; its hardware facts and environment traps still hold (summarised below),
but its status and ownership sections are stale. For current status, read
`TODO.md` and `git log`. Update: `STATE.md` has been deprecated and deleted
in favor of this very document.

Please **DO** update this file if there is anything helpful you feel like
you would've wanted to know when you started on this tree. And dont just
append to the file, edit it properly. Don't let it get cluttered.

## What this is

Fr OS is an x86-64 OS for QEMU, written from scratch in C11 and x86-64 assembly.
The project has six named parts; the names are macros in `src/include/version.h`:

| Name | Where | What |
|---|---|---|
| Fr OS | - | The project |
| Fr Core | `src/kernel` | The kernel. Its banner and panics say Fr Core, not Fr OS |
| Fr Boot | `src/boot` | Stage 1 (MBR) and stage 2 (into long mode) |
| Fr Libc | `src/libc` | C runtime shared by userland |
| Fr Init | `src/userspace/init` | First process; REPL, runs other programs |
| Fr Userland | `src/userspace/*` | hello, paint, snake, tetris, triangle, ansi |

- `prompt.txt` is the original, aspirational spec. It is not a description of
  current state. Do not edit it.
- `TODO.md` is the working list of missing features, in priority order (P0, P1,
  P2). It is the source of truth for what to build next. Its recommended order
  is Signals, then Termios, then core filesystem operations, then wait semantics.
- `docs/src/` holds the design docs (published by `.github/workflows/pages.yml`).
  The spec asks for documentation under `docs/`.
- **Known doc error:** the syscall numbers in `docs/src/syscalls/*.md` do not
  match `src/include/uapi/syscall.h`. The header is what the kernel dispatches.
  Trust the header and fix the doc when you touch it.

## Repo map

- `src/include/uapi/` - the userspace ABI, shared by kernel and libc:
  `syscall.h` (numbers and structs), `signal.h`, `sysinfo.h`, `errno.h`, ...
- `src/kernel/` - `arch/` (asm entry, IDT, GDT), `proc/` (syscalls, process,
  fd, signals), `sched/` (scheduler, tasks), `mm/` (pmm, vmm, kmalloc),
  `console/` (tty, console, framebuffer), `fs/` (FrFS), `drivers/` (including
  `ata.c`), `core/` (klog, panic, kprintf), `include/` (kernel headers),
  `link.ld` (image layout and budget).
- `filesystem/` is the root filesystem source tree, packed by `tools/mkfs.py`
  into `build/fs.img` and attached as the second IDE disk. The build installs
  its programs into `filesystem/bin/`; files added there by hand are packed and
  never deleted.
- `src/libc/src/*.c` is collected automatically; public headers are in
  `src/libc/include/`.
- `src/userspace/<name>/` - userland programs, packed into the initrd by
  `tools/initrd.py` and installed in `filesystem/bin/`. Each `.c` file under a
  directory (except `src/userspace/init/`) is its own program, named after the
  file: `paint/paint_gui.c` builds `paint_gui`. Adding a file is enough; the
  Makefile generates its link rule and both program lists from that wildcard.
- `tests/runtime/framework/` - in-OS test framework. Suites are in
  `tests/runtime/kernel/*.c` and `tests/runtime/init/`.
- `tests/*.py`, `tests/*.sh`, `tests/*.c` - host-side harnesses: `boot_smoke.py`
  (boots the image and asserts milestones), context-switch harness, TLS harness,
  schedule-resume check.
- `tools/` - `initrd.py`, `disk.py`, `verify_isa.py` (ISA gate: kernel must be
  GPR-only), `ownership.py` (file claims), `detect_kbd_layout.sh`, `psf2c.py`,
  `bin2c.py`.
- `run.sh` - interactive QEMU launcher.
- Generated and junk files: `build/` is gitignored. **18 generated or backup
  files are tracked in git** (`*.c.d`, `*.S.d`, `*.bak`, `*.debug`, for example
  `src/libc/src/crt1.c.d`, `tests/runtime/framework/test_runner.c.debug`). Never
  add new ones, and never stage them. Cleaning them up is its own commit, and
  only when asked.

## Build, run, test

```
make            # build/os.img and build/fs.img
make kernel     # kernel ELF only
make run        # boot interactively under QEMU (or ./run.sh)
make clean      # remove build/
make check      # the full gate; see below
make help       # every target and the boot-smoke environment variables
make frscript-smoke  # compile frscript's fr-os hello, boot it, run it
```

`make check` runs, in order: `verify-isa-test`, `boot-smoke`, `test-harnesses`
and `test-runtime`, and finishes with `all checks passed`. It is the gate. Before
you claim a kernel change works, run it and read the output.

- `.github/workflows/test.yml` runs `make check` on `ubuntu-latest` on pushes to
  `main` and on pull requests. It does not guarantee `/dev/kvm`, so check for a
  SKIPPED line in its log. Update: It will use TCG because kvm isn't available on
  GitHub actions runners.

- `test-runtime` boots the OS with `build/test-runner` as the initrd program.
  The suites print `=== Suite ... ===` and a `Tests:` summary on the console.
- **Per-test `[FAIL]` lines are not in `make check` output.** They are in the
  serial log the harness writes: `build/boot-smoke/serial-<pid>.log`. Grep there
  first when a suite reports failures.
- `boot-smoke` needs `/dev/kvm`. Without it the harness can skip (exit 77).
  Skips are reported as `check: the boot smoke test was SKIPPED`. A green
  `make check` with a SKIPPED line did not exercise the boot path. Set
  `BOOT_SMOKE_ALLOW_TCG=1` to run under emulation.
- `python3 tests/boot_smoke.py --self-test` tests the boot-log classifier.
- `make frscript-smoke` builds `examples/fr-os/hello.fr` with the frscript
  compiler (a separate checkout, `FRSCRIPT_DIR`, default `../frscript`), checks
  it with the user ISA gate, and boots it with `--run-command frhello`. It is
  not part of `make check` because frscript is external. `--run-command` works
  for any program that exits; `triangle` is the exception, which waits for its
  frame marker.
- There is no separate lint or typecheck target. Compiler flags include
  `-Wall -Wextra -Werror=implicit-function-declaration -Werror=return-type`,
  so the build is the typecheck. `console.c` already emits an unused-variable
  warning; leave it unless you are touching that code.

### Build and scratch environment

- **Clean build before you measure anything.** Make cannot see some header
  dependencies (mtime rewinds from tar or rsync), so stale objects happen. Use
  `make clean && make -j8`.
- When other agents share the tree, serialise builds:
  `flock /tmp/fr-build.lock make clean && flock /tmp/fr-build.lock make -j8`.
- Scratch directories: `TMPDIR` and `FR_TMPDIR` select them. If both are unset,
  the harnesses use `build/tmp`. Do not point them at a small `/tmp`. A full
  filesystem gives a 0-byte serial log and a zero exit code, which reads as a
  healthy guest.
- `KBD_LAYOUT` (`fi` or `us`) is chosen at build time from the host keymap
  (`tools/detect_kbd_layout.sh`). Override with `make KBD_LAYOUT=us`. Changing it
  rebuilds everything.

### QEMU and debugging traps

- Always stop QEMU with `timeout -s TERM`. `SIGKILL` loses the serial buffer and
  truncates the log.
- The serial chardev syntax has a colon: `-serial chardev:s0`. `chardev=s0`
  gives a 0-byte log and a one-line error that is easy to miss.
- `run.sh` needs `mux=on` for stdin to reach the guest.
- Run gdb and QEMU in one bounded shell command (`qemu ... -S -s & sleep 4; gdb
  ...; kill %1`). Backgrounding them separately gives no output.
- A breakpoint on a standalone symbol does not prove a call did not happen: the
  compiler inlines `pic_eoi` into `interrupt_dispatch`.
- Print the status byte an interrupt handler reads. Do not infer device state
  from the fact that the IRQ fired.

## Working rules for this repo

- **Commit only when asked.** When the user asks for features, each feature gets
  its own commit. No "misc" commits. Stage explicit paths with `git add <files>`,
  never `git add -A`, because the tree can contain untracked files you did not
  create (for example `.github/workflows/*`).
- Commit subjects are `subsystem: lowercase imperative summary`, for example
  `fd:`, `sys:`, `signal:`, `syscall:`, `console:`, `tests:`, `input:`. The body
  says what the change does, how it works, and what was **not** verified.
- **TODO.md moves with the work.** When a commit implements part of a TODO row,
  update that row in the same commit. Remove the row when everything in it is
  done. Do not leave finished items listed.
- Before you edit a shared file in a multi-agent session, run
  `python3 tools/ownership.py check <file> --agent <name>`. Claim it with `claim`
  and release it with `release` when you finish. Claims expire after 3 hours.
  `view` lists current claims. A stale claim blocks other work, so release yours.
- Report things you find outside your scope. Do not silently fix them.
- Do not hide failures with defaults that avoid the bug. Find the cause.
- Say what you verified and what you did not. "Built" and "booted" and "tested"
  are three different claims.

## Kernel conventions

**Syscall ABI** (`src/include/uapi/syscall.h`). Numbers are grouped by domain and
never reused: process 0-31, memory 32-63, file 64-127, IPC 128-159, socket
160-199, scheduling 200-231, security 256-287. `SYSCALL_MAX` is 288. An unknown
or unimplemented number returns `-ENOSYS` and logs a WARN with the caller's pid.

To add a syscall:

1. Add the number to `uapi/syscall.h`, and any structs or flags to a uapi header.
2. Write the kernel implementation (in `src/kernel/proc/` or a subsystem file).
3. Add an `sc_*` wrapper in `src/kernel/proc/syscall.c` and register it in
   `syscall_init`.
4. Add the libc `sys_*` stub in `src/libc/src/syscall.c` (it uses `__syscallN`
   and `check0`), then the public API in a libc source and its header.
5. Add cases to a suite in `tests/runtime/kernel/`. **Update that suite's
   `.num_cases` to match the table length.** The framework iterates `num_cases`
   (`tests/runtime/framework/test_framework.c`), so a wrong value silently drops
   or misreads cases.

**Return values.** The kernel returns `-errno`. libc's `check0` sets errno and
returns -1.

**User pointers.** Never dereference them. Use `copy_from_user` and
`copy_to_user`, which return negative on a fault. Validate every pointer and
argument before you change any state, so a failed copy leaves the task
unchanged.

**Per-task state.** Lives in `struct task` (`src/kernel/include/task.h`) and is
zeroed by `task_alloc`. For each field, decide three things: what `task_alloc`
sets, whether `task_inherit` copies it for fork and clone, and whether `execve`
resets it. The fd table and fd flags, rlimits and signal dispositions all follow
this pattern.

**Image budget.** `KERNEL_MAX_BYTES = 0x1D0000` in `src/kernel/link.ld` is a
link-time assert, not a guideline. Keep roughly 16 KiB of headroom. Do not add
large static per-CPU arrays. A 1.27 MiB magazine array was once the cause.

**Logging.** Use `klog` with a `KLOG_SUBSYSTEM` name per file. Levels run from
DEBUG to FATAL. Do not log from interrupt handlers: a full input ring must not
recurse into the console. For boot-critical messages write to serial directly,
because `klog` buffers.

**Fragile asm.** `src/kernel/arch/syscall_entry.S` and `interrupt_entry.S` depend
on exact frame offsets, which must match `struct syscall_regs` and
`struct interrupt_frame`. Read the header comments before you change either the
asm or the struct. The compiler and the `_Static_assert`s check some of these
offsets, not all of them.

## Hardware and architecture facts (verified in earlier sessions)

- `LRETQ` pops RIP and CS and not RFLAGS. Only `IRET` pops flags.
- Long-mode IDT gates: `0x0E` is an interrupt gate (clears IF), `0x0F` a trap
  gate (does not). There is no size bit.
- A clear U/S bit in any upper-level page-table entry makes the whole
  translation supervisor-only. Every level needs it.
- GDT and TSS descriptors are pinned by `_Static_assert`. Loading a selector into
  GS refreshes the cached descriptor and replaces the hidden base. Do not remove
  that instruction to "fix" a symptom.
- 8042 status bit 2 is the self-test flag, set after POST. Auxiliary (mouse) data
  is bit 5 (`0x20`).
- The PS/2 keyboard has no protocol for its layout, so the scancode tables are
  compiled in.
- The syscall return path reads the user RSP from the struct, and RIP and RFLAGS
  from stack copies. `ret_to_user` does not restore callee-saved registers, so a
  forked child that resumes compiled C code there gets kernel residue.
- Each process gets a per-CPU slot through `RDMSR` of the hidden GS base. Do not
  use `SWAPGS` without establishing the invariant first.

## Current gaps (verified on this tree)

- The root filesystem is FrFS on the second IDE disk, mounted read-only. `open`,
  `read`, `lseek`, `fstat`, `getdents` and `execve` work; open for write and
  `write` return `EROFS`. `stat`, `access`, `mkdir`, `unlink`, `rename`, `chdir`
  and `getcwd` have no syscall numbers. ext4 and virtio-blk (the spec's target)
  are not implemented; the storage driver is ATA PIO.
- Sockets: only `SYS_socket` (160) exists. Documented numbers 161-168 are not
  implemented and return `-ENOSYS`.
- `sc_getdents` always returns `-ENOTDIR`.
- The console ioctl handler supports `TIOCGWINSZ` and `TCGETS`. `TCGETS`
  returns a fixed struct and there is no `TCSETS`. The tty has no line
  discipline: input is raw bytes, and echo is userspace's job.
- `sc_kill` calls `task_exit_current(-sig)` for `SIGKILL`, so it exits the
  **caller**, not the target. Signal delivery is not implemented. The
  `process_deliver_signal` hook is weak and terminates the process.
- `setrlimit` and `getrlimit` store limits, but nothing enforces them.
- The guest's serial output occasionally repeats one character (seen as
  `prrogram`, `exiteed`, `Helllo, World!`). It is intermittent, appears in libc
  `printf` output, and the boot-smoke command matcher tolerates it by collapsing
  repeated characters. The cause is not yet diagnosed.

For what is being built right now, see `TODO.md` and `git log`.

## Environment traps from earlier sessions

- `/tmp` has hit its quota before. A full filesystem makes `printf > file`
  return rc=1 and leave a 0-byte file. A 0-byte log from a QEMU run is not a
  result. Check `df` and the path first.
- `export TMPDIR=...` before running gcc, or it can fail writing assembly temp
  files.
- A missing value is not a value. If a command that should print something
  prints nothing, find out why before you rely on it. `grep -c` on an empty
  file prints nothing.
- Run a command bare first and read its error output before you reason about
  what it did.
- Do not announce a fix before measuring it. If you cannot measure, say
  "inferred".
- When a child crashes at the same source line its parent just ran, suspect the
  register context the kernel built for the child, not the arguments.
- When a disassembly line is ambiguous, read the opcode and ModRM bytes.
  `push 0xffff...` in objdump output can be `PUSH r/m` of memory contents.
- Derive nothing you can check. `_Static_assert` and disassembly beat reasoning
  about the expected layout.
