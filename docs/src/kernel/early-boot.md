# Early Boot

## Overview

Early boot is the phase from `kernel_main()` entry to the point where the scheduler starts and the first kernel thread runs. During this phase:

- Only CPU 0 is active.
- Interrupts are disabled until the IDT and APIC are configured.
- No dynamic memory allocation is available until the physical and virtual memory managers are initialized.
- Only serial output (`early_printk`) is available until `klog_init`.

## Initialization Sequence

The sequence is strictly ordered. Each step depends on all prior steps.

### Step 1 — Validate Boot Info

```c
kernel_main(struct BootInfo *boot_info) {
    if (boot_info->magic != BOOT_MAGIC) early_panic("bad boot magic");
    if (boot_info->version != BOOT_VERSION) early_panic("bad boot version");
```

`early_panic` writes to COM1 and halts. It does not use the normal panic system (not yet initialized).

### Step 2 — Early Serial Output

```c
    early_serial_init();   // COM1 at 115200 8N1, direct port I/O
    early_printk("[early] Kernel started\n");
```

### Step 3 — Physical Memory Manager

```c
    pmm_init(&boot_info->memory_map, boot_info->memory_map_count);
```

The PMM parses the E820 map, builds the buddy allocator free lists, and makes memory available for allocation. The kernel image pages, boot page table pages, and the `BootInfo` structure are marked reserved before anything else.

### Step 4 — Virtual Memory Manager

```c
    vmm_init();
```

Sets up the kernel page tables (higher-half direct map, vmalloc region). Switches CR3 to the kernel PML4, discarding the bootloader's temporary page tables. After this, the identity map from the bootloader is no longer accessible.

### Step 5 — SLAB Allocator

```c
    slab_init();
```

Registers default slab caches for common kernel objects (`task_struct`, `file`, `inode`, `dentry`, `socket`, etc.). `kmalloc` becomes available after this point.

### Step 6 — Structured Logging

```c
    klog_init();
    klog_info("kernel %s booting on %d CPUs", KERNEL_VERSION, boot_info->cpu_count);
```

The ring buffer, serial sink, and (later) framebuffer sink are activated. `early_printk` is replaced by `klog`.

### Step 7 — GDT and TSS (per CPU 0)

```c
    gdt_init();    // Load kernel GDT
    tss_init();    // Initialize CPU 0 TSS with kernel stack
```

### Step 8 — IDT and APIC

```c
    idt_init();    // Set up all 256 IDT entries
    apic_init();   // Enable LAPIC, configure I/O APIC, calibrate LAPIC timer
    sti();         // Enable interrupts
```

Interrupts are now active. The scheduler tick will fire, but the scheduler is not yet initialized — the tick handler returns immediately until step 9.

### Step 9 — Scheduler

```c
    sched_init();  // Initialize MLFQ queues, real-time class, idle thread
```

### Step 10 — Module System

```c
    module_init();
```

### Step 11 — Driver Probes

```c
    driver_probe_all();  // Run all compiled-in driver init functions
```

Discovers PCI devices, initializes the block device layer, registers the framebuffer driver.

### Step 12 — Root Filesystem Mount

```c
    vfs_init();
    ext4_register();
    mount_root();  // Mount ext4 from the boot device to "/"
```

### Step 13 — SMP Bring-Up

```c
    smp_init();    // Send INIT/SIPI to all APs, wait for them to register
```

Each AP runs its own abbreviated init: GDT, TSS, IDT, APIC, SLAB magazine, then enters the scheduler idle loop.

### Step 14 — Spawn Init

```c
    kernel_exec("/sbin/init", NULL, NULL);
    // Does not return
```

`kernel_exec` creates the first user process (PID 1) and starts the scheduler. CPU 0 becomes a normal scheduler participant at this point.

## Error Handling During Early Boot

If any step fails before `klog_init`, `early_panic()` is used (serial + halt). After `klog_init`, `kpanic()` (the normal panic system) is used.

## Related Documents

- [bootloader/kernel-handoff.md](../bootloader/kernel-handoff.md)
- [kernel/logging.md](logging.md)
- [kernel/panic-system.md](panic-system.md)
- [memory/physical-allocator.md](../memory/physical-allocator.md)
- [architecture/boot-sequence.md](../architecture/boot-sequence.md)
