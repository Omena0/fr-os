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

# The serial log. Named here because the QEMU arguments below need it and the
# reporting at the end needs it, and a path written in three places is a path
# that will eventually disagree with itself.
LOG="${LOG:-build/serial.log}"
mkdir -p "$(dirname "$LOG")"

# A bounded run.
#
# The kernel has no shutdown path yet, so QEMU never exits on its own and this
# script would block a terminal for as long as the user cares to wait. The
# failure mode that matters more than the hang is the misdiagnosis: a kernel
# that triple-faults into a halt looks exactly like a kernel that is booting
# slowly, and the only way to tell them apart from outside is to stop and read
# the serial log. Without a bound there is no "stop and read".
#
# The signal is SIGTERM, never the timeout(1) default of SIGKILL. QEMU's
# chardev writes are asynchronous: SIGKILL takes the process down with bytes
# still in the write buffer, so the log is truncated at an arbitrary point and
# reads as a hang at the last line printed rather than as whatever actually
# happened. SIGTERM lets QEMU drain and close the file. Losing the tail of a
# serial log has cost this project more than one wrong conclusion.
#
# Override with RUN_TIMEOUT=0 for an unbounded interactive run, or
# RUN_TIMEOUT=60 for a slower machine. SIGINT (^C) is handled the same way, so
# interrupting the script still produces a complete log.
RUN_TIMEOUT="${RUN_TIMEOUT:-30}"

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
#
# This is not just a speed choice, and the difference is not cosmetic. The
# kernel writes MSR_GS_KERNEL_BASE (0xC0000102) unconditionally at the top of
# kmain. TCG's -cpu max accepts it; KVM with -cpu host passes the MSR through
# to hardware, where it is Intel-only, so on this AMD host the wrmsr takes a #GP
# at 0xffffffff80000196 -- 22 instructions into the kernel, before any banner.
# Same image, same flags, and the only difference is which CPU model QEMU was
# told to present.
#
# That is a kernel bug and it belongs to whoever owns kmain.S, not here. What
# belongs here is that the run script does not quietly pick a configuration that
# changes the result. FORCE_TCG=1 pins TCG, which is what the reference boot
# command uses and what every measurement on this project has been taken with.
: "${FORCE_TCG:=}"
: "${USE_KVM:=}"

# KVM is opt-in, not automatic.
#
# With `-enable-kvm -cpu host` the kernel does not get past its first
# instruction: `wrmsr` to IA32_GS_BASE (0xC0000101) raises #GP and the machine
# lands in the bootstrap reporter. Under TCG with `-cpu max` the same image
# boots and runs. That is reproducible here, and the loader has already proved
# long mode is genuinely active at that point -- it read EFER.LMA back as set --
# so this is not the loader handing over in the wrong mode.
#
# Writing IA32_GS_BASE at CPL 0 in long mode is unconditional on x86-64 and
# there is no known reason for it to fault, so the cause is not understood.
# What *is* understood is that defaulting to KVM makes `./run.sh` fail on a
# machine where the same run under TCG works, which is the worst possible
# default: it turns an unexplained hardware-interaction question into "the
# project doesn't boot".
#
# So: TCG unless KVM is explicitly asked for, and say so either way.
if [ -n "$USE_KVM" ] && [ -z "$FORCE_TCG" ] && [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
	QEMU_ARGS+=(-enable-kvm -cpu host)
	echo "run.sh: USE_KVM=1 requested, using KVM (see the note in this file)" >&2
else
	if [ -n "$FORCE_TCG" ]; then
		echo "run.sh: FORCE_TCG set, using TCG" >&2
	elif [ -n "$USE_KVM" ]; then
		echo "run.sh: USE_KVM set but /dev/kvm is unusable, using TCG" >&2
	else
		echo "run.sh: using TCG; set USE_KVM=1 to try KVM (it currently #GPs in kmain.S)" >&2
	fi
	QEMU_ARGS+=(-cpu max)
fi

# Serial to the terminal and to a log file. The log is what a test or a bug
# report can be built from, because the framebuffer output is not recoverable
# once the window closes.
#
# Both sinks come from ONE COM1, via the chardev backend's own logfile= option,
# which writes a copy of everything passing through to a file. The two
# arrangements that do not work, both of which were tried here first:
#
#   -serial mon:stdio plus a separate -device isa-serial,chardev=...
#       installs two ISA serial devices. The guest's COM1 goes to whichever
#       QEMU bound first, so the log file stayed 0 bytes while the kernel
#       printed to the terminal -- the exact failure this script exists to
#       prevent, and it is silent.
#
#   -chardev mux,chardev=file,chardev=stdio
#       a mux is a multiplexer for *multiplexing protocols*, not a tee. It
#       forwards to the first backend that is ready, so the file backend never
#       receives anything. Also 0 bytes.
#
# logfile= is the documented way to get both, and it is a copy rather than a
# second consumer, so neither sink can starve the other.
#
# signal=off stops QEMU installing its own Ctrl-C handler, so ^C reaches this
# script and takes the drain path below instead of killing QEMU mid-write and
# truncating the log. chardev stdio is used rather than `-serial mon:stdio`
# because mon: multiplexes the monitor onto the same stream, which corrupts the
# log with escape sequences whenever a key is pressed.
QEMU_ARGS+=(
	-chardev stdio,id=ser0,signal=off,logfile="$LOG"
	-serial chardev:ser0
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
: > "$LOG"

# The watchdog, and the log tail.
#
# qemu is deliberately not exec'd: the shell has to stay alive in order to time
# the run out and then report on it, and exec would replace it. QEMU runs in
# the background and is waited on by PID, so the signal reaches QEMU itself.
#
# The timer is a real watchdog rather than `timeout qemu ...` for one reason:
# `timeout` puts itself between the signal and the process, and it forwards
# SIGTERM in a way that races QEMU's own shutdown. Sending it from here, after
# the wait has been interrupted, keeps the ordering explicit -- stop, let QEMU
# drain, then read the log.
#
# The wait loop is what makes the drain reliable. `wait` returns as soon as it
# is interrupted, but QEMU still has buffered serial bytes to write, so reading
# the log immediately would reintroduce exactly the truncation this arrangement
# exists to prevent. The poll below gives QEMU a bounded window to close the
# chardev, and stops early the moment it is actually gone.

qemu_pid=""
watchdog_pid=""

stop_qemu() {
	[ -n "$qemu_pid" ] || return 0
	kill -0 "$qemu_pid" 2>/dev/null || return 0
	kill -TERM "$qemu_pid" 2>/dev/null || true
}

cleanup() {
	[ -n "$watchdog_pid" ] && kill "$watchdog_pid" 2>/dev/null
	watchdog_pid=""
	stop_qemu
	return 0
}
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap cleanup EXIT

qemu-system-x86_64 "${QEMU_ARGS[@]}" "$@" &
qemu_pid=$!

# Sleep in the background so the wait below is interruptible. Using a subshell
# rather than `&` on a compound command keeps $! pointing at the sleep, which
# is what has to be killed on the way out.
if [ "$RUN_TIMEOUT" -gt 0 ] 2>/dev/null; then
	( sleep "$RUN_TIMEOUT"; kill -TERM "$qemu_pid" 2>/dev/null || true ) &
	watchdog_pid=$!
fi

timed_out=0
while :; do
	if wait "$qemu_pid"; then
		status=0
	else
		status=$?
	fi

	# QEMU handles SIGTERM itself and exits 0, so a watchdog stop and a clean
	# shutdown are indistinguishable by exit code alone -- the watchdog sets a
	# flag when it fires and that flag is what distinguishes them.
	#
	# The test is written as a full if rather than `[ x ] && y=1` on purpose.
	# Under `set -e` a false `[ ... ]` on the left of && makes the whole list
	# return non-zero, and because the list is a statement in its own right the
	# shell exits on it -- which is how a successful run ends up reporting
	# status 1.
	if [ "$status" -eq 143 ] || [ "$status" -eq 130 ]; then
		timed_out=1
	elif [ -n "$watchdog_pid" ] && ! kill -0 "$watchdog_pid" 2>/dev/null; then
		# The watchdog has fired and QEMU has already collected the signal.
		timed_out=1
	fi

	# Whether QEMU exited on its own or was asked to stop, give it a bounded
	# window to flush the chardev before the log is read. Reading immediately
	# after `wait` returns is what truncates a serial log.
	for _ in $(seq 1 50); do
		kill -0 "$qemu_pid" 2>/dev/null || break
		sleep 0.1
	done
	stop_qemu
	wait "$qemu_pid" 2>/dev/null || true
	break
done

cleanup
qemu_pid=""

if [ -s "$LOG" ]; then
	echo "run.sh: ---- $LOG (last 40 lines) ----" >&2
	tail -n 40 "$LOG" >&2
	echo "run.sh: ---- end of log, $(wc -l < "$LOG") lines total ----" >&2
else
	echo "run.sh: $LOG is empty -- the guest produced no serial output at all," >&2
	echo "        which usually means it died before stage2's first print" >&2
	echo "        (check that the image is the one you just built)" >&2
fi

if [ "$timed_out" -eq 1 ]; then
	echo "run.sh: stopped after ${RUN_TIMEOUT}s (SIGTERM, so the log above is complete)" >&2
	exit 0
fi
exit "$status"
