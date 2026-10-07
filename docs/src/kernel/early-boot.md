# Early Boot

## Overview

Early boot is the phase from `kernel_main()` entry to the point where the scheduler starts and the first kernel thread runs. During this phase:

- Only CPU 0 is active.
- Interrupts are disabled until the IDT and APIC are configured.
- No dynamic memory allocation is available until the physical and virtual memory managers are initialized.
- Only serial output (`early_printk`) is available until `klog_init`.

## Initialization Sequence

The sequence is strictly ordered. Each step depends on all prior steps.

### Step 1 — Early Serial Output

```c
    early_serial_init();   // COM1 at 115200 8N1, direct port I/O
                           // (stage 2 used 38400 on the same wire)
    early_printk("[early] Kernel started\n");
```

### Step 2 — Validate Boot Info

```c
    kmain(uint64_t bootinfo_phys) {
        if (boot_info->magic != BOOT_MAGIC) early_panic("bad boot magic");
        if (boot_info->version != BOOT_VERSION) early_panic("bad boot version");
```

`early_panic` writes to COM1 and halts. It does not use the normal panic system. The `BootInfo` structure follows the Multiboot specification[^multiboot-bootinfo].

### Step 3 — Copy Boot Info

```c
    const struct bootinfo *src = boot_ptr((phys_addr_t)bootinfo_phys);
    boot = *src;
    // Copy E820 map to kernel's static buffer
```

The kernel copies the bootloader's structures before they can be reclaimed by the physical allocator.

### Step 4 — Per-CPU Setup and CPU Features

```c
    percpu_setup(0);
    cpu_features_init();
```

The per-CPU area must exist before anything uses `this_cpu()`, and CPU features must be detected before paging setup (which chooses 1 GiB vs 2 MiB pages based on CPUID).

### Step 5 — Virtual Memory Manager

```c
    vmm_init();
    vmm_switch_to_kernel_pgd();
```

Sets up the kernel page tables (higher-half direct map, vmalloc region, kernel window). Switches CR3 to the kernel PML4, discarding the bootloader's temporary page tables.

### Step 6 — Physical Memory Manager

```c
    pmm_init(kernel_virt_to_phys(e820_copy), boot.e820_count);
    pmm_reserve_range(0, 1024 * 1024);              // Low 1 MiB (bootloader still there)
    pmm_reserve_range(0x002D0000, 0x00010000);      // Bootstrap page tables
```

PMM parses the E820 map, builds buddy free lists, and makes memory available. VMM must come first because PMM places its metadata through `phys_to_virt()`.

### Step 7 — SLAB Allocator

```c
    kmalloc_init();
```

Initializes generic kmalloc caches (8 bytes through 4 KiB). `kmalloc` becomes available after this point.

### Step 8 — Framebuffer Console (if present)

```c
    console_init(&boot);  // Attaches framebuffer backend, allocates cell buffer
```

The framebuffer backend is attached here (after `kmalloc_init`) because it allocates the cell buffer.

### Step 9 — Structured Logging

```c
    klog_init();
    klog_info("%s booting on %d CPUs", KERNEL_VERSION_STRING, cpu_features.logical_processors);
```

The ring buffer, serial sink, and framebuffer sink are activated. `early_printk` is replaced by `klog`.

### Step 10 — IDT and Interrupts

```c
    idt_init();           // Set up all 256 IDT entries, install PIT, unmask IRQ0
```

The IDT is initialized before the GDT so that faults during GDT reload are caught by the kernel's own handlers, not the bootloader's.

### Step 11 — GDT and TSS (per CPU 0)

```c
    asm volatile("cli");
    gdt_reload(0);        // Load kernel GDT, TSS
    tss_set_kernel_stack(__kernel_stack_top);
    exceptions_init();    // Install exception handlers
```

Interrupts are disabled across the GDT reload to prevent a timer tick from interleaving with the far-return frame. `tss_set_kernel_stack` sets RSP0 for ring-3 transitions.

### Step 12 — Syscall Entry

```c
    syscall_init();       // Install LSTAR/STAR/SFMASK
```

Installed after the IDT so that a user process cannot reach a SYSCALL instruction with no dispatcher behind it.

### Step 13 — TTY/Console

```c
    tty_init();           // Ring buffers, keyboard handler, enable scanning
```

Attaches the console TTY before creating the first user process.

### Step 14 — Scheduler

```c
    sched_init();         // Initialize MLFQ queues, real-time class, idle thread
```

Creates per-CPU run queues, idle task, and publishes `per_cpu(current)`.

### Step 15 — Create Init Process

```c
    struct task *init = process_create_init();
    // Creates first user process (PID 1), loads ELF, sets up address space
```

### Step 16 — Enter Scheduler

```c
    sched_start();        // Never returns; hands CPU to first runnable task
```

## Error Handling During Early Boot

If any step fails before `klog_init`, `early_panic()` is used (serial + halt). After `klog_init`, `kpanic()` (the normal panic system) is used.

## Related Documents

- [bootloader/kernel-handoff.md](../bootloader/kernel-handoff.md)
- [kernel/logging.md](logging.md)
- [kernel/panic-system.md](panic-system.md)
- [memory/physical-allocator.md](../memory/physical-allocator.md)
- [architecture/boot-sequence.md](../architecture/boot-sequence.md)

## References

- [Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A — System Programming][intel-sdm-vol3a]
- [AMD64 Architecture Programmer's Manual, Volume 2 — System Initialization][amd-apm-init]
- [Multiboot Specification — Boot Information][multiboot-bootinfo]
- [ACPI Specification — RSDP and RSDT][acpi-rsdp]

[intel-sdm-vol3a]: https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html#vol3a "Intel SDM Volume 3A: System Programming Guide, Part 1"
[amd-apm-init]: https://www.amd.com/en/developer/architecture-programmer-manuals.html "AMD64 Architecture Programmer's Manual Volume 2"
[multiboot-bootinfo]: https://www.gnu.org/software/grub/manual/multiboot/multiboot.html#Boot-information "Multiboot Specification - Boot Information"
[acpi-rsdp]: https://uefi.org/specs/ACPI/6.5/05_ACPI_Software_Programming_Model/ACPI_Software_Programming_Model.html#root-system-description-pointer-rsdp "ACPI 6.5 - Root System Description Pointer (RSDP)"
