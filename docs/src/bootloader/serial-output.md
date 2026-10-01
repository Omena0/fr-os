# Bootloader Serial Output

## Purpose

The bootloader emits diagnostic messages to the serial port (COM1, `0x3F8`) during the boot process. This output is visible in QEMU via `-serial stdio` or `-serial file:boot.log` and is essential for debugging boot failures before the kernel's own logging is active.

## Serial Port Initialization

Stage 2 initializes COM1 before emitting any output:

```
Baud rate: see below — it is not the same on every stage
Data bits: 8
Stop bits: 1
Parity:    None
FIFO:      Enabled (trigger level 14 bytes)

| Component | Divisor | Baud |
|---|---|---|
| Fr Boot stage 1 | 1 | 115200 |
| Fr Boot stage 2 | 3 | 38400 |
| Fr Core serial driver | 1 | 115200 |

All three drive COM1 on the same wire at three different speeds, so a real serial
capture is a mixture of two baud rates. QEMU's `-chardev file` ignores the rate
entirely, which is why nobody has noticed. Fixing it means picking one rate and
using it everywhere; the kernel should keep 115200 because that is what stage 1
already established.
```

Initialization sequence (direct port I/O):
1. Disable interrupts on UART (`IER = 0x00`).
2. Set baud rate divisor (`DLAB=1`, DLL=1, DLH=0` for 115200 at 1.8432 MHz clock).
3. Set 8N1 line control (`LCR = 0x03`).
4. Enable FIFO (`FCR = 0xC7`).
5. Set modem control (`MCR = 0x0B`).
6. Clear DLAB.

## Output Format

Each message is prefixed with a stage identifier:

```
[BOOT1] Stage 1 loaded, jumping to stage 2
[BOOT2] A20 enabled
[BOOT2] E820 map: 6 entries
[BOOT2] Entering protected mode
[BOOT2] Entering long mode
[BOOT2] Kernel ELF loaded at phys 0x0010_0000, entry 0xFFFF_FFFF_8000_0000
[BOOT2] Handing off to kernel
```

## Error Output

Boot errors emit a descriptive message and halt:

```
[BOOT1] ERROR: disk read failed (status=0x01)
[BOOT2] ERROR: A20 enable failed
[BOOT2] ERROR: ELF magic mismatch
[BOOT2] ERROR: kernel segment load out of bounds
```

## Relationship to Kernel Logging

Once `kernel_main` is entered, the bootloader serial output path is no longer used. The kernel initializes its own serial logging subsystem (`klog_serial_init`) independently. There is no shared state between the two serial output implementations.

The kernel serial logger supports log levels (DEBUG, INFO, WARN, ERROR, PANIC) and timestamps. See [kernel/logging.md](../kernel/logging.md).

## QEMU Configuration

```bash
# View serial output on stdout:
qemu-system-x86_64 -serial stdio ...

# Save serial output to a file:
qemu-system-x86_64 -serial file:serial.log ...

# Use virtual serial port accessible via telnet:
qemu-system-x86_64 -serial tcp::4444,server,nowait ...
```

## Related Documents

- [overview.md](overview.md)
- [stage2.md](stage2.md)
- [kernel/logging.md](../kernel/logging.md)
- [build/qemu-setup.md](../build/qemu-setup.md)
