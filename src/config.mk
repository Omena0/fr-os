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

# objcopy invocations, kept in one place because "which artifacts are stripped"
# is a decision and not a detail of any single rule.
#
# -g3 stays on everywhere. The debug info is worth having, and the question of
# what it costs is not "how big is the file" but "does the boot path ever read
# it" -- which is answered per-artifact below, not globally.
#
#   kernel.elf   NOT stripped, and it does not need to be. stage2 reads only the
#                PT_LOAD segments: it walks the program headers and copies
#                p_filesz bytes of each, so everything past the last segment
#                (.debug_*, .symtab, .strtab) is never fetched. Measured on the
#                current build: 730 KB on disk, of which the loader reads
#                274 KB, and every one of the 456 KB of debug data sits after
#                the last PT_LOAD at file offset 0x43070. Stripping would save
#                disk and save the boot nothing.
#
#   init.elf     STRIPPED, and this one is not optional. It is different in kind
#                from the kernel: tools/bin2c.py turns it into a C array that
#                is compiled into the kernel's .rodata, which *is* a PT_LOAD.
#                So 91% of init.elf's bytes -- 177 KB of .debug_* out of 195 KB
#                -- are not sitting harmlessly after the image, they are inside
#                it, being read sector by sector by the loader, copied into
#                physical memory, and mapped. That is paid on every boot, and
#                it is charged against KERNEL_MAX_BYTES, which has 32 KB of
#                headroom left. See the ASSERT in src/kernel/link.ld.
#
# The kernel's own ELF headers stay intact, so `nm` and `objdump` still resolve
# every symbol by name; only the DWARF is dropped, and the source is the
# debugger anyway.
STRIP_ALL      := --strip-all
STRIP_DEBUG    := --strip-debug

BUILD           := build
OBJ             := $(BUILD)/obj

# ---------------------------------------------------------------- kernel ----
# -mcmodel=kernel gives us RIP-relative addressing for everything above
# 0xFFFFFFFF80000000, so no GOT indirection and no runtime relocation
# processing in the entry path.
#
# The GPR-only extensions are kept: BMI, BMI2, ADX, RDRND, CLWB, CLFLUSHOPT and
# POPCNT all operate on general-purpose registers and need no register state
# beyond what long mode already gives us.
#
# The vector prohibition is the interesting part, and it is at the END of the
# flag list on purpose.
#
# A VEX or SSE instruction is not merely an optimisation here, it is an
# instruction fault. The register state it needs has to have been enabled
# through XCR0 before it will execute at all, and the kernel entry path does
# not and should not program XCR0. So the rule is not "avoid vector code
# because it is slower", it is "any vector instruction at all is a #UD before
# the first character is printed".
#
# Two flags got this wrong before, in the same way, and both were invisible:
#
#   -mavx2 -mfma -mf16c -mxsave   made every 16-byte block copy a VEX-encoded
#                                vmovdqu, and the first one raised #UD in kmain.
#
#   -msse4.2                     sat *after* -mno-sse2 and silently won. On x86
#                                the last -m flag for an ISA feature is the one
#                                that counts, so a build that read as "vector
#                                code is forbidden" permitted it anyway. This is
#                                the trap worth designing against: a prohibition
#                                expressed as an earlier flag is not a
#                                prohibition, it is a preference.
#
# So the -mno- flags come last, after every -m flag that could re-enable
# anything, and the `verify-isa` target in the Makefile disassembles the linked
# kernel and fails the build if a single vector instruction survives. The flag
# list is the policy; the check is what makes it true regardless of what anyone
# appends to it later.
KERNEL_CFLAGS := \
	-std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
	-fno-pic -fno-pie -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fno-strict-aliasing -fno-common -fomit-frame-pointer \
	-m64 -mcmodel=kernel -mno-red-zone \
	-mbmi -mbmi2 -madx -mrdrnd -mclwb -mclflushopt \
	-mpopcnt \
	-O2 -g3 -Wall -Wextra -Werror=implicit-function-declaration \
	-Werror=return-type -Wno-unused-parameter -Wno-address-of-packed-member \
	-I src/include -I src/kernel/include -I src/kernel \
	-mno-sse -mno-sse2 -mno-avx -mno-avx2 -mno-fma -mno-f16c \
	-mno-mmx -mno-80387 -mno-80387 -msoft-float -mno-red-zone

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
	-mbmi -mbmi2 -mpopcnt \
	-I src/include -I src/libc/include

# -static keeps the user image free of an interpreter: the kernel's ELF loader
# maps PT_LOAD segments and jumps to e_entry. There is no dynamic linker yet.
USER_LDFLAGS := -no-pie -nostdlib -static -z noexecstack -Wl,--build-id=none

QEMU := qemu-system-x86_64
