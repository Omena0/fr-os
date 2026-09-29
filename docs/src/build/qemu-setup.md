# QEMU Setup

## Standard Run Command

```sh
qemu-system-x86_64 \
    -enable-kvm \
    -cpu host \
    -smp 18 \
    -m 4G \
    -drive format=raw,file=build/disk.img \
    -serial stdio \
    -display gtk \
    -no-reboot \
    -no-shutdown
```

## Flag Reference

| Flag | Effect |
|---|---|
| `-enable-kvm` | Use Linux KVM hardware virtualization (faster; requires `/dev/kvm`) |
| `-cpu host` | Expose host CPU model to guest (enables SSE4.2, AVX, etc.) |
| `-smp 18` | 18 virtual CPUs |
| `-m 4G` | 4 GB RAM |
| `-drive format=raw,file=build/disk.img` | Use raw disk image |
| `-serial stdio` | Route COM1 serial to host stdout (see serial diagnostics) |
| `-display gtk` | Open a GTK window for the framebuffer display |
| `-no-reboot` | Exit QEMU instead of rebooting on kernel panic |
| `-no-shutdown` | Do not power off on `sys_reboot` with power-off command (for debugging) |

## GDB Debugging

Launch QEMU with GDB support:
```sh
qemu-system-x86_64 \
    -enable-kvm \
    -cpu host \
    -smp 18 \
    -m 4G \
    -drive format=raw,file=build/disk.img \
    -serial stdio \
    -s -S
```

- `-s`: shorthand for `-gdb tcp::1234` (GDB listens on port 1234).
- `-S`: freeze execution at startup until GDB connects.

Connect with GDB:
```sh
gdb build/kernel.elf
(gdb) target remote :1234
(gdb) continue
```

## ISO Boot

To boot from a CD-ROM image:
```sh
qemu-system-x86_64 \
    -enable-kvm \
    -cpu host \
    -smp 18 \
    -m 4G \
    -cdrom build/os.iso \
    -boot d \
    -serial stdio
```

## Without KVM (Portable)

On systems without KVM (or on macOS/Windows with other hypervisors):
```sh
qemu-system-x86_64 \
    -cpu qemu64 \
    -smp 4 \
    -m 2G \
    -drive format=raw,file=build/disk.img \
    -serial stdio
```

Remove `-enable-kvm` and `-cpu host`. Performance will be lower.

## Debugging Tips

- **Serial output**: kernel log appears in the terminal where QEMU is launched.
- **QEMU monitor**: press `Ctrl+Alt+2` in the GTK window to access the QEMU monitor (`info registers`, `x /10i $rip`, etc.).
- **Core dumps**: QEMU can write a core dump of the guest memory on panic with `-dump-vmstate`.

## Related Documents

- [overview.md](overview.md)
- [toolchain.md](toolchain.md)
- [makefile-structure.md](makefile-structure.md)
- [debugging/serial-diagnostics.md](../debugging/serial-diagnostics.md)
