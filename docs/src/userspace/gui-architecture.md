# GUI Architecture

## Overview

The OS provides a framebuffer-based graphical user interface consisting of three layers:
1. **Display server**: owns the framebuffer, manages windows, routes input events.
2. **Window manager**: controls window placement, decorations, focus policy.
3. **Widget toolkit**: provides standard UI controls (buttons, text fields, menus).

## Display Server

The display server runs in ring-3 with access to the framebuffer device (`/dev/fb0`). Applications connect to it via a Unix domain socket (`/run/display`).

### Client Protocol

Each application window is backed by a **shared memory buffer** (allocated via `sys_shm_open`). When the application wants to update its window:
1. It renders into the shared memory buffer.
2. It sends a `DAMAGE` message to the display server with the dirty rectangle.
3. The display server composites the damaged region from the shared buffer into the framebuffer.

This avoids copying pixel data through the socket — only metadata crosses the IPC boundary.

### Message Types

| Message | Direction | Payload |
|---|---|---|
| `MSG_CREATE_WINDOW` | Client → Server | Width, height, title |
| `MSG_DESTROY_WINDOW` | Client → Server | Window ID |
| `MSG_DAMAGE` | Client → Server | Window ID, dirty rect |
| `MSG_RESIZE` | Server → Client | New width and height |
| `MSG_KEY_EVENT` | Server → Client | Keycode, modifier mask, down/up |
| `MSG_MOUSE_EVENT` | Server → Client | X, Y, button mask, scroll delta |
| `MSG_FOCUS_IN/OUT` | Server → Client | Focus gained/lost |
| `MSG_CLOSE_REQUEST` | Server → Client | User clicked the close button |

## Window Manager

The window manager is a separate process that connects to the display server as a privileged client. It:
- Draws window decorations (title bar, borders, close/minimize/maximize buttons).
- Handles mouse events on decorations (drag to move, resize handles).
- Manages window stacking order (z-order).
- Implements focus policy (click-to-focus or follow-mouse).

The window manager and display server communicate via the same IPC protocol as regular clients, but with additional privileged messages for repositioning and stacking windows.

## Framebuffer Layout

The display server maps `/dev/fb0` (the framebuffer memory) directly into its address space. The framebuffer is a flat array of pixels in `XRGB8888` format:

```
offset = y * pitch + x * 4
pixel  = (0xFF << 24) | (r << 16) | (g << 8) | b
```

`pitch` is the number of bytes per row (may include padding for alignment). The screen resolution and pitch are queried via `ioctl(fb_fd, FBIOGET_VSCREENINFO, ...)`.

## Compositing

The display server maintains a list of visible windows (front-to-back order). When a window sends a `DAMAGE` message, the compositor:
1. Clips the damaged rect to the screen bounds.
2. For each pixel in the dirty area: find the topmost window that covers that pixel; blit from that window's shared buffer.
3. Update the framebuffer (`memcpy` or SIMD copy).

For 18-core systems: compositing can be parallelized by dividing the dirty area into horizontal bands and processing them on multiple threads.

## Widget Toolkit

The widget toolkit provides standard UI controls. It is a userspace library that:
- Renders widgets into the window's shared memory buffer.
- Translates display server events into high-level widget events (button clicks, text input).
- Manages widget layout (flex-like layout engine).

### Widget Tree

```
Window
  └─ VBox (vertical layout)
       ├─ MenuBar
       ├─ HBox (horizontal layout)
       │    ├─ TextArea (main content)
       │    └─ Scrollbar
       └─ StatusBar
```

Each widget has a bounding box (position + size) within the window. Layout is computed top-down.

## Related Documents

- [overview.md](overview.md)
- [drivers/display-driver.md](../drivers/display-driver.md)
- [drivers/input-driver.md](../drivers/input-driver.md)
- [ipc/unix-sockets.md](../ipc/unix-sockets.md)
- [ipc/shared-memory.md](../ipc/shared-memory.md)
