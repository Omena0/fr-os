# Multiboot Compatibility

## Policy

The OS bootloader is a **custom two-stage design** (stage1 + stage2). It does **not** use the Multiboot or Multiboot2 specification as its primary boot protocol.

However, for QEMU development and testing convenience, the kernel binary can optionally be loaded directly by QEMU using the `-kernel` flag with a Multiboot2-compatible header embedded in the kernel image. This is a development shortcut only — the production boot path is always stage1 → stage2 → kernel.

## Multiboot2 Header (Development Mode)

The kernel embeds a Multiboot2 header in the `.multiboot` section of the ELF binary:

```c
#define MB2_MAGIC    0xE85250D6
#define MB2_ARCH     0           // i386 (Multiboot2 uses i386 even for 64-bit)
#define MB2_LENGTH   /* computed at build time */

// Placed at a 64-byte aligned offset within the first 32 KB of the binary
struct Multiboot2Header {
    uint32_t magic;
    uint32_t architecture;
    uint32_t header_length;
    uint32_t checksum;       // -(magic + arch + length)
    // Tags follow...
    // End tag: type=0, flags=0, size=8
};
```

When QEMU's `-kernel` mode is active, QEMU loads the kernel and passes a Multiboot2 information structure. The kernel's early boot path detects this (by checking for the Multiboot2 info magic in `rbx`) and translates the Multiboot2 info into the internal `BootInfo` format before calling the main initialization path.

## QEMU Development Invocation

```bash
qemu-system-x86_64 \
  -enable-kvm \
  -kernel build/kernel.elf \
  -append "console=ttyS0" \
  -serial stdio \
  -m 4G \
  -smp 18
```

This bypasses the bootloader entirely and boots the kernel directly. Useful for rapid kernel development iteration.

## Limitations of Multiboot Mode

- No framebuffer configured by stage 2 (QEMU may provide one via Multiboot2 framebuffer tag).
- Memory map comes from Multiboot2 (accurate for QEMU but not BIOS E820 directly).
- The kernel binary must have the Multiboot2 header within the first 32 KB.
- Long mode is **not** set up by Multiboot2 loaders for 64-bit kernels — the kernel must include its own 32-bit bootstrap stub to enter long mode from the Multiboot2 32-bit protected mode entry point.

## Related Documents

- [overview.md](overview.md)
- [stage1.md](stage1.md)
- [stage2.md](stage2.md)
- [kernel-handoff.md](kernel-handoff.md)
- [build/qemu-setup.md](../build/qemu-setup.md)
