
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

## References

- [Intel 64 and IA-32 Architectures Software Developer's Manual][intel-sdm]
- [AMD64 Architecture Programmer's Manual][amd-apm]
- [POSIX.1-2017 (IEEE Std 1003.1-2017)][posix-2017]
- [System V Application Binary Interface AMD64 Architecture Processor Supplement][sysv-abi]

[intel-sdm]: https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html "Intel 64 and IA-32 Architectures Software Developer's Manual"
[amd-apm]: https://www.amd.com/en/developer/architecture-programmer-manuals.html "AMD64 Architecture Programmer's Manual"
[posix-2017]: https://pubs.opengroup.org/onlinepubs/9699919799/ "The Open Group Base Specifications Issue 7, 2018 edition (POSIX.1-2017)"
[sysv-abi]: https://gitlab.com/x86-psABIs/x86-64-ABI/-/blob/master/abi.md "System V AMD64 ABI"

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

## Documentation

`docs/src/` holds the design documentation. Unless a page explicitly states that
something is implemented, **treat it as a goal or intended behaviour**, not the
current state. Many subsystems documented here do not yet exist in code.

For the verified current state, see [`STATE.md`](STATE.md) and
[`MEGA_AUDIT.md`](MEGA_AUDIT.md).

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
MEGA_AUDIT.md     source audit with a per-finding status
```

## Licence

See `docs/config.json`. The copyright holder is Omena0.
