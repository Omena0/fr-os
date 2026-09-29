# Input Driver

## Overview

The input driver handles keyboard and mouse (pointer) input from hardware devices, normalizes events into a device-independent format, and delivers them to the GUI and application processes via event queues.

## Supported Input Sources

| Device | Interface | QEMU Device |
|---|---|---|
| Keyboard | PS/2 (I/O ports 0x60/0x64) | `ps2` (default) |
| Mouse | PS/2 (I/O port 0x60) | `ps2` (default) |
| USB keyboard | USB HID (via future USB stack) | `usb-kbd` (planned) |
| USB mouse | USB HID | `usb-mouse` (planned) |

In QEMU, PS/2 keyboard and mouse are available by default with no additional arguments.

## Kernel-Side Input Core

The kernel handles the PS/2 interrupt (IRQ 1 for keyboard, IRQ 12 for mouse):

**Keyboard ISR** (IRQ 1):

1. Read scancode from I/O port 0x60.
2. Handle multi-byte scancode sequences (e.g., extended scancodes with 0xE0 prefix).
3. Translate scancode to a logical key code (using a built-in scancode→keycode table).
4. Write a `struct input_event` to the kernel input event ring buffer.
5. EOI to LAPIC.
6. Increment `irq_count` in the shared region to wake the userspace input driver.

**Mouse ISR** (IRQ 12):

1. Read 3 bytes from I/O port 0x60 (button state + X delta + Y delta).
2. Write a `struct input_event` of type `INPUT_EV_MOUSE_REL` to the ring buffer.
3. EOI.

## Input Event Format

```c
#define INPUT_EV_KEY        1   // keyboard key press/release
#define INPUT_EV_MOUSE_REL  2   // relative mouse movement
#define INPUT_EV_MOUSE_BTN  3   // mouse button press/release

struct input_event {
    uint32_t type;
    uint64_t timestamp_ns;
    union {
        struct { uint16_t keycode; uint8_t state; } key;      // state: 0=up, 1=down
        struct { int16_t dx, dy; int8_t dz; } mouse_rel;     // relative movement
        struct { uint8_t button; uint8_t state; } mouse_btn;
    };
};
```

## Userspace Input Driver

The userspace input driver process (`/usr/lib/drivers/input_drv`):

1. Receives the shared region fd from `DRIVER_INIT`.
2. Maps the shared region. Enters the event loop.
3. On `irq_count` increment: drains the kernel input event ring buffer.
4. Translates keycodes to Unicode codepoints (using the active keyboard layout table).
5. Dispatches events to the GUI event queue (via a Unix domain socket or shared memory to the display server).

## Keyboard Layout

Layout tables are loaded from `/usr/share/keymaps/<layout>.map` at driver startup. The default layout is US-QWERTY. The driver supports modifier key tracking (Shift, Ctrl, Alt, AltGr, CapsLock, NumLock) to produce the correct Unicode output.

## Event Queue to Applications

The GUI/display server distributes input events to the focused window's owning process via its input event fd (a Unix domain socket or an eventfd). Applications call `epoll_wait` on this fd to receive input events without polling.

## Related Documents

- [overview.md](overview.md)
- [hybrid-architecture.md](hybrid-architecture.md)
- [kernel/interrupt-handling.md](../kernel/interrupt-handling.md)
- [userspace/gui-architecture.md](../userspace/gui-architecture.md)
