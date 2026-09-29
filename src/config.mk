# Build configuration shared by every sub-Makefile fragment.

# The build runs on the host toolchain. Everything is freestanding: no host
# libc, no host CRT, no host linker scripts.
HOST_GCC        := gcc
HOST_AR         := ar
HOST_LD         := ld
HOST_OBJCOPY    := objcopy
HOST_AS         := as
HOST_CC_32      := $(HOST_GCC) -m32
HOST_CC_64      := $(HOST_GCC) -m64
HOST_LD_32      := $(HOST_LD) -m elf_i386
HOST_LD_64      := $(HOST_LD) -m elf_x86_64

BUILD           := build
OBJ             := $(BUILD)/obj

# ---------------------------------------------------------------- kernel ----
# -mcmodel=kernel gives us RIP-relative addressing for everything above
# 0xFFFFFFFF80000000, so no GOT indirection and no runtime relocation
# processing in the entry path.
KERNEL_CFLAGS := \
	-std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
	-fno-pic -fno-pie -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fno-strict-aliasing -fno-common -fomit-frame-pointer \
	-m64 -mcmodel=kernel -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
	-mno-80387 -msoft-float \
	-mavx2 -mbmi -mbmi2 -mfma -madx -mrdrnd -mf16c -mclwb -mclflushopt \
	-mxsave -mxsaveopt -mxsavec \
	-msse4.2 -mpopcnt -mclflushopt \
	-O2 -g3 -Wall -Wextra -Werror=implicit-function-declaration \
	-Werror=return-type -Wno-unused-parameter -Wno-address-of-packed-member \
	-I src/include -I src/kernel/include -I src/kernel

# The kernel is loaded by stage2 at a fixed physical address, so the link
# script pins the physical placement too.
#
# Note the absence of -n (--nmagic). nmagic tells the linker not to resolve
# undefined symbols, which would turn a missing kernel function into a call to
# address zero at runtime — precisely the class of bug this build should fail
# on. The kernel is statically linked with no dynamic loader to fall back on, so
# an undefined symbol must be a link error.
KERNEL_LDFLAGS := -T src/kernel/link.ld -z max-page-size=4096 --no-dynamic-linker

# ------------------------------------------------------------- bootloader ----
BOOT_CFLAGS := \
	-std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
	-fno-pic -fno-pie -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fno-strict-aliasing -fno-common -fomit-frame-pointer \
	-m32 -mno-red-zone -mno-sse -mno-mmx -mno-80387 -msoft-float \
	-O2 -g3 -Wall -Wextra -Wno-unused-parameter \
	-I src/include -I src/boot

BOOT_ASFLAGS := -m32 -I src/include/ -I src/boot/

# -------------------------------------------------------------- userspace ----
# Userspace targets the same ISA as the kernel but is compiled as ordinary
# non-PIE position-dependent code: the kernel supplies the entry point and the
# initial stack, so we keep full control of the process image layout.
USER_CFLAGS := \
	-std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
	-fno-pic -fno-pie -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fno-strict-aliasing -fno-common -m64 -mno-red-zone -mcmodel=small \
	-O2 -g3 -Wall -Wextra -Wno-unused-parameter \
	-mavx2 -mfma -mbmi -mbmi2 -mf16c -mxsave \
	-I src/include -I src/libc/include

# -static keeps the user image free of an interpreter: the kernel's ELF loader
# maps PT_LOAD segments and jumps to e_entry. There is no dynamic linker yet.
USER_LDFLAGS := -no-pie -nostdlib -static -z noexecstack -Wl,--build-id=none

QEMU := qemu-system-x86_64
