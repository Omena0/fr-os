
# Fr OS

A x86-64 operating system for QEMU: a bootloader, a kernel, a C runtime and
a userspace, written from scratch in C11 and x86-64 assembly.

**Fr OS** is the project. It is assembled from six named components, and the names
are macros in [`src/include/version.h`](src/include/version.h) so that no two
banners can ever disagree:

| Component | What it is |
|---|---|
| **Fr OS** | The project as a whole |
| **Fr Core** | The kernel |
| **Fr Boot** | The bootloader — stage 1 in the MBR, stage 2 into long mode — which hands control to Fr Core |
| **Fr Init** | The first userspace process, and system bring-up |
| **Fr Libc** | The C runtime shared by the kernel and Fr Userland |
| **Fr Userland** | The programs that run on top of Fr Init |

Fr OS and Fr Core are not the same thing, and the distinction is load-bearing. A
panic that says "Fr OS" is claiming the entire system is down when in fact one
component failed; a panic that says "Fr Core" is claiming the kernel failed while
the bootloader and the firmware are still fine. The kernel's banner and its panic
path therefore print **Fr Core**, not Fr OS. `KERNEL_VERSION_STRING` is
`FR_CORE_NAME " " KERNEL_VERSION`.

## Build and run

```sh
make            # or: make clean && make -j8
./run.sh
```

`make clean` matters. The dependency tracking in this tree has, on more than one
occasion, failed to rebuild an object after a header edit; a stale `stage2.c.o`
once presented as a memory-management bug for hours. Always clean before you
believe a build.

To boot by hand and capture a serial log:

```sh
make clean && make -j8
timeout -s TERM 25 qemu-system-x86_64 -cpu max \
  -drive file=build/os.img,format=raw,if=ide,index=0,media=disk \
  -m 4G -smp 1 -machine pc,acpi=off -display none -no-reboot \
  -chardev file,id=s0,path=/tmp/boot.log -serial chardev:s0
```

Use `-s TERM`, never the default `SIGKILL`: killing QEMU loses the serial buffer
and produces a truncated log that looks exactly like a hang.

The serial port is the primary observation channel. It is not a convenience.

## Current state

The kernel boots. This is a real, verified statement, not an aspiration — see
[`MEGA_AUDIT.md`](MEGA_AUDIT.md) for the boot log that establishes it and for the
status of every one of the 177 findings in a full source-tree audit.

A boot gets as far as this:

```
[boot2] running at 0x0000000000008000, boot drive 0x0000000000000080
[boot2] A20 enabled via port 0x92 fast gate
[boot2] E820 memory map: ... 7 entries ...
[boot2]   kernel loaded, 273483 bytes
[boot2] entering long mode, jumping to kernel at ffffffff80000180
abcdefg[    0.000 cpu0 percpu/F] percpu: ...
[    0.000 cpu0 vmm/I] vmm: direct map 0-4 GiB, 2 MiB pages
[    0.000 cpu0 vmm/I] vmm: kernel window ffffffff80000000-ffffffff81000000 -> phys 100000-1100000, 4 KiB pages
[    0.000 cpu0 pmm/I] pmm: 1039018 usable frames (4058 MiB) of 1310720 tracked (5120 MiB)
Fr Core 0.1.0 (Oct  1 2026 23:32:17, rev unknown)
boot: entry 0xffffffff80000180, image 0x0000000000100000, drive 0x80, cmdline '(none)'
memory: 4058 MiB total, 4057 MiB free, 7 E820 entries
```

The `abcdefg` and the `0.000` timestamps are both real, both known, and both
listed in the audit. **The system does not yet reach userspace.** Fr Init cannot
start, so nothing after this point has been exercised. The audit's "what actually
blocks the system now" section lists, in order: the initrd container being handed
to the ELF loader instead of an ELF; the syscall entry path; the absence of a
timer; `build_missing_tables()` being unable to create a PDPT; and the absence of
any TLS setup.

## Documentation

`docs/src/` holds the design documentation. It is a mixture of implemented
behaviour, design intent, and — for a number of subsystems — pure fiction. Read
the status line on any page that has one before you write code against it.

Start with:

- [docs/src/README.md](docs/src/README.md) — the documentation index
- [docs/src/architecture/memory-layout.md](docs/src/architecture/memory-layout.md)
  — what the address space actually looks like, with every constant traceable
- [docs/src/bootloader/stage2.md](docs/src/bootloader/stage2.md) — how control
  reaches the kernel
- [docs/src/build/overview.md](docs/src/build/overview.md) — toolchain and make

## Repository layout

```
src/boot/         Fr Boot — stage1.S, stage2.c, stage2_entry.S, stage2_long.S
src/kernel/       Fr Core — vmm.c, pmm.c, mm.c, kmalloc.c, sched.c, syscall.c,
                  process.c, elf.c, idt.c, gdt.c, panic.c, console.c, tty.c
src/libc/         Fr Libc
src/userspace/    Fr Init and Fr Userland
src/include/      Shared headers, including version.h (the branding macros)
tools/            disk.py, initrd.py, bin2c.py, psf2c.py — the build helpers
docs/src/         Documentation
MEGA_AUDIT.md     177-finding source audit with a per-finding status
```

## Licence

See `config.json`. The copyright holder field has never been filled in.
