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
# The reason the kernel is GPR-only is NOT that a VEX instruction would fault
# here. The loader does program XCR0: stage2_long.S:195-251 reads CPUID.1:ECX
# for AVX (bit 27) and OSXSAVE (bit 28), sets CR4.OSXSAVE, masks whatever the
# firmware left in XCR0 against CPUID.0x0D subleaf 0, and writes back XCR0|0x7
# -- x87 | SSE | YMM_Hi128. So on this machine AVX is *enabled*, and a VEX
# instruction in the kernel would execute.
#
# That makes the prohibition three separate things depending on the CPU, which
# is why it has to be a prohibition and not an inference from a flag:
#
#   1. CPU without AVX or without OSXSAVE. The loader takes the .Lno_avx path
#      and never touches XCR0, so every VEX instruction is #UD. A build that
#      emits one works on the developer's -cpu max and dies on real hardware,
#      or the other way round.
#   2. CPU with AVX, YMM enabled. A VEX instruction in the kernel executes --
#      and the context switch cannot preserve what it leaves behind.
#      fpu_save/fpu_restore in context.S are FXSAVE/FXRSTOR, the legacy 512-byte
#      layout. That layout holds the x87 state and XMM0-XMM15 and nothing
#      else: YMM_Hi128 is not in it. sched.c calls them on every task switch, so
#      any YMM upper half a task is holding is silently destroyed when it is
#      preempted, with no fault and nothing in the log to say so. That is the
#      failure mode this flag list exists to prevent, and it is worse than the
#      #UD because it cannot announce itself.
#   3. A VEX instruction in the kernel is also clobbered by any task switch at
#      all, since the kernel shares one CPU with the tasks.
#
# Note what is deliberately NOT here: -mxsave/-mxsaveopt. Enabling those lets
# the compiler emit XSAVE/XRSTOR for its own spills, which would change the
# meaning of a task's state area from "whatever context.S wrote into it" to
# "whatever the compiler's XSAVE header says is valid", and the two are not
# reconciled anywhere.
#
# So the rule is not "avoid vector code because it is slower". It is "no
# instruction may write register state that nothing on the kernel side saves
# and restores across a context switch".
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
#
# The check is narrower than the policy, in a way worth knowing before relying
# on it. `verify-isa` runs from the kernel.elf rule only (Makefile:198), so it
# sees build/kernel.elf and nothing else -- not init.elf, not libc.a. And it
# exempts fxsave/fxrstor on the grounds that they "move 512 bytes without
# interpreting any of it", which is true and is also the whole problem: the
# 512 bytes are the entire contract, and nothing in the build verifies that the
# contract still covers what the code needs saved. Today it does, because the
# kernel emits no vector instruction at all -- the exemption is unreachable in
# the sense that no other vector state exists to lose. Both halves of that
# sentence are load-bearing and neither is enforced.
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
# Userspace is compiled as ordinary non-PIE position-dependent code: the kernel
# supplies the entry point and the initial stack, so we keep full control of the
# process image layout.
#
# Userspace does NOT target the same ISA as the kernel, and the difference is
# the whole point of this block.
#
# The kernel is GPR-only, which means nothing in the kernel holds register state
# that a context switch has to carry. Userspace is preemptible and shares one
# CPU with the kernel, so its register state *is* carried: sched.c:931-933 calls
# fpu_save(prev->fpu_state) and fpu_restore(next->fpu_state) on every task
# switch, and those are FXSAVE/FXRSTOR -- the legacy 512-byte image, x87 plus
# XMM0-XMM15 plus MXCSR and nothing else.
#
# So the contract this flag list has to keep is:
#
#   XMM (128-bit)     LEGAL.  Covered by the FXSAVE image. Today's init.elf
#                     contains 46 movaps, 11 movups, 7 movdqa and a few more,
#                     and every one of those is restored correctly across a
#                     preemption.
#
#   YMM (256-bit)     NOT LEGAL. The upper 128 bits of YMM0-YMM15 are not in
#                     the FXSAVE layout. A task preempted holding them gets
#                     them silently overwritten by the next task to run -- no
#                     fault, no log line, no way to notice except by noticing
#                     that a computation came out wrong. Only XSAVE/XRSTOR with
#                     the YMM_Hi128 component set in XCR0 can carry them, and
#                     nothing on the kernel side does that today.
#
#   ZMM/opmask        NOT LEGAL, same reason plus AVX-512 needs components 5, 6
#                     and 7, which are not in the legacy layout at all.
#
# The x86-64 baseline already gives us SSE2, so XMM is legal without asking for
# it; what has to be forbidden is everything above it, and for the same reason
# the kernel's -mno- flags are last: on x86 the last -m flag for an ISA feature
# is the one that counts. `-march=x86-64-v3` is the realistic way this breaks,
# because it is the natural thing to add to a libc and it silently brings in
# AVX2, FMA and every YMM user with it.
#
# These four flags produce a byte-identical init.elf to the list without them
# (no VEX instruction is emitted by the current sources, verified by md5), so
# they are a statement of intent rather than a behaviour change. Unlike the
# kernel list, they are not backed by a build gate: `verify-isa` is invoked from
# the kernel.elf rule only (Makefile:198) and never looks at init.elf or
# libc.a. This list is therefore the *only* thing standing between a future
# -march= and silent YMM corruption.
USER_CFLAGS := \
	-std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
	-fno-pic -fno-pie -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fno-strict-aliasing -fno-common -m64 -mno-red-zone -mcmodel=small \
	-O2 -g3 -Wall -Wextra -Wno-unused-parameter \
	-mbmi -mbmi2 -mpopcnt \
	-I src/include -I src/libc/include \
	-mno-avx -mno-avx2 -mno-fma -mno-f16c

# -static keeps the user image free of an interpreter: the kernel's ELF loader
# maps PT_LOAD segments and jumps to e_entry. There is no dynamic linker yet.
USER_LDFLAGS := -no-pie -nostdlib -static -z noexecstack -Wl,--build-id=none

QEMU := qemu-system-x86_64
