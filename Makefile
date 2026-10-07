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
# hello is Fr Userland's smoke test. It is built with the same flags as init
# and goes into the same initrd; nothing in the kernel runs it automatically,
# because there is no shell yet to type its name at.
HELLO_SRC := $(sort $(wildcard src/userspace/hello/*.c))
HELLO_ELF := build/hello.elf
INITRD := build/initrd.img

DISK := build/os.img

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
$(HELLO_ELF): $(HELLO_SRC) $(LIBC_OBJ) $(BUILD)/libc.a src/include/version.h \
             $(VERIFY_ISA_PY)
	@mkdir -p $(dir $@)
	$(HOST_CC_64) $(USER_CFLAGS) $(USER_LDFLAGS) -o $@ $(HELLO_SRC) \
		$(BUILD)/libc.a $(KERNEL_VERSION_DEFS)
	$(HOST_OBJCOPY) $(STRIP_DEBUG) $@
	@$(MAKE) --no-print-directory verify-isa PROFILE=user FILE=$@

$(INITRD): $(INIT_ELF) $(HELLO_ELF) tools/initrd.py
	@mkdir -p $(dir $@)
	python3 tools/initrd.py --out $@ --program init=$(INIT_ELF) \
		--program hello=$(HELLO_ELF)

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

# Enforce the ISA policy in config.mk instead of trusting it.
#
# The -mno-sse/-mno-sse2/-mno-avx flags in KERNEL_CFLAGS are a request to the
# compiler, not a guarantee: any -m flag appended after them wins, which is
# exactly how -msse4.2 and then -mavx2 each got vector code into a build that
# read as GPR-only. Checking the linked output rather than the flags is what
# makes this robust -- it does not care what order the flags are in, what a
# future edit appends, or whether an inline asm block in a .S file slipped a
# VEX encoding past the compiler entirely.
#
# So the rule is enforced in tools/verify_isa.py, on the linked artefact, and
# it is run against *both* images with two different profiles.
#
#   kernel   build/kernel.elf must be GPR-only: no x87, no XMM, no YMM, no
#            ZMM, no opmask, no XSAVE. fxsave/fxrstor are the two exemptions,
#            because context.S defines fpu_save/fpu_restore on them and the
#            scheduler calls them on every task switch.
#
#   user     build/init.elf and build/hello.elf may use XMM and may not use
#            anything wider. This is a second and deliberately weaker
#            invocation, not a reuse of the kernel one, and the difference is
#            the whole reason it is separate. The x86-64 baseline already
#            provides SSE2, and FXSAVE *does* preserve the 128-bit state that
#            legacy SSE uses: the image context.S saves is the legacy 512-byte
#            one, x87 plus XMM0-XMM15 plus MXCSR. A legacy SSE instruction in
#            userland is therefore carried across a preemption correctly. A
#            VEX or AVX-512 instruction is not -- its upper halves are not in
#            that image at all, so a preempted task loses them silently, with
#            no fault and nothing in the log.
#
# That second invocation is not redundant. USER_CFLAGS gained
# -mno-avx -mno-avx2 -mno-fma -mno-f16c precisely because -march=x86-64-v3
# would otherwise acquire AVX2 silently, and until now nothing checked the
# result. `make verify-isa-test` is what keeps this honest; see
# tests/isa/verify_isa_test.sh.
#
# The reason the two profiles exist at all is one sentence, and it is the
# sentence the old failure message got wrong: fpu_save/fpu_restore in
# context.S are FXSAVE/FXRSTOR, so any register state above 128 bits is
# destroyed by the next task switch without announcing it. On a CPU that
# reports AVX and OSXSAVE the loader sets XCR0 |= 7, so such an instruction
# executes happily first. The tool's error message says exactly that.
#
# The tool is a prerequisite of the three linked images, not of the objects.
# The gate reads the finished artefact, so a change to the gate has to
# re-validate the artefacts that exist rather than wait for an unrelated
# edit; it has nothing to say about how an object is compiled, so it must not
# invalidate one. That is the same split the .d files make.
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
# verify-isa-test is NOT in `all`. It checks the *checker*, not the tree, and it
# is not free: it runs a dozen compiles through both profiles. When it sat in
# front of $(DISK), a stale expectation inside it stopped the kernel being built
# at all -- a failure mode that is strictly worse than a missing check, because
# the thing being protected went unbuilt. `make check` runs it.
# verify-isa-test checks the *checker*, not the tree. It runs a dozen small
# compiles through both profiles to prove the classifier still recognises each
# instruction family it claims to. That is worth doing before a release and in
# CI, and it is not worth doing on every incremental build: a stale or wrong
# expectation inside the check itself would otherwise stop the kernel being
# built at all, which is the wrong failure mode for a test of a safety net.
#
# `make check` runs it. `make verify-isa-test` runs it directly.
#
# The gate that *does* gate every build is `verify-isa` itself, applied to the
# finished kernel.elf, init.elf and hello.elf from the link rules.
all: $(DISK)
	@echo "built $(DISK)"

# The boot smoke test: the only check here that boots anything at all, and the
# one that answers the question the ISA self-test cannot -- does the kernel
# still reach userspace. verify-isa-test compiles probes and reads
# disassembly; it never starts a CPU, so every boot regression in this tree so
# far has been found by a person reading a serial log by hand.
#
# It depends on $(DISK) and that dependency is not decoration. Twice in this
# project's history a boot log that stopped mid-way turned out to be an image
# make had not rebuilt, and it was read as a real bug. A check that builds
# what it is about to boot cannot make that mistake, and tests/boot_smoke.py
# refuses to run on an image older than its inputs as a second line of defence
# for the case where the script is run by hand.
#
# run.sh is reused for the QEMU invocation (IMAGE, LOG, HEADLESS=1, RUN_TIMEOUT
# are all set by the script); what the Makefile adds is the exit-code contract:
#
#   0   pass
#   77  skipped -- no QEMU, or no usable /dev/kvm. Deliberately not 0. A smoke
#       test that did not run has verified nothing, and a skipped test that
#       looks like a pass is worse than no test.
#   *   failed
#
# A skip is not a failure, so `check` still succeeds -- but it drops a marker
# and the check target turns that into a loud "not exercised" line instead of
# "all checks passed".
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

$(BUILD)/initrd-runtime.img: $(INIT_ELF) build/test-runner tools/initrd.py
	@mkdir -p $(dir $@)
	python3 tools/initrd.py --out $@ --program init=$(INIT_ELF) --program runtime-tests=build/test-runner

$(BUILD)/os-runtime.img: $(BUILD)/stage1.elf $(BUILD)/stage2.elf $(BUILD)/kernel.elf $(BUILD)/initrd-runtime.img tools/disk.py
	@echo "--stage1 build/stage1.bin \\"
	@echo "--stage2 build/stage2.bin \\"
	python3 tools/disk.py --stage1 build/stage1.bin \
		--stage2 build/stage2.bin \
		--kernel build/kernel.elf \
		--initrd $(BUILD)/initrd-runtime.img \
		--out $@

.PHONY: test-runtime
test-runtime: $(BUILD)/os-runtime.img build/test-runner
	@echo "=== Booting runtime tests ==="
	@FR_TMPDIR=$(BUILD) BOOT_SMOKE_TIMEOUT=60 python3 tests/boot_smoke.py --image $(BUILD)/os-runtime.img --run-tests
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
	@echo "make check      the ISA self-test plus the boot smoke test"
	@echo "make boot-smoke boot the image and assert the kernel reached init"
	@echo "make clean      remove build/"
	@echo "make verify-isa PROFILE=kernel|user FILE=<elf>   run the ISA gate by hand"
	@echo "make verify-isa-test          test the ISA gate itself"
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
