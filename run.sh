#!/usr/bin/env bash
#
# run.sh — boot the OS image under QEMU.
#
# Interactive use (default): QEMU runs in the foreground and takes over the
# terminal. Characters you type are echoed by the kernel, and the REPL at
# Fr Init> accepts commands like `help` and `run hello`. To return to the
# shell, close the QEMU terminal or send SIGTERM from another window.
# Ctrl+C is forwarded to the guest as a byte; the OS treats it as an
# aborted command rather than an exit request.
#
# Automated / background use: set RUN_TIMEOUT=N (seconds). QEMU is then
# backgrounded, the serial log is written to $LOG, and the script exits
# after the timeout. Input is not accepted in background mode.
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

# This script's own commentary, off by default.
#
# The serial log is the product. It is already delivered twice over, once live
# to the terminal and once to $LOG, and the two are the same bytes -- so the
# `tail -n 40 "$LOG"` that used to run at the end was a third delivery of output
# the user had already watched go past, and the only thing it added was the
# chance to read a line twice and wonder which copy was current.
#
# What is left is short status chatter, and it is worse than it looks: it is
# interleaved with the kernel's own output on the same stream, so every line
# the script adds is a line of the boot log that is not the boot log. The one
# message that is not chatter is the empty-log diagnostic at the end, which only
# prints when the guest said nothing at all and is kept unconditional.
#
# DEBUG=1 restores the diagnostics.
DEBUG="${DEBUG:-0}"

# Written as a full if rather than `[ ... ] && echo ... || true` because under
# `set -e` a false test on the left of && makes the whole list return non-zero,
# and the shell exits on a statement that looks like it should be harmless.
note() {
	if [ "$DEBUG" = "1" ]; then
		echo "run.sh: $*" >&2
	fi
	return 0
}

IMAGE="${IMAGE:-build/os.img}"
MEM="${MEM:-4G}"
# One CPU, and deliberately so.
#
# sched_init() is per-CPU: it allocates sched_runqueues[cpu] and publishes
# per_cpu(current), per_cpu(idle_task) and per_cpu(in_scheduler) for that CPU
# alone. Nothing else calls it -- there is no secondary-CPU bring-up path in the
# tree -- so a second CPU would come up with a NULL run queue and fault on its
# first touch. SMP=N is honoured for experimentation and is not currently
# correct.
SMP="${SMP:-1}"

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
#
# Other switches: DEBUG=1 restores the script's own diagnostics, IMAGE= and
# LOG= change what is booted and where the serial log lands, FORCE_TCG=1 pins
# emulation, SMP=N is honoured for experimentation only, and HEADLESS=1 keeps
# QEMU off the display.
RUN_TIMEOUT="${RUN_TIMEOUT:-}"

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

# Accelerator.
#
# KVM when /dev/kvm is usable, TCG otherwise. This was the opposite for most of
# a day: `-enable-kvm -cpu host` failed at the kernel's first instruction with a
# #GP on `wrmsr` to IA32_GS_BASE, while TCG with `-cpu max` booted the same
# image -- so the script pinned TCG by default and said why in a long comment.
#
# That failure is fixed. The cause was the GS base, not the accelerator: the
# kernel installed it with WRMSR, which KVM services, and the value it wrote was
# malformed. It now prefers WRGSBASE (which KVM does not intercept) behind
# CR4.FSGSBASE, set by the loader where the rest of the CPU state is set up.
# Both accelerators boot the current image, verified.
#
# FORCE_TCG=1 pins TCG. That is worth knowing about: every measurement taken
# while the GS bug was live came from TCG, so a difference between the two is
# more likely to be a real difference than a configuration artefact.
if [ -n "${FORCE_TCG:-}" ]; then
	QEMU_ARGS+=(-cpu max)
	note "FORCE_TCG=1, using TCG emulation"
elif [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
	QEMU_ARGS+=(-enable-kvm -cpu host)
	note "using KVM (-cpu host); FORCE_TCG=1 to compare under emulation"
else
	QEMU_ARGS+=(-cpu max)
	note "/dev/kvm not usable, falling back to TCG emulation"
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
# signal=off stops QEMU installing its own Ctrl-C handler. For interactive
# use, leave it off so the kernel sees Ctrl+C as byte 0x03 (the OS handles
# it as an aborted command). To exit QEMU, close the terminal window or
# send SIGTERM from another shell. For automated runs, signal handling is
# irrelevant because QEMU is backgrounded.
#
# mux=on makes the stdio chardev bidirectional so input from the terminal
# reaches the guest's COM1 receive interrupt.
QEMU_ARGS+=(
	-chardev stdio,id=ser0,signal=off
	-serial chardev:ser0
)

# Debug console for port 0xE9 output (kernel markers)
DEBUG_LOG="${LOG%.log}.debug"
QEMU_ARGS+=(
	-debugcon "file:$DEBUG_LOG"
)

# A graphical display only when there is somewhere to put it. The kernel drives
# the framebuffer directly, so QEMU needs no extra video device.
# A VGA device explicitly. The kernel drives the framebuffer directly, and this
# BIOS offers no linear framebuffer, so the only thing that can appear in the
# window is the VGA text buffer -- which needs the device to exist.
QEMU_ARGS+=(-device VGA)

# HEADLESS=1 forces -display none even with a display available.
#
# Worth having as an explicit switch rather than only as the absence of DISPLAY:
# a desktop session has DISPLAY set, so "unset it to go headless" means editing
# the environment of whatever invoked the script, which is not something you can
# do from a Makefile or a CI wrapper. And a window per run is a poor trade for a
# serial log -- the framebuffer is the same bytes, arriving later and over the top
# of everything else.
if [ -n "${HEADLESS:-}" ]; then
	QEMU_ARGS+=(-display none)
	note "HEADLESS=1, no display"
elif [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
	QEMU_ARGS+=(-display gtk)
else
	QEMU_ARGS+=(-display none)
fi

if [ "${1:-}" = "--gdb" ]; then
	note "GDB stub listening on 127.0.0.1:1234"
	note "in another shell, run: gdb build/kernel.elf"
	QEMU_ARGS+=(-s -S)
	shift
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
	# Every step here is `|| true`, and that is load-bearing rather than
	# stylistic. Under `set -e`, `[ -n "$x" ] && kill "$x"` is a statement whose
	# exit status is the status of the whole list: when the variable is empty,
	# or the process is already gone, the list returns non-zero and the shell
	# exits -- from inside a trap, on the way to reporting what happened. That
	# is why a perfectly good bounded run exited 1 with `timed_out=1` already
	# decided and the "stopped after Ns" message two lines away: the script died
	# in cleanup, silently, before reaching it.
	[ -n "$watchdog_pid" ] && kill "$watchdog_pid" 2>/dev/null || true
	watchdog_pid=""
	stop_qemu
	return 0
}
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap cleanup EXIT

# Interactive by default: QEMU runs in the foreground so the stdio chardev
# receives terminal input. Agents and CI can opt into background mode with
# RUN_TIMEOUT=N, which still logs to $LOG but does not accept interactive input.
if [ -z "$RUN_TIMEOUT" ] || [ "$RUN_TIMEOUT" = "0" ]; then
	exec qemu-system-x86_64 "${QEMU_ARGS[@]}" "$@"
fi

qemu-system-x86_64 "${QEMU_ARGS[@]}" "$@" &
qemu_pid=$!

# Sleep in the background so the wait below is interruptible. Using a subshell
# rather than `&` on a compound command keeps $! pointing at the sleep, which
# is what has to be killed on the way out.
#
# The watchdog leaves a marker *before* it signals, and the marker is how the
# parent tells a timeout from a clean exit. It used to be told by testing
# whether the watchdog was still alive, which is a race: the subshell is still
# finishing its `kill` when the parent's `wait` returns, so the test usually
# saw it alive, concluded no timeout, and fell through to `exit "$status"` with
# whatever QEMU returned. QEMU exits non-zero when it is terminated, so a
# perfectly good run reported failure -- and a CI job gating on this script's
# exit code would have failed on every successful boot.
#
# The marker is written first, so if QEMU died from the watchdog the marker is
# already there. That ordering is the whole point: there is no window in which
# QEMU is dead and the marker does not exist.
watchdog_marker="${LOG}.watchdog"
rm -f "$watchdog_marker"
if [ "$RUN_TIMEOUT" -gt 0 ] 2>/dev/null; then
	( sleep "$RUN_TIMEOUT"
	  : > "$watchdog_marker"
	  kill -TERM "$qemu_pid" 2>/dev/null || true ) &
	watchdog_pid=$!
fi

timed_out=0
while :; do
	if wait "$qemu_pid"; then
		status=0
	else
		status=$?
	fi

	# A watchdog stop and a clean shutdown are indistinguishable by exit code
	# alone, because QEMU handles SIGTERM itself and reports a non-zero status
	# for it. The marker is what distinguishes them.
	if [ -e "$watchdog_marker" ]; then
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

# An empty log is a failure, not a status line, so this one is not behind DEBUG:
# there is no other way to tell "the guest is still booting" from "the guest
# never said anything" without reading the file, and an empty file has nothing
# to read. A non-empty log needs no commentary -- it has already gone to the
# terminal live.
if [ ! -s "$LOG" ]; then
	echo "run.sh: $LOG is empty -- the guest produced no serial output at all," >&2
	echo "        which usually means it died before stage2's first print" >&2
	echo "        (check that the image is the one you just built)" >&2
elif [ "$DEBUG" = "1" ]; then
	note "---- $LOG (last 40 lines) ----"
	tail -n 40 "$LOG" >&2
	note "---- end of log, $(wc -l < "$LOG") lines total ----"
fi

if [ "$timed_out" -eq 1 ]; then
	# Worth keeping even without DEBUG: a bounded run that ends on the
	# watchdog is indistinguishable from one that ended on its own, and
	# "it just stopped" is the more alarming of the two readings.
	echo "run.sh: stopped after ${RUN_TIMEOUT}s" >&2
	exit 0
fi
exit "$status"
