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

# The first target in the file is the default goal, and without this it would
# be the first pattern rule -- a single stage1 object file. That builds happily,
# reports success, and produces nothing runnable.
.DEFAULT_GOAL := all

# ------------------------------------------------------------------ patterns --

# Every object in the kernel tree is compiled with the kernel flags. Using a
# pattern rule with a stem that spans directories is what makes that possible
# without writing one rule per subdirectory.
$(OBJ)/boot/%.S.o: src/boot/%.S
	@mkdir -p $(dir $@)
	$(HOST_GCC) $(BOOT_ASFLAGS) -c $< -o $@

$(OBJ)/boot/%.c.o: src/boot/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC_32) $(BOOT_CFLAGS) -c $< -o $@

$(OBJ)/kernel/%.S.o: src/kernel/%.S
	@mkdir -p $(dir $@)
	$(HOST_CC_64) -c -x assembler-with-cpp $(KERNEL_CFLAGS) -I src/kernel/include $< -o $@

$(OBJ)/kernel/drivers/%.c.o: src/kernel/drivers/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC_64) $(KERNEL_CFLAGS) $< -c -o $@

$(OBJ)/kernel/%.c.o: src/kernel/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC_64) $(KERNEL_CFLAGS) $< -c -o $@

$(OBJ)/libc/%.S.o: src/libc/%.S
	@mkdir -p $(dir $@)
	$(HOST_CC_64) -c -x assembler-with-cpp $(USER_CFLAGS) -I src/libc/include $< -o $@

$(OBJ)/libc/%.c.o: src/libc/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC_64) $(USER_CFLAGS) $< -c -o $@

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
$(INITRD): $(INIT_ELF)
	@mkdir -p $(dir $@)
	python3 tools/initrd.py --out $@ --program init=$(INIT_ELF)

$(INIT_ELF): $(INIT_MAIN_SRC) $(LIBC_OBJ) $(BUILD)/libc.a
	@mkdir -p $(dir $@)
	$(HOST_CC_64) $(USER_CFLAGS) $(USER_LDFLAGS) -o $@ $(INIT_MAIN_SRC) \
		$(BUILD)/libc.a $(KERNEL_VERSION_DEFS)

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
$(OBJ)/kernel/initrd.c.o: $(INITRD)
	@mkdir -p $(dir $@)
	python3 tools/bin2c.py $< init_image > build/initrd.c
	$(HOST_CC_64) $(KERNEL_CFLAGS) -c build/initrd.c -o $@
$(BUILD)/kernel.elf: $(KERNEL_ENTRY_OBJ) $(BUILD)/libk.a $(OBJ)/kernel/initrd.c.o src/kernel/link.ld
	@mkdir -p $(dir $@)
	$(HOST_LD_64) $(KERNEL_LDFLAGS) -o $@ $(KERNEL_ENTRY_OBJ) \
		$(BUILD)/libk.a $(OBJ)/kernel/initrd.c.o

$(DISK): $(BUILD)/stage1.elf $(BUILD)/stage2.elf $(BUILD)/kernel.elf $(INITRD)
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
.PHONY: deps
deps:
	@mkdir -p $(OBJ)
	@set -e; for f in $(KERNEL_C_SRC); do \
		o=$(patsubst src/%.c,$(OBJ)/%.c.o,$$f); \
		$(HOST_CC_64) $(KERNEL_CFLAGS) -MM -MT $$o -MF $$o.d $$f; \
	done
	@set -e; for f in $(LIBC_SRC); do \
		o=$(patsubst src/%.c,$(OBJ)/%.c.o,$$f); \
		$(HOST_CC_64) $(USER_CFLAGS) -MM -MT $$o -MF $$o.d $$f; \
	done
	@set -e; for f in $(STAGE2_SRC); do \
		o=$(patsubst src/%.c,$(OBJ)/%.c.o,$$f); \
		$(HOST_CC_32) $(BOOT_CFLAGS) -MM -MT $$o -MF $$o.d $$f; \
	done

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

-include $(shell find $(OBJ) -name '*.d' 2>/dev/null)
