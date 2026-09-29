# Serial Diagnostics

## Overview

Serial port output is the most reliable diagnostic channel available — it works before the framebuffer is initialized, persists through kernel panics, and requires no kernel heap or driver framework. QEMU routes the serial port to the host terminal by default.

## UART Initialization

The kernel initializes COM1 (I/O port 0x3F8) during the earliest phase of boot (before paging is enabled in the stage-2 bootloader):

```nasm
; 16-bit mode UART init (38400 baud, 8N1)
uart_init:
    mov dx, 0x3F9
    mov al, 0x00        ; Disable all interrupts
    out dx, al

    mov dx, 0x3FB
    mov al, 0x80        ; Enable DLAB (set baud rate divisor)
    out dx, al

    mov dx, 0x3F8
    mov al, 0x03        ; Divisor low byte: 3 → 38400 baud
    out dx, al

    mov dx, 0x3F9
    mov al, 0x00        ; Divisor high byte
    out dx, al

    mov dx, 0x3FB
    mov al, 0x03        ; 8 bits, no parity, one stop bit
    out dx, al

    mov dx, 0x3FC
    mov al, 0x03        ; RTS/DSR
    out dx, al
    ret
```

## Polled Output

The kernel uses **polled output** (no interrupts) for diagnostic writes. This is safe even in interrupt context and during panics:

```c
static void uart_write_byte(uint8_t byte) {
    while (!(inb(0x3F8 + 5) & 0x20)); // wait for THRE (transmit holding register empty)
    outb(0x3F8, byte);
}

void serial_write(const char *str) {
    while (*str) {
        if (*str == '\n') uart_write_byte('\r');
        uart_write_byte(*str++);
    }
}
```

## Structured Diagnostic Messages

Serial output uses a compact prefix to aid filtering:

```
[BOOT] Stage 1: MBR loaded
[BOOT] Stage 2: entering protected mode
[KRNL] INFO  kernel: KASLR base = 0xffff8000_deadbeef
[KRNL] WARN  mm: 89% of RAM in use
[KRNL] ERR   drv: NVMe: timeout
[KRNL] PANIC mm: null pointer dereference at 0x0000000000000018
```

Format: `[tag] level  subsystem: message\r\n`.

## Panic Output

On kernel panic, the panic handler emits:
1. `[KRNL] PANIC` message with a brief description.
2. Register dump: `RAX=...`, `RBX=...`, etc.
3. Stack backtrace (using frame pointer unwinding):
   ```
   [KRNL] PANIC kernel: assertion failed: ptr != NULL
   RAX=0000000000000000 RBX=ffff800012345678 RCX=0000000000000001
   ...
   Backtrace:
     [0] 0xffff800000abcdef  slab_alloc+0x4f
     [1] 0xffff800000123456  kmalloc+0x12
     [2] 0xffff800000fedcba  net_alloc_skb+0x28
     ...
   ```

## QEMU Serial Setup

In QEMU, serial output is configured in the run command:

```sh
qemu-system-x86_64 \
    ...
    -serial stdio \
    ...
```

This routes COM1 output to the QEMU standard output — visible in the terminal.

Alternatively, log to a file:
```sh
-serial file:/tmp/serial.log
```

## Related Documents

- [overview.md](overview.md)
- [event-logging.md](event-logging.md)
- [kernel/logging.md](../kernel/logging.md)
- [kernel/panic-system.md](../kernel/panic-system.md)
- [build/qemu-setup.md](../build/qemu-setup.md)
