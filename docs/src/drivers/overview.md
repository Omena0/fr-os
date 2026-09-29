# Drivers Overview

## Architecture

The OS uses a **hybrid driver model**: a minimal ring-0 driver core handles interrupt routing, DMA, and device register access, while the bulk of device-specific logic runs in ring-3 userspace driver processes.

```
User ring-3:    [Userspace driver process]  ←→  [Shared memory / event queue]
                          ↑  ↓ (syscalls: ioctl, mmap, read event)
Kernel ring-0:  [Driver core] ←→ [Hardware registers, IRQs, DMA]
                          ↑
                     [PCI bus / device tree]
```

Benefits:

- Driver crashes do not crash the kernel (ring-3 isolation).
- Drivers can be restarted without rebooting.
- Drivers can be written in any language that can call syscalls.

Trade-off:

- One additional context switch per I/O operation compared to in-kernel drivers.
- Shared memory minimizes data copying; the hot path avoids per-operation syscalls.

## Kernel-Side Driver Responsibilities

- Device discovery (PCI enumeration).
- Interrupt registration and routing (LAPIC + I/O APIC).
- DMA mapping and buffer management.
- MMIO region mapping into the userspace driver's address space.
- Event queue ring buffer management (kernel writes events; userspace reads).
- Crash detection (userspace driver process death → failsafe stub).

## Userspace Driver Responsibilities

- Device protocol (e.g., NVMe command set, USB HID report parsing, display mode setting).
- Command processing from the kernel event queue.
- Higher-level abstractions (e.g., block I/O queue for storage, pixel output for display).

## Driver Categories

| Category | Ring-0 core | Userspace logic | Document |
|---|---|---|---|
| Display | Framebuffer init, mode set | Compositing, format conversion | [display-driver.md](display-driver.md) |
| Input | HID IRQ, PS/2 decoder | Key map, event queue | [input-driver.md](input-driver.md) |
| Storage | DMA, virtio-blk protocol | Block scheduling, caching | [storage-driver.md](storage-driver.md) |
| Network | DMA ring, IRQ | TCP/IP stack (in-kernel) | [networking/overview.md](../networking/overview.md) |

## Driver Registration

In-kernel (probe-based) driver registration:

```c
struct driver {
    const char *name;
    uint16_t    pci_vendor, pci_device;  // for PCI drivers
    int (*probe)(struct device *dev);    // called when device is found
    void (*remove)(struct device *dev);  // called on hotplug remove
};

void driver_register(struct driver *drv);
```

Probe functions are called by the device manager during PCI enumeration or virtual device initialization (for QEMU virtio devices).

## Related Documents

- [hybrid-architecture.md](hybrid-architecture.md)
- [kernel-drivers.md](kernel-drivers.md)
- [userspace-drivers.md](userspace-drivers.md)
- [driver-communication.md](driver-communication.md)
- [display-driver.md](display-driver.md)
- [input-driver.md](input-driver.md)
- [storage-driver.md](storage-driver.md)
- [hotplug.md](hotplug.md)
- [driver-isolation.md](driver-isolation.md)
