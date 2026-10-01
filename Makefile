# Makefile — the OS build.
#
# Four artifacts come out of this build, in dependency order:
#
#   build/stage1.elf  512-byte MBR, assembled and linked
#   build/stage2.elf  32-bit protected-mode bootloader
#   build/kernel.elf  the 64-bit higher-half kernel
#   build/init.elf    a statically linked userspace program
#   build/os.img      the raw disk image, assembled by tools/disk.py
#
# The kernel and the userspace images are ELF rather than flat binaries because
# the kernel's own loader reads PT_LOAD segments. A flat binary would force the
# bootloader to invent a load address and a bss size, and those are precisely
# the things an ELF header already says.
#
# Every recipe is written to fail loudly. A build that produces a subtly wrong
# image is worse than no build, because the symptom shows up as a kernel fault
# hours later.

include src/config.mk

# --------------------------------------------------------------- source sets --

# The bootloader. stage1 must be linked separately: it is a 512-byte MBR with
# a fixed size, and pulling in the other objects would blow that budget with
# linker alignment rather than code.
# Object files carry the source suffix in their own name (foo.c.o, foo.S.o).
# A single pattern rule per language would otherwise try to compile foo.c and
# foo.S to the same object name, and the one that lost would be rebuilt on every
# invocation.
STAGE1_SRC := src/boot/stage1.S
STAGE1_OBJ := $(OBJ)/boot/stage1.S.o

STAGE2_SRC := src/boot/stage2_entry.S src/boot/stage2_long.S src/boot/stage2.c
STAGE2_OBJ := $(OBJ)/boot/stage2_entry.S.o $(OBJ)/boot/stage2_long.S.o \
              $(OBJ)/boot/stage2.c.o

# The kernel.
KERNEL_C_SRC := $(sort $(wildcard src/kernel/*.c)) \
                $(sort $(wildcard src/kernel/drivers/*.c))
KERNEL_S_SRC := $(sort $(wildcard src/kernel/*.S))
KERNEL_OBJ := $(patsubst src/%.c,$(OBJ)/%.c.o,$(KERNEL_C_SRC)) \
              $(patsubst src/%.S,$(OBJ)/%.S.o,$(KERNEL_S_SRC))
KERNEL_ENTRY_OBJ := $(OBJ)/kernel/kmain.S.o $(OBJ)/kernel/main.c.o

# The libc. Compiled like a kernel object but with the small code model and no
# -mcmodel=kernel, because user space has no negative address range to reach and
# asking for the kernel model would make every global access a relocation.
LIBC_SRC := $(sort $(wildcard src/libc/src/*.c))
LIBC_S_SRC := $(sort $(wildcard src/libc/src/*.S))
LIBC_OBJ := $(patsubst src/%.c,$(OBJ)/%.c.o,$(LIBC_SRC)) \
            $(patsubst src/%.S,$(OBJ)/%.S.o,$(LIBC_S_SRC))

# The initrd and the userspace programs. Only src/userspace/init is linked into
# the PID 1 image; every other program is a separate ELF placed in the initrd
# for the ramfs to expose once it exists. A wildcard here would link two `main`
# functions into one image, which the linker would resolve by picking one and
# discarding the other without saying which.
INIT_MAIN_SRC := $(sort $(wildcard src/userspace/init/*.c))
INIT_ELF := build/init.elf
INITRD := build/initrd.img

DISK := build/os.img

# The content digest of every input that can change what an object compiles to.
# Defined here, used as a prerequisite of every object rule below, and built
# further down. See the comment at its recipe for why this exists at all.
.PHONY: FORCE
FORCE:

BUILD_INPUTS := Makefile src/config.mk
HEADER_FILES := $(shell find src -name '*.h' 2>/dev/null | sort)
BUILD_INPUTS_STAMP := $(BUILD)/.build-inputs.stamp

# The stamp is a real prerequisite of every object, not an order-only one. That
# matters: an order-only prerequisite is never allowed to make its target out of
# date, which is exactly the behaviour being fixed here.

# The first target in the file is the default goal, and without this it would
# be the first pattern rule -- a single stage1 object file. That builds happily,
# reports success, and produces nothing runnable.
.DEFAULT_GOAL := all

# ------------------------------------------------------------------ patterns --

# Every object in the kernel tree is compiled with the kernel flags. Using a
# pattern rule with a stem that spans directories is what makes that possible
# without writing one rule per subdirectory.
#
# EVERY rule below passes -MMD -MP -MF $@.d, including the assembly ones. That
# is not a detail: an object compiled without it has no dependency information
# at all, so nothing that happens to a header it included can ever rebuild it.
# The .S files here are not exempt -- stage1.S and stage2_entry.S both
# #include "boot_layout.h", which is the single header that owns the physical
# memory map: the E820 buffer, the page tables, the landing zone, BOUNCE_ADDR
# and STACK32_ADDR. An object that cannot see it is an object that silently
# keeps the old map.
#
# This is the concrete mechanism behind "make did not rebuild after I edited a
# header". The dependency graph was not merely incomplete, it was absent for
# three of the four loader objects, and the ones that did have a .d were
# trusted. $(BUILD_INPUTS_STAMP) below is the other half of it.
$(OBJ)/boot/%.S.o: src/boot/%.S $(BUILD_INPUTS_STAMP)
	@mkdir -p $(dir $@)
	$(HOST_GCC) $(BOOT_ASFLAGS) -MMD -MP -MF $@.d -x assembler-with-cpp -c $< -o $@

$(OBJ)/boot/%.c.o: src/boot/%.c $(BUILD_INPUTS_STAMP)
	@mkdir -p $(dir $@)
	$(HOST_CC_32) $(BOOT_CFLAGS) -MMD -MP -MF $@.d -c $< -o $@

$(OBJ)/kernel/%.S.o: src/kernel/%.S $(BUILD_INPUTS_STAMP)
	@mkdir -p $(dir $@)
	$(HOST_CC_64) -MMD -MP -MF $@.d -c -x assembler-with-cpp $(KERNEL_CFLAGS) -I src/kernel/include $< -o $@

$(OBJ)/kernel/drivers/%.c.o: src/kernel/drivers/%.c $(BUILD_INPUTS_STAMP)
	@mkdir -p $(dir $@)
	$(HOST_CC_64) $(KERNEL_CFLAGS) -MMD -MP -MF $@.d $< -c -o $@

$(OBJ)/kernel/%.c.o: src/kernel/%.c $(BUILD_INPUTS_STAMP)
	@mkdir -p $(dir $@)
	$(HOST_CC_64) $(KERNEL_CFLAGS) -MMD -MP -MF $@.d $< -c -o $@

$(OBJ)/libc/%.S.o: src/libc/%.S $(BUILD_INPUTS_STAMP)
	@mkdir -p $(dir $@)
	$(HOST_CC_64) -MMD -MP -MF $@.d -c -x assembler-with-cpp $(USER_CFLAGS) -I src/libc/include $< -o $@

$(OBJ)/libc/%.c.o: src/libc/%.c $(BUILD_INPUTS_STAMP)
	@mkdir -p $(dir $@)
	$(HOST_CC_64) $(USER_CFLAGS) -MMD -MP -MF $@.d $< -c -o $@

# The 32-bit bootloader uses a separate linker and its own script.
$(BUILD)/stage1.elf: $(STAGE1_OBJ) src/boot/stage1.ld
	@mkdir -p $(dir $@)
	$(HOST_LD_32) -m elf_i386 -T src/boot/stage1.ld -o $@ $(STAGE1_OBJ) --build-id=none
	$(HOST_OBJCOPY) -O binary $@ $(BUILD)/stage1.bin
	@size=$$(stat -c%s $(BUILD)/stage1.bin); \
	 if [ "$$size" -ne 512 ]; then \
		echo "ERROR: stage1.bin is $$size bytes, the MBR must be exactly 512"; \
		exit 1; \
	 fi

$(BUILD)/stage2.elf: $(STAGE2_OBJ) src/boot/stage2.ld
	@mkdir -p $(dir $@)
	$(HOST_LD_32) -m elf_i386 -T src/boot/stage2.ld -o $@ $(STAGE2_OBJ) \
		--build-id=none -z noexecstack
	$(HOST_OBJCOPY) -O binary $@ $(BUILD)/stage2.bin

# The initrd is a flat container of (name, offset, size) records wrapping the
# userspace ELFs. It is built before the kernel because the kernel embeds it as
# a C array, and a 512-byte sector boundary is not a boundary a C string literal
# can be trusted to respect.
#
# The generating script is a prerequisite. tools/initrd.py decides the container
# format, so editing it changes the output bytes; without the dependency the
# blob would keep the old layout until something unrelated forced a rebuild.
$(INITRD): $(INIT_ELF) tools/initrd.py
	@mkdir -p $(dir $@)
	python3 tools/initrd.py --out $@ --program init=$(INIT_ELF)

# version.h supplies KERNEL_VERSION/KERNEL_GIT_REV/KERNEL_BUILD_STAMP, which
# reach the image as -D flags on the link line rather than through a header
# dependency, so nothing else would notice a change to it.
$(INIT_ELF): $(INIT_MAIN_SRC) $(LIBC_OBJ) $(BUILD)/libc.a src/include/version.h
	@mkdir -p $(dir $@)
	$(HOST_CC_64) $(USER_CFLAGS) $(USER_LDFLAGS) -o $@ $(INIT_MAIN_SRC) \
		$(BUILD)/libc.a $(KERNEL_VERSION_DEFS)
	@# Strip DWARF only. The symtab stays, so the kernel's ELF loader and any
	@# backtrace still resolve symbols by name; what goes is the .debug_*
	@# sections, which for this image are 91% of its bytes. That matters
	@# because the initrd is bin2c'd into the kernel's .rodata -- a PT_LOAD
	@# the loader reads -- so every byte here is a byte read from disk at
	@# boot and charged against the landing-zone budget. See config.mk.
	$(HOST_OBJCOPY) $(STRIP_DEBUG) $@

$(BUILD)/libc.a: $(LIBC_OBJ)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(HOST_AR) rcs $@ $^

# The kernel archive. Kept separate from libc so a kernel-only change does not
# relink the userspace images.
$(BUILD)/libk.a: $(KERNEL_OBJ)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(HOST_AR) rcs $@ $^

# The initrd blob is turned into a C array and compiled into the kernel. A
# separate translation unit keeps the 100 KB of hex out of the main build's
# dependency graph, so touching main.c does not force a rebuild of it.
$(OBJ)/kernel/initrd.c.o: $(INITRD) tools/bin2c.py
	@mkdir -p $(dir $@)
	python3 tools/bin2c.py $< init_image > build/initrd.c
	$(HOST_CC_64) $(KERNEL_CFLAGS) -c build/initrd.c -o $@
$(BUILD)/kernel.elf: $(KERNEL_ENTRY_OBJ) $(BUILD)/libk.a $(OBJ)/kernel/initrd.c.o src/kernel/link.ld
	@mkdir -p $(dir $@)
	$(HOST_LD_64) $(KERNEL_LDFLAGS) -o $@ $(KERNEL_ENTRY_OBJ) \
		$(BUILD)/libk.a $(OBJ)/kernel/initrd.c.o
	@$(MAKE) --no-print-directory verify-isa FILE=$@

# Enforce the ISA policy in config.mk instead of trusting it.
#
# The -mno-sse/-mno-sse2/-mno-avx flags in KERNEL_CFLAGS are a request to the
# compiler, not a guarantee: any -m flag appended after them wins, which is
# exactly how -msse4.2 and then -mavx2 each got vector code into a build that
# read as GPR-only. And a vector instruction in this kernel is not a slowdown,
# it is a #UD -- the YMM/XMM state is never enabled through XCR0 on the kernel
# entry path -- so the failure surfaces as a dead boot with no message.
#
# Checking the linked output rather than the flags is what makes this robust.
# It does not care what order the flags are in, what a future edit appends, or
# whether an inline asm block in a .S file slipped a VEX encoding past the
# compiler entirely. If one vector instruction reaches kernel.elf, the build
# stops here, where the error names the instruction, instead of at boot, where
# it does not.
#
# The list is the x86 vector and x87 state, plus the MMX registers. It is
# matched against mnemonics only, which is the right granularity: a false
# positive costs one investigation, a false negative costs the boot.
#
# The pattern lives in a variable rather than inline because make expands $ in
# recipes, and the regex is full of them. $$(...) would be needed everywhere and
# makes the expression unreadable; a variable keeps it literal and lets the
# recipe stay a single readable pipeline.
VECTOR_MNEMONIC_RE := ^(v[a-z0-9]+|movdq[au]|movap[au]|movup[au]|padd[busw]|psub[busw]|pxor|pand|por|pcmpeq[bdw]|punpck[a-z]+|pshuf[dbw]|unpck[a-z]+|adds[sd]|subs[sd]|muls[sd]|divs[sd]|sqrts[sd]|comis[sd]|ucomis[sd]|cvt[a-z0-9]+|movs[sd]|andnp[sd]|andp[sd]|orp[sd]|xorp[sd]|maxs[sd]|mins[sd]|haddp[sd]|emms|ldmxcsr|stmxcsr|fxsave|fxrstor|fld|fst|fmul|fdiv|fadd|fsub|fabs|fsqrt|fucom|fxch|fil[de]|fist[pt]?)$$

# Two instructions are exempt, and the exemption is exactly two.
#
# fxsave and fxrstor appear in the kernel on purpose: context.S defines
# fpu_save/fpu_restore, and the scheduler calls them on every task switch to
# carry a task's FPU image across. They are hand-written asm, not compiler
# output, and they are state-management rather than computation -- they move
# 512 bytes without interpreting any of it. The loader enables what they need
# (stage2_long.S clears CR0.EM, sets CR0.MP and CR4.OSFXSR unconditionally, and
# ORs 0x7 into XCR0 when the CPU has AVX), so they execute rather than fault.
#
# Nothing else is exempt. A compiler-emitted fld or cvtsi2sd is exactly the bug
# this check exists to catch, and allowing the whole x87 family to hide a
# hand-written one would defeat the point.
VECTOR_MNEMONIC_ALLOWED_RE := ^(fxsave|fxrstor)$$

.PHONY: verify-isa
verify-isa:
	@file=$${FILE:-$(BUILD)/kernel.elf}; \
	 if [ ! -f "$$file" ]; then echo "verify-isa: $$file does not exist" >&2; exit 1; fi; \
	 hits=$$(objdump -d "$$file" 2>/dev/null \
	   | grep -oP '\t\K[a-z][a-z0-9]*' \
	   | grep -E '$(VECTOR_MNEMONIC_RE)' \
	   | grep -Ev '$(VECTOR_MNEMONIC_ALLOWED_RE)' \
	   | sort | uniq -c | sort -rn); \
	 if [ -n "$$hits" ]; then \
	   echo "ERROR: vector/x87 instructions in $$file -- the kernel never" >&2; \
	   echo "       enables XCR0, so each of these is a #UD at boot:" >&2; \
	   echo "$$hits" | sed 's/^/         /' >&2; \
	   exit 1; \
	 fi; \
	 echo "verify-isa: $$file is GPR-only (fxsave/fxrstor exempt: deliberate FPU context save)"

# The image is a function of the four artifacts *and* of the script that lays
# them out: disk.py owns the LBA assignments, the sector budget checks and the
# image size, so a change to it changes build/os.img without any of its inputs
# changing. It is a prerequisite for the same reason link.ld is one for the
# kernel.
$(DISK): $(BUILD)/stage1.elf $(BUILD)/stage2.elf $(BUILD)/kernel.elf $(INITRD) tools/disk.py
	@mkdir -p $(dir $@)
	python3 tools/disk.py \
		--stage1 $(BUILD)/stage1.bin \
		--stage2 $(BUILD)/stage2.bin \
		--kernel $(BUILD)/kernel.elf \
		--initrd $(INITRD) \
		--out $@

# ----------------------------------------------------------------- top level --

# The version string comes from git when the tree is a checkout, and from the
# file mtime otherwise, so a build from a tarball still produces a stamp that
# distinguishes one build from the next.
KERNEL_VERSION := $(shell sed -n 's/^#define KERNEL_VERSION "\(.*\)"/\1/p' src/include/version.h | head -1)
KERNEL_GIT_REV := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)
KERNEL_BUILD_STAMP := $(shell date -u '+%Y-%m-%dT%H:%M:%SZ')
KERNEL_VERSION_DEFS := -DKERNEL_VERSION=\"$(KERNEL_VERSION)\" \
                       -DKERNEL_GIT_REV=\"$(KERNEL_GIT_REV)\" \
                       -DKERNEL_BUILD_STAMP=\"$(KERNEL_BUILD_STAMP)\"

.PHONY: all
all: $(DISK)
	@echo "built $(DISK)"

.PHONY: kernel
kernel: $(BUILD)/kernel.elf

.PHONY: init
init: $(INIT_ELF)

.PHONY: disk
disk: $(DISK)

# Header dependencies. The kernel's headers change rarely but change
# globally, so a plain .d file per object is enough and much cheaper than
# hand-maintaining the list.
#
# This target only exists for the case where someone has added a source file
# and wants its dependency file before the first build of that file. A normal
# build writes the .d as a side effect of compiling, because every pattern rule
# above passes -MMD -MP -MF. It used to be the only way a .d could appear,
# which is why a build started from a clean tree once had objects with no
# dependency information at all.
.PHONY: deps
deps:
	@mkdir -p $(OBJ)
	@set -e; for f in $(KERNEL_C_SRC) $(KERNEL_S_SRC); do \
		o=$(patsubst src/%,$(OBJ)/%.o,$$f); \
		if [ "$${f##*.}" = "c" ]; then \
			$(HOST_CC_64) $(KERNEL_CFLAGS) -MM -MT $$o -MF $$o.d $$f; \
		else \
			$(HOST_CC_64) -x assembler-with-cpp $(KERNEL_CFLAGS) -I src/kernel/include -MM -MT $$o -MF $$o.d $$f; \
		fi; \
	done
	@set -e; for f in $(LIBC_SRC) $(LIBC_S_SRC); do \
		o=$(patsubst src/%,$(OBJ)/%.o,$$f); \
		if [ "$${f##*.}" = "c" ]; then \
			$(HOST_CC_64) $(USER_CFLAGS) -MM -MT $$o -MF $$o.d $$f; \
		else \
			$(HOST_CC_64) -x assembler-with-cpp $(USER_CFLAGS) -I src/libc/include -MM -MT $$o -MF $$o.d $$f; \
		fi; \
	done
	@set -e; for f in $(STAGE2_SRC) $(STAGE1_SRC); do \
		o=$(patsubst src/%,$(OBJ)/%.o,$$f); \
		if [ "$${f##*.}" = "c" ]; then \
			$(HOST_CC_32) $(BOOT_CFLAGS) -MM -MT $$o -MF $$o.d $$f; \
		else \
			$(HOST_CC_32) -x assembler-with-cpp $(BOOT_ASFLAGS) -MM -MT $$o -MF $$o.d $$f; \
		fi; \
	done

# ------------------------------------------------- dependency self-healing ----
#
# make decides whether to rebuild a file by comparing timestamps, and that is
# the whole of its knowledge. It cannot tell "this object is up to date" from
# "this object looks up to date". The distinction is not academic here: a
# header whose mtime moves backwards -- an editor that preserves times, a
# tarball or cp -a that restores an old timestamp, a patch applied with a
# pinned clock, an rsync without --checksum -- leaves every dependent object
# looking current while holding the old value. The build then reports success
# and links a binary built from a header that no longer exists in that form.
#
# That is the second half of why "make did not rebuild after I edited a header"
# kept happening here even for the objects that did have a correct .d file: the
# .d file was right, and the timestamp comparison it feeds was not.
#
# The fix is to stop trusting the object's mtime against the header's and start
# comparing the thing that actually determines the object's contents: the text
# of the inputs. A single digest over every header plus the build
# configuration is cheap -- a few hundred files, a few hundred kilobytes -- and
# it is exact. Timestamps can lie; content cannot.
#
# The stamp is rewritten only when the digest changes, so a rebuild that
# touches no header does not cascade into recompiling the world. When an input
# does change, every object is rebuilt: wasteful but correct, and a redundant
# compile costs a second where a stale object has already cost hours.
#
# Makefile and src/config.mk are in the digest on purpose. They decide the
# compile command line, so a change to either invalidates every object by the
# same argument -- which is also what disposes of objects left behind by an
# older rule set, such as the three loader objects that used to be built with no
# dependency tracking at all.
#
# The stamp is a real file target with a real recipe, so it participates in the
# ordinary up-to-date logic. FORCE makes the recipe run on every invocation so
# the digest is always rechecked; the recipe leaves the file untouched when the
# digest is unchanged, which is what keeps the stamp from looking newer than
# every object on every single build.
$(BUILD_INPUTS_STAMP): FORCE
	@mkdir -p $(dir $@)
	@digest=$$(cat $(BUILD_INPUTS) $(HEADER_FILES) 2>/dev/null | sha256sum | cut -d' ' -f1); \
	 if [ -r $@ ] && [ "$$(cat $@)" = "$$digest" ]; then exit 0; fi; \
	 printf '%s\n' "$$digest" > $@.tmp && mv -f $@.tmp $@; \
	 echo "make: build inputs changed ($$digest), rebuilding every object"

.PHONY: clean
clean:
	rm -rf $(BUILD)

.PHONY: run
run: $(DISK)
	./run.sh

.PHONY: run-gdb
run-gdb: $(DISK)
	./run.sh --gdb

.PHONY: help
help:
	@echo "make            build build/os.img"
	@echo "make kernel     build the kernel ELF only"
	@echo "make run        boot the image under QEMU"
	@echo "make run-gdb    boot under QEMU with a GDB stub on :1234"
	@echo "make deps       regenerate header dependency files"
	@echo "make clean      remove build/"

# Pull in the per-object header dependencies. Every pattern rule above writes
# one next to its object, so this list is complete by construction and is
# derived from the object lists rather than from a `find` over the tree.
#
# The `find` this replaces was a correctness hazard, not just untidy. It ran at
# parse time, so it saw whatever .d files happened to exist when make started,
# and a .d written by a concurrent or previous build for an object no longer in
# any list was still included -- adding prerequisites to a target that nothing
# would build. More importantly, it silently found *nothing* on a tree whose
# object directory did not exist yet, which is the normal state after `make
# clean`, and reported success having checked nothing.
-include $(addsuffix .d,$(KERNEL_OBJ) $(LIBC_OBJ) $(STAGE2_OBJ) $(STAGE1_OBJ))
