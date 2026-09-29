# Kernel Driver Infrastructure

## Overview

The kernel driver infrastructure provides the in-kernel half of the hybrid driver model. It handles device discovery, interrupt management, DMA, and the shared-memory region setup for userspace drivers.

## Device Model

Every discovered device is represented by a `struct device`:

```c
struct device {
    char            name[64];
    enum bus_type   bus;          // BUS_PCI, BUS_VIRTIO, BUS_PLATFORM
    struct driver  *driver;       // matched driver (NULL if unbound)
    void           *driver_data;  // driver-private pointer
    uint32_t        pci_bus, pci_dev, pci_func;  // PCI address (if BUS_PCI)
    struct list_head children;    // child devices
    struct device  *parent;
};
```

## PCI Enumeration

During early boot, the kernel scans PCI bus 0 (and bridges recursively):

```c
for (uint8_t bus = 0; bus < 256; bus++)
    for (uint8_t dev = 0; dev < 32; dev++)
        for (uint8_t func = 0; func < 8; func++) {
            uint16_t vendor = pci_read_config_word(bus, dev, func, 0);
            if (vendor == 0xFFFF) continue;  // no device
            struct device *d = alloc_device();
            d->bus = BUS_PCI;
            d->pci_bus = bus; d->pci_dev = dev; d->pci_func = func;
            device_register(d);
            driver_try_probe(d);
        }
```

QEMU provides virtio devices on the PCI bus (vendor `0x1AF4`).

## Driver Probe and Bind

`driver_try_probe(device)` iterates the registered driver list and calls `driver->probe(device)` for the first driver with a matching PCI vendor/device ID (or bus type for platform drivers). The `probe` function:

1. Reads device configuration registers (PCI BAR addresses, capabilities).
2. Maps MMIO regions into kernel virtual memory (`ioremap`).
3. Allocates DMA-coherent memory for command and completion queues.
4. Registers interrupt handlers (`irq_register(vector, handler, device)`).
5. Sets up the shared memory region for the userspace driver.
6. Spawns the userspace driver process (`kernel_exec("/usr/lib/drivers/<name>")`).

## Interrupt Registration

```c
int irq_register(uint8_t vector, irq_handler_t handler, void *dev);
void irq_unregister(uint8_t vector);
```

The IRQ vector is programmed in the I/O APIC routing table to deliver to the correct CPU (or all CPUs for shared IRQs). The handler runs in interrupt context (interrupts disabled, cannot sleep).

IRQ handler rules:

- Must not call sleeping functions (`kmutex_lock`, `kfree`).
- Must acknowledge the interrupt (EOI to LAPIC).
- Should do minimal work: acknowledge the device, write a completion entry, wake the userspace driver (via `irq_count` increment and optional futex wake).

## DMA

The kernel provides a DMA API:

```c
void *dma_alloc_coherent(size_t size, dma_addr_t *phys_out);
void  dma_free_coherent(void *virt, dma_addr_t phys, size_t size);
dma_addr_t dma_map_page(struct page *page, size_t offset, size_t size, int direction);
void       dma_unmap_page(dma_addr_t addr, size_t size, int direction);
```

All DMA addresses are physical addresses. QEMU with `-enable-kvm` and the identity-mapped direct region (0xFFFF_8000_0000_0000 + phys_addr) makes DMA transparent.

## Related Documents

- [hybrid-architecture.md](hybrid-architecture.md)
- [userspace-drivers.md](userspace-drivers.md)
- [driver-communication.md](driver-communication.md)
- [hotplug.md](hotplug.md)
- [kernel/interrupt-handling.md](../kernel/interrupt-handling.md)
