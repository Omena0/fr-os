#!/usr/bin/env bash
#
# run.sh — boot the OS image under QEMU.
#
# The defaults are tuned for the reference machine (Intel Core Ultra 5 135H,
# Lunar Lake) but degrade gracefully: without KVM the same command runs under
# TCG, just slowly, and without an X display the display is dropped entirely
# rather than failing, so a headless build agent can still exercise the boot
# path through the serial port.
#
# The serial port is the primary observation channel, not a convenience. The
# framebuffer goes to a curses window that only exists on a developer's desktop,
# so a CI machine would otherwise produce a kernel that boots invisibly.

set -euo pipefail

cd "$(dirname "$0")"

IMAGE="${IMAGE:-build/os.img}"
MEM="${MEM:-4G}"
SMP="${SMP:-18}"

# acpi=off: the kernel has no ACPI or power-management support yet, so the
# machine's default ACPI tables would only produce interrupt storms the kernel
# cannot yet handle. QEMU has no standalone -no-acpi option; it is a property
# of the machine type, which is why it appears on -machine rather than here.
QEMU_ARGS=(
	-drive "file=${IMAGE},format=raw,if=ide,index=0,media=disk"
	-m "$MEM"
	-smp "$SMP"
	-machine "${MACHINE:-pc,acpi=off}"
	-no-reboot
)

# KVM when it is usable, TCG otherwise. -cpu host is only valid under KVM:
# under TCG the host CPU model does not exist and QEMU rejects it outright.
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
	QEMU_ARGS+=(-enable-kvm -cpu host)
else
	echo "run.sh: /dev/kvm unavailable, falling back to TCG emulation" >&2
	QEMU_ARGS+=(-cpu max)
fi

# Serial to the terminal and to a log file. The log is what a test or a bug
# report can be built from, because the framebuffer output is not recoverable
# once the window closes.
QEMU_ARGS+=(
	-serial mon:stdio
	-chardev file,id=serlog,path=build/serial.log
	-device isa-serial,chardev=serlog
)

# A graphical display only when there is somewhere to put it. The kernel drives
# the framebuffer directly, so QEMU needs no extra video device.
if [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
	QEMU_ARGS+=(-display gtk)
else
	QEMU_ARGS+=(-display none)
fi

if [ "${1:-}" = "--gdb" ]; then
	echo "run.sh: GDB stub listening on 127.0.0.1:1234" >&2
	echo "run.sh: in another shell, run: gdb build/kernel.elf" >&2
	QEMU_ARGS+=(-s -S)
fi

mkdir -p build
exec qemu-system-x86_64 "${QEMU_ARGS[@]}" "$@"
