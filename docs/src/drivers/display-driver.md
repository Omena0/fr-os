# Display Driver

## Overview

The display driver provides a framebuffer abstraction to the rest of the OS. All graphical output goes through the framebuffer — the GUI system writes pixels into the framebuffer, and the display driver outputs those pixels to the physical (or virtual) display.

## QEMU Environment

In QEMU, the display device is a standard VGA/VESA-compatible device or `virtio-gpu`. The initial framebuffer is set up by the bootloader (stage 2) using the VESA BIOS extension (VBE), and its parameters (address, width, height, bytes-per-pixel) are passed to the kernel in the `BootInfo` struct.

For `virtio-gpu` (preferred for QEMU): the display driver interacts with the virtio-gpu device via virtio queues to set display modes and update the display surface.

## Kernel-Side Display Core

The kernel maintains a minimal framebuffer abstraction:

```c
struct framebuffer {
    uint8_t   *virt_addr;     // kernel virtual address of framebuffer memory
    uint64_t   phys_addr;     // physical address
    uint32_t   width, height; // display resolution in pixels
    uint32_t   pitch;         // bytes per row
    uint8_t    bpp;           // bits per pixel (32 typical)
    uint32_t   format;        // pixel format (RGB888, ARGB8888, ...)
};

extern struct framebuffer kfb;  // global kernel framebuffer (set at boot)
```

The kernel framebuffer is used directly during early boot (before the userspace driver starts) for log output.

## Userspace Display Driver

The userspace display driver process (`/usr/lib/drivers/display_drv`):

1. Receives the display parameters from the kernel via `DRIVER_INIT`.
2. mmap's the framebuffer physical memory into its address space.
3. Accepts `DISP_CMD_FLIP` commands from the kernel (triggered by the GUI system).
4. On flip: copies the backbuffer (provided by the GUI) into the physical framebuffer.
5. For `virtio-gpu`: sends a virtio GPU update command to flush the display surface.
6. Sends `DISP_RSP_VSYNC` back to the kernel when the flip is complete.

## Double Buffering

The GUI system uses a double-buffer model:

- **Front buffer**: The currently displayed image (mapped in the display driver's address space).
- **Back buffer**: The next frame being composed by the GUI (mapped in the GUI process's address space).

On `DISP_CMD_FLIP`: the kernel atomically swaps the VMA mappings (remapping pages) to make the back buffer the new front buffer. No data is copied — only page table entries are updated (zero-copy flip).

## Display Modes

Mode setting is done at initialization:

```c
struct disp_mode_cmd {
    uint32_t width, height;
    uint32_t bpp;
    uint32_t refresh_hz;  // advisory; QEMU runs at VSYNC rate
};
```

After mode set, a new framebuffer region is allocated matching the new resolution.

## Kernel Panic Display

The `kpanic` handler in the kernel bypasses the display driver entirely. It writes directly to the physical framebuffer memory (using the `kfb.virt_addr` kernel mapping) to display a white-on-red panic message. This is intentional — a panicking system cannot rely on any userspace process.

## Related Documents

- [overview.md](overview.md)
- [hybrid-architecture.md](hybrid-architecture.md)
- [driver-communication.md](driver-communication.md)
- [userspace/gui-architecture.md](../userspace/gui-architecture.md)
