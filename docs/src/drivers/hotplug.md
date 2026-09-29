# Device Hotplug

## Overview

Hotplug allows devices to be added to or removed from the system while it is running, without requiring a reboot. In the QEMU environment, hotplug is primarily relevant for virtual device attach/detach via the QEMU monitor.

## PCI Hotplug

PCI hotplug is signaled to the OS via an ACPI hotplug notification or a PCI hotplug interrupt. For QEMU:

- `device_add virtio-blk-pci,...` triggers a PCI hotplug event.
- `device_del <id>` triggers a PCI hot-remove event.

The kernel receives these as ACPI SCI interrupts, which are routed to the ACPI subsystem.

## Hotplug Event Flow (Add)

1. **ACPI SCI interrupt** fires → ACPI subsystem parses the event → identifies a new PCI device on a specific bus/device/function.
2. **Device enumeration**: The kernel reads the new device's PCI config space (vendor ID, device ID, BARs).
3. **Driver probe**: `driver_try_probe(new_device)` is called. If a matching driver is registered: the `probe` function runs.
4. **Userspace notification**: A hotplug event is written to the device manager's event socket. The device manager (`/sbin/devd`) may spawn new services or load driver configuration.

## Hotplug Event Flow (Remove)

1. **ACPI SCI interrupt** signals a device departure.
2. **Driver remove**: `driver->remove(device)` is called.
   - The driver sends `SIGTERM` to the userspace driver process.
   - The userspace driver drains in-flight requests, writes completions with status `-ENODEV`.
   - Unmaps shared memory and MMIO regions.
3. **Device unregistration**: The device is removed from the device tree.
4. **Userspace notification**: A hotplug removal event is sent to `/sbin/devd`.
5. Any processes with open file descriptors to the device receive `-EIO` on next access.

## Device Manager (`/sbin/devd`)

The device manager is a userspace daemon that:

- Listens on a kernel hotplug event socket (a special fd exposed by the kernel).
- Matches device events against rules in `/etc/devd.conf`.
- Runs rule actions: create/remove device nodes in `/dev`, load/unload driver modules, start/stop services.

## Storage Hot-Remove Consistency

When a storage device is hot-removed while mounted:

1. The block device layer marks all I/O to that device as failed (`-EIO`).
2. The filesystem layer sees I/O errors and marks itself as read-only (or triggers an emergency unmount).
3. All open file descriptors on files from that filesystem return `-EIO`.
4. The kernel logs a warning.

Clean hot-remove requires the user to `umount` first (which flushes all dirty pages and closes the device safely).

## Related Documents

- [kernel-drivers.md](kernel-drivers.md)
- [overview.md](overview.md)
- [filesystem/mount-system.md](../filesystem/mount-system.md)
