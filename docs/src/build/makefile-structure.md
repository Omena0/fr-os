# Makefile Structure

## Top-Level Makefile

The repository root contains a single top-level `Makefile` that orchestrates all sub-builds:

```makefile
# Top-level Makefile

CROSS    := x86_64-elf
CC       := $(CROSS)-gcc
LD       := $(CROSS)-ld
OBJCOPY  := $(CROSS)-objcopy
NASM     := nasm
QEMU     := qemu-system-x86_64

BUILDDIR := build

.PHONY: all clean run iso test check

all: $(BUILDDIR)/disk.img

clean:
	rm -rf $(BUILDDIR)

run: all
	$(QEMU) -enable-kvm -cpu host -smp 18 -m 4G \
	    -drive format=raw,file=$(BUILDDIR)/disk.img \
	    -serial stdio -display gtk -no-reboot

iso: $(BUILDDIR)/os.iso

test: unit-tests

check: unit-tests integration-tests

# Include sub-build rules:
include src/boot/Makefile.inc
include src/kernel/Makefile.inc
include src/libc/Makefile.inc
include src/userspace/Makefile.inc
include src/initrd/Makefile.inc
include src/disk/Makefile.inc
```

## Sub-Build Makefile Fragments

### `src/boot/Makefile.inc`

```makefile
BOOT_STAGE1_SRC := src/boot/stage1.asm
BOOT_STAGE2_SRC := $(wildcard src/boot/stage2/*.c src/boot/stage2/*.asm)

$(BUILDDIR)/stage1.bin: $(BOOT_STAGE1_SRC)
	$(NASM) -f bin -o $@ $<

$(BUILDDIR)/stage2.elf: $(BOOT_STAGE2_SRC)
	i686-elf-gcc -m32 -ffreestanding -nostdlib -o $@ $^ \
	    -T src/boot/stage2/link.ld
```

### `src/kernel/Makefile.inc`

```makefile
KERNEL_SRCS := $(wildcard src/kernel/**/*.c src/kernel/**/*.asm)
KERNEL_CFLAGS := -ffreestanding -nostdlib -mno-red-zone -mno-mmx -mno-sse \
    -Wall -Wextra -O2 -g -mcmodel=kernel \
    -mno-implicit-float

$(BUILDDIR)/kernel.elf: $(KERNEL_SRCS)
	$(CC) $(KERNEL_CFLAGS) -o $@ $^ -T src/kernel/link.ld
	$(OBJCOPY) --strip-debug -O binary $@ $(BUILDDIR)/kernel.bin
```

## Key Compiler Flags

| Flag | Reason |
|---|---|
| `-ffreestanding` | No standard library; kernel provides its own runtime |
| `-nostdlib` | Do not link against host libc |
| `-mno-red-zone` | Disable red zone (required for interrupt handlers — ISRs can interrupt at any time) |
| `-mno-mmx -mno-sse -mno-implicit-float` | No FPU/SIMD in kernel (FPU context managed manually via CR0.TS) |
| `-mcmodel=kernel` | Place kernel in the upper half of the address space (above 0xFFFF800000000000) |
| `-O2` | Enable optimizations |
| `-g` | Include debug symbols in ELF (stripped to `.bin` by objcopy) |

## Linker Script: `src/kernel/link.ld`

```ld
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)

ENTRY(kernel_start)

SECTIONS {
    . = 0xFFFF800000100000;   /* kernel virtual base */

    .text   : { *(.text*) }
    .rodata : { *(.rodata*) }
    . = ALIGN(4096);
    .data   : { *(.data*) }
    .bss    : { *(COMMON) *(.bss*) }
}
```

## Dependency Tracking

The Makefile uses automatic dependency generation:
```makefile
DEPFLAGS = -MMD -MP -MF $(BUILDDIR)/$*.d
-include $(wildcard $(BUILDDIR)/**/*.d)
```

`-MMD -MP` cause GCC to write a `.d` file alongside each object file listing all header dependencies. The `-include` re-reads those `.d` files so header changes trigger recompilation.

## Related Documents

- [overview.md](overview.md)
- [toolchain.md](toolchain.md)
- [cross-compilation.md](cross-compilation.md)
- [qemu-setup.md](qemu-setup.md)
