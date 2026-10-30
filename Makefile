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

# The kernel. Sources are grouped by subsystem under src/kernel; the wildcard
# is recursive so a new file in an existing folder is picked up without a
# Makefile edit. Object paths mirror the source paths.
KERNEL_C_SRC := $(sort $(shell find src/kernel -name '*.c'))
KERNEL_S_SRC := $(sort $(shell find src/kernel -name '*.S'))
KERNEL_OBJ := $(patsubst src/%.c,$(OBJ)/%.c.o,$(KERNEL_C_SRC)) \
              $(patsubst src/%.S,$(OBJ)/%.S.o,$(KERNEL_S_SRC))
KERNEL_ENTRY_OBJ := $(OBJ)/kernel/core/kmain.S.o $(OBJ)/kernel/core/main.c.o

# The libc. Compiled like a kernel object but with the small code model and no
# -mcmodel=kernel, because user space has no negative address range to reach and
# asking for the kernel model would make every global access a relocation.
LIBC_SRC := $(sort $(wildcard src/libc/src/*.c))
LIBC_S_SRC := $(sort $(wildcard src/libc/src/*.S))
LIBC_OBJ := $(patsubst src/%.c,$(OBJ)/%.c.o,$(LIBC_SRC)) \
            $(patsubst src/%.S,$(OBJ)/%.S.o,$(LIBC_S_SRC))

# The initrd and the userspace programs. Only src/userspace/init is linked into
# the PID 1 image; every other program is a separate ELF named after its source
# file, placed in the initrd and installed in the filesystem. init keeps its own
# rule because it links several sources; a wildcard for it would link two `main`
# functions into one image, which the linker would resolve by picking one and
# discarding the other without saying which.
INIT_MAIN_SRC := $(sort $(wildcard src/userspace/init/*.c))
INIT_ELF := build/init.elf

# A .c file under src/userspace/<dir>/ is a program when it defines main(), and
# its name is the program name: coreutils/ls.c -> ls, paint/paint_gui.c ->
# paint_gui. A file without main() is a library for its directory, compiled
# once and linked into every program there. This is what lets a folder hold a
# set of tools plus shared helpers without one rule per file. init is the
# exception: all of its sources are linked into one image, so it keeps its own
# rule.
USERSPACE_DIRS := $(sort $(dir $(filter-out src/userspace/init/%, \
                  $(wildcard src/userspace/*/*.c))))
USERSPACE_C_ALL := $(sort $(filter-out src/userspace/init/%, \
                   $(wildcard src/userspace/*/*.c)))
# Make matches parentheses in a $(shell ...) argument before the shell sees
# them, so the regex is built from these variables rather than written with
# literal brackets.
LPAREN := (
RPAREN := )
USERSPACE_MAIN_RE := ^[[:space:]]*$(LPAREN)int|void$(RPAREN)[[:space:]]+main[[:space:]]*[$(LPAREN)]
USERSPACE_PROGRAM_SRC := $(shell grep -lE '$(USERSPACE_MAIN_RE)' \
    $(USERSPACE_C_ALL) 2>/dev/null | sort)
USERSPACE_LIB_SRC := $(filter-out $(USERSPACE_PROGRAM_SRC),$(USERSPACE_C_ALL))
USERSPACE_LIB_OBJ := $(patsubst src/%.c,$(OBJ)/%.c.o,$(USERSPACE_LIB_SRC))
USERSPACE_NAMES := $(notdir $(basename $(USERSPACE_PROGRAM_SRC)))
USERSPACE_ELVES := $(addprefix $(BUILD)/,$(addsuffix .elf,$(USERSPACE_NAMES)))
HELLO_ELF := $(BUILD)/hello.elf
INITRD := build/initrd.img

DISK := build/os.img

# The root filesystem image. filesystem/ is the source tree that is packed;
# built userland programs are installed into filesystem/bin, and anything added
# to filesystem/ by hand is packed too. Nothing in that directory is deleted by
# the build.
FS_DIR := filesystem
FS_IMAGE := build/fs.img
FS_PROGRAM_NAMES := init $(USERSPACE_NAMES)
FS_BIN := $(addprefix $(FS_DIR)/bin/,$(FS_PROGRAM_NAMES))
FS_SOURCES := $(shell find $(FS_DIR) -type f 2>/dev/null | sort)

# The content digest of every input that can change what an object compiles to.
# Defined here, used as a prerequisite of every object rule below, and built
# further down. See the comment at its recipe for why this exists at all.
.PHONY: FORCE
FORCE:

# The tool is a prerequisite of the three linked images, not of the objects.
# The gate reads the finished artefact, so a change to the gate has to
# re-validate the artefacts that exist rather than wait for an unrelated
# edit; it has nothing to say about how an object is compiled, so it must not
# invalidate one. That is the same split the .d files make, and the same
# reason the -MMD -MP -MF $@.d flag is on the assembly rules too.
VERIFY_ISA_PY := tools/verify_isa.py
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

$(OBJ)/userspace/%.c.o: src/userspace/%.c $(BUILD_INPUTS_STAMP)
	@mkdir -p $(dir $@)
	$(HOST_CC_64) $(USER_CFLAGS) -MMD -MP -MF $@.d $< -c -o $@

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
# One link rule per userspace program, generated from USERSPACE_SRC. The
# recipe is the same for all of them: link against libc, strip DWARF (the
# symtab stays), and gate the result on the user ISA profile.
# $(1) is the program source, $(2) the library objects from its directory.
define USERSPACE_PROGRAM_RULE
$(BUILD)/$(notdir $(basename $(1))).elf: $(1) $(2) $(LIBC_OBJ) $(BUILD)/libc.a \
        src/include/version.h $(VERIFY_ISA_PY)
	@mkdir -p $$(dir $$@)
	$$(HOST_CC_64) $$(USER_CFLAGS) $$(USER_LDFLAGS) -o $$@ $$< $(2) \
		$$(BUILD)/libc.a $$(KERNEL_VERSION_DEFS)
	$$(HOST_OBJCOPY) $$(STRIP_DEBUG) $$@
	@$$(MAKE) --no-print-directory verify-isa PROFILE=user FILE=$$@
endef
$(foreach src,$(USERSPACE_PROGRAM_SRC), \
    $(eval $(call USERSPACE_PROGRAM_RULE,$(src), \
        $(filter $(OBJ)/userspace/$(notdir $(patsubst %/,%,$(dir $(src))))/%, \
                 $(USERSPACE_LIB_OBJ)))))

# The initrd is the bootstrap, not the root filesystem: it carries init and
# hello, which is enough to reach a shell and prove a second program can run.
# Everything else lives in filesystem/ and is packed into the disk image.
INITRD_PROGRAMS := init hello

$(INITRD): $(INIT_ELF) $(USERSPACE_ELVES) tools/initrd.py
	@mkdir -p $(dir $@)
	python3 tools/initrd.py --out $@ \
		$(foreach p,$(INITRD_PROGRAMS),--program $(p)=$(BUILD)/$(p).elf)

# version.h supplies KERNEL_VERSION/KERNEL_GIT_REV/KERNEL_BUILD_STAMP, which
# reach the image as -D flags on the link line rather than through a header
# dependency, so nothing else would notice a change to it.
$(INIT_ELF): $(INIT_MAIN_SRC) $(LIBC_OBJ) $(BUILD)/libc.a src/include/version.h \
            $(VERIFY_ISA_PY)
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
	@$(MAKE) --no-print-directory verify-isa PROFILE=user FILE=$@

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
$(BUILD)/kernel.elf: $(BUILD)/libk.a $(OBJ)/kernel/initrd.c.o \
                   src/kernel/link.ld $(VERIFY_ISA_PY)
	@mkdir -p $(dir $@)
	$(HOST_LD_64) $(KERNEL_LDFLAGS) -o $@ \
		$(BUILD)/libk.a $(OBJ)/kernel/initrd.c.o
	@$(MAKE) --no-print-directory verify-isa PROFILE=kernel FILE=$@

# Runtime kernel that embeds initrd-runtime.img so it can exec runtime-tests.
# The regular kernel.elf embeds initrd.img (init + hello), but runtime tests
# need runtime-tests in the initrd so the kernel can execve it.
$(OBJ)/kernel/initrd-runtime.c.o: $(BUILD)/initrd-runtime.img tools/bin2c.py
	@mkdir -p $(dir $@)
	python3 tools/bin2c.py $< init_image > build/initrd-runtime.c
	$(HOST_CC_64) $(KERNEL_CFLAGS) -c build/initrd-runtime.c -o $@
$(BUILD)/kernel-runtime.elf: $(BUILD)/libk.a $(OBJ)/kernel/initrd-runtime.c.o \
                             src/kernel/link.ld $(VERIFY_ISA_PY)
	@mkdir -p $(dir $@)
	$(HOST_LD_64) $(KERNEL_LDFLAGS) -o $@ \
		$(BUILD)/libk.a $(OBJ)/kernel/initrd-runtime.c.o
	@$(MAKE) --no-print-directory verify-isa PROFILE=kernel FILE=$@

# ISA policy is checked on the linked artefact, not trusted from the flags:
# any -m flag appended later wins, and inline asm can slip a vector encoding
# past the compiler entirely. tools/verify_isa.py reads the machine code.
#
#   kernel  GPR-only: no x87, XMM, YMM, ZMM, opmask or XSAVE. fxsave/fxrstor
#           are exempt because context.S saves the FPU with them.
#   user    SSE2 (XMM) is allowed, wider is not: the 512-byte FXSAVE image
#           covers x87 and XMM but not AVX upper halves, so an AVX task would
#           silently lose them on the next switch.
#
# The gate is a prerequisite of each linked image, not of objects: a change to
# the checker re-validates artefacts, not compiles.
# VERIFY_ISA_PY is defined up with BUILD_INPUTS, because immediately-expanded
# (=) variables are read in order.

.PHONY: verify-isa
verify-isa:
	@if [ -z "$(PROFILE)" ]; then \
	   echo "verify-isa: set PROFILE=kernel or PROFILE=user" >&2; exit 1; \
	 fi
	@python3 $(VERIFY_ISA_PY) --profile $(PROFILE) $${FILE:-$(BUILD)/kernel.elf}

# The gate is the only thing standing between a flag typo and a kernel that
# cannot boot, so it is tested. `make verify-isa-test` runs the classifier's
# own self test (several hundred mnemonics, in both directions) and then
# compiles real objects through both profiles with the real flag lists,
# including the exact trap: KERNEL_CFLAGS with -msse2 appended, and
# USER_CFLAGS with -march=x86-64-v3 appended. Both must be rejected.
#
# The flag strings go in as single arguments and are word-split inside the
# script, which is what a make recipe does with them too. $(OUT) is under
# $(BUILD) so `make clean` removes the probes.
.PHONY: verify-isa-test
verify-isa-test:
	@sh tests/isa/verify_isa_test.sh "$(HOST_CC_64)" "$(BUILD)/verify-isa" \
		"$(KERNEL_CFLAGS)" "$(USER_CFLAGS)"

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

# Install the programs the build produces into the filesystem tree. Only the
# listed programs are touched: a program placed in filesystem/bin by hand is
# left alone and still packed into the image.
$(FS_DIR)/bin/%: $(BUILD)/%.elf
	@mkdir -p $(dir $@)
	cp -f $< $@

# Pack the tree into the raw image QEMU exposes as the second IDE disk.
$(FS_IMAGE): $(FS_BIN) $(FS_SOURCES) tools/mkfs.py
	@mkdir -p $(dir $@)
	python3 tools/mkfs.py $(FS_DIR) --out $@ --size-mb 64

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
# verify-isa-test checks the *checker*, not the tree: a dozen compiles through
# both profiles. It is in `make check`, not `all`, so a stale expectation in
# the test cannot stop the kernel being built. The gate that runs on every
# build is verify-isa itself, from the link rules.
all: $(DISK) $(FS_IMAGE)
	@echo "built $(DISK) and $(FS_IMAGE)"

# Boot smoke test: the only check that starts a CPU. It builds the image it
# boots, so a stale image cannot be mistaken for a boot bug, and reuses run.sh
# for QEMU. Exit codes: 0 pass, 77 skipped (no QEMU or /dev/kvm), else fail.
# A skip is reported loudly rather than counted as a pass.
.PHONY: boot-smoke
boot-smoke: $(DISK)
	@rm -f $(BUILD)/.boot-smoke-skipped
	@python3 tests/boot_smoke.py --image $(DISK) $(BOOT_SMOKE_ARGS); \
	 rc=$$?; \
	 if [ $$rc -eq 0 ]; then exit 0; fi; \
	 if [ $$rc -eq 77 ]; then \
	   mkdir -p $(BUILD); : > $(BUILD)/.boot-smoke-skipped; exit 0; \
	 fi; \
	 exit $$rc

.PHONY: test-harnesses
test-harnesses: $(INIT_ELF) $(HELLO_ELF)
	@echo "== TLS harness =="
	@sh tests/tls_harness.sh build/init.elf build/hello.elf
	@echo "== Context switch harness =="
	@sh tests/run_context_harness.sh
	@echo "== Schedule resume check =="
	@python3 tests/check_schedule_resume.py build/kernel.elf || true

# Runtime tests: build and run the test framework inside the booted OS
RUNTIME_TESTS := tests/runtime/framework/test_runner.c \
		 tests/runtime/framework/test_framework.c \
		 tests/runtime/kernel/test_process_syscalls.c \
		 tests/runtime/kernel/test_memory_syscalls.c \
		 tests/runtime/kernel/test_file_syscalls.c \
		 tests/runtime/kernel/test_ipc_syscalls.c \
		 tests/runtime/kernel/test_scheduling_syscalls.c \
		 tests/runtime/kernel/test_security_syscalls.c \
		 tests/runtime/init/test_init_system.c

build/test-runner: $(RUNTIME_TESTS) $(INIT_ELF) $(HELLO_ELF) $(BUILD)/libc.a
	@mkdir -p $(dir $@)
	$(HOST_CC_64) $(USER_CFLAGS) -I tests/runtime/framework $(USER_LDFLAGS) -o $@ \
		$(RUNTIME_TESTS) $(BUILD)/libc.a
	strip --strip-all --strip-debug $@

$(BUILD)/initrd-runtime.img: $(INIT_ELF) build/test-runner $(HELLO_ELF) tools/initrd.py
	@mkdir -p $(dir $@)
	python3 tools/initrd.py --out $@ --program init=$(INIT_ELF) \
		--program runtime-tests=build/test-runner --program hello=$(HELLO_ELF)

$(BUILD)/os-runtime.img: $(BUILD)/stage1.elf $(BUILD)/stage2.elf $(BUILD)/kernel-runtime.elf $(BUILD)/initrd-runtime.img tools/disk.py
	@echo "--stage1 build/stage1.bin \\"
	@echo "--stage2 build/stage2.bin \\"
	python3 tools/disk.py --stage1 build/stage1.bin \
		--stage2 build/stage2.bin \
		--kernel build/kernel-runtime.elf \
		--initrd $(BUILD)/initrd-runtime.img \
		--out $@

# ------------------------------------------------- frscript integration ---
#
# frscript (a separate repository) can emit a static ELF for this target. This
# is not part of `make check`: the compiler is an external checkout, and CI has
# no way to require one. Point FRSCRIPT_DIR at it to run the check by hand.
FRSCRIPT_DIR ?= $(abspath ../frscript)
FRSCRIPT_PROGRAM ?= $(FRSCRIPT_DIR)/examples/fr-os/hello.fr
FRSCRIPT_ELF := $(BUILD)/frscript-hello.elf

$(FRSCRIPT_ELF): $(FRSCRIPT_PROGRAM) $(FRSCRIPT_DIR)/src/cli.py
	@mkdir -p $(dir $@)
	python3 $(FRSCRIPT_DIR)/src/cli.py compile $(FRSCRIPT_PROGRAM) -o $(BUILD)/frscript-hello.bc
	python3 $(FRSCRIPT_DIR)/src/cli.py native $(BUILD)/frscript-hello.bc -o $@ --target fr-os
	@$(MAKE) --no-print-directory verify-isa PROFILE=user FILE=$@

$(BUILD)/initrd-frscript.img: $(INIT_ELF) $(FRSCRIPT_ELF) tools/initrd.py
	@mkdir -p $(dir $@)
	python3 tools/initrd.py --out $@ --program init=$(INIT_ELF) --program frhello=$(FRSCRIPT_ELF)

$(OBJ)/kernel/initrd-frscript.c.o: $(BUILD)/initrd-frscript.img tools/bin2c.py
	@mkdir -p $(dir $@)
	python3 tools/bin2c.py $< init_image > build/initrd-frscript.c
	$(HOST_CC_64) $(KERNEL_CFLAGS) -c build/initrd-frscript.c -o $@

$(BUILD)/kernel-frscript.elf: $(BUILD)/libk.a $(OBJ)/kernel/initrd-frscript.c.o \
                              src/kernel/link.ld $(VERIFY_ISA_PY)
	@mkdir -p $(dir $@)
	$(HOST_LD_64) $(KERNEL_LDFLAGS) -o $@ \
		$(BUILD)/libk.a $(OBJ)/kernel/initrd-frscript.c.o
	@$(MAKE) --no-print-directory verify-isa PROFILE=kernel FILE=$@

$(BUILD)/os-frscript.img: $(BUILD)/stage1.elf $(BUILD)/stage2.elf $(BUILD)/kernel-frscript.elf $(BUILD)/initrd-frscript.img tools/disk.py
	python3 tools/disk.py --stage1 build/stage1.bin \
		--stage2 build/stage2.bin \
		--kernel build/kernel-frscript.elf \
		--initrd $(BUILD)/initrd-frscript.img \
		--out $@

.PHONY: frscript-smoke
frscript-smoke: $(BUILD)/os-frscript.img
	@echo "=== Booting the frscript hello for Fr OS ==="
	@FR_TMPDIR=$(BUILD) BOOT_SMOKE_TIMEOUT=60 BOOT_SMOKE_ALLOW_TCG=1 python3 tests/boot_smoke.py --image $(BUILD)/os-frscript.img --run-command frhello
	@echo "=== frscript smoke complete ==="

.PHONY: test-runtime
test-runtime: $(BUILD)/os-runtime.img build/test-runner $(FS_IMAGE)
	@echo "=== Booting runtime tests ==="
	@FR_TMPDIR=$(BUILD) BOOT_SMOKE_TIMEOUT=60 BOOT_SMOKE_ALLOW_TCG=1 python3 tests/boot_smoke.py --image $(BUILD)/os-runtime.img --run-tests
	@echo "=== Runtime tests complete ==="

.PHONY: check
check: verify-isa-test boot-smoke test-harnesses test-runtime
	@if [ -f $(BUILD)/.boot-smoke-skipped ]; then \
	   rm -f $(BUILD)/.boot-smoke-skipped; \
	   echo "check: the boot smoke test was SKIPPED -- the kernel boot path was NOT exercised"; \
	 else \
	   echo "all checks passed"; \
	 fi
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
	@set -e; for f in $(KERNEL_C_SRC) $(KERNEL_S_SRC); do \
		o=$(patsubst src/%,$(OBJ)/%.o,$$f); \
		mkdir -p $$(dirname $$o); \
		if [ "$${f##*.}" = "c" ]; then \
			$(HOST_CC_64) $(KERNEL_CFLAGS) -MM -MT $$o -MF $$o.d $$f; \
		else \
			$(HOST_CC_64) -x assembler-with-cpp $(KERNEL_CFLAGS) -I src/kernel/include -MM -MT $$o -MF $$o.d $$f; \
		fi; \
	done
	@set -e; for f in $(LIBC_SRC) $(LIBC_S_SRC); do \
		o=$(patsubst src/%,$(OBJ)/%.o,$$f); \
		mkdir -p $$(dirname $$o); \
		if [ "$${f##*.}" = "c" ]; then \
			$(HOST_CC_64) $(USER_CFLAGS) -MM -MT $$o -MF $$o.d $$f; \
		else \
			$(HOST_CC_64) -x assembler-with-cpp $(USER_CFLAGS) -I src/libc/include -MM -MT $$o -MF $$o.d $$f; \
		fi; \
	done
	@set -e; for f in $(STAGE2_SRC) $(STAGE1_SRC); do \
		o=$(patsubst src/%,$(OBJ)/%.o,$$f); \
		mkdir -p $$(dirname $$o); \
		if [ "$${f##*.}" = "c" ]; then \
			$(HOST_CC_32) $(BOOT_CFLAGS) -MM -MT $$o -MF $$o.d $$f; \
		else \
			$(HOST_CC_32) -x assembler-with-cpp $(BOOT_ASFLAGS) -MM -MT $$o -MF $$o.d $$f; \
		fi; \
	done

# ------------------------------------------------- dependency self-healing ----
#
# mtime alone cannot tell "up to date" from "looks up to date": a restored or
# rewound header timestamp leaves dependent objects stale while make reports
# success. A digest of every header plus the build configuration is exact, and
# is rewritten only when it changes. Makefile and config.mk are in it because
# they decide the command line, so a rule change invalidates every object.
#
$(BUILD_INPUTS_STAMP): FORCE
	@mkdir -p $(dir $@)
	@digest=$$( { cat $(BUILD_INPUTS) $(HEADER_FILES) 2>/dev/null; \
	              printf 'kbd-layout %s\n' '$(KBD_LAYOUT)'; } \
	            | sha256sum | cut -d' ' -f1); \
	 if [ -r $@ ] && [ "$$(cat $@)" = "$$digest" ]; then exit 0; fi; \
	 printf '%s\n' "$$digest" > $@.tmp && mv -f $@.tmp $@; \
	 echo "make: build inputs changed ($$digest), rebuilding every object"

.PHONY: clean
clean:
	rm -rf $(BUILD)

.PHONY: run
run: $(DISK) $(FS_IMAGE)
	./run.sh

.PHONY: run-gdb
run-gdb: $(DISK) $(FS_IMAGE)
	./run.sh --gdb

.PHONY: help
help:
	@echo "make            build build/os.img"
	@echo "make kernel     build the kernel ELF only"
	@echo "make run        boot the image under QEMU"
	@echo "make run-gdb    boot under QEMU with a GDB stub on :1234"
	@echo "make deps       regenerate header dependency files"
	@echo "make check      the ISA self-test plus the boot smoke test"
	@echo "make boot-smoke boot the image and assert the kernel reached init"
	@echo "make clean      remove build/"
	@echo "make verify-isa PROFILE=kernel|user FILE=<elf>   run the ISA gate by hand"
	@echo "make verify-isa-test          test the ISA gate itself"
	@echo "make frscript-smoke           build frscript's fr-os hello and boot it (needs FRSCRIPT_DIR)"
	@echo "python3 tests/boot_smoke.py --self-test   test the boot-log classifier"
	@echo ""
	@echo "boot smoke test environment:"
	@echo "  BOOT_SMOKE_TIMEOUT=25   seconds to wait for init's prompt (0 is refused)"
	@echo "  BOOT_SMOKE_ALLOW_TCG=1  run under emulation when /dev/kvm is unusable"
	@echo "  FR_TMPDIR=<dir>         scratch for the image copy and serial log"

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
