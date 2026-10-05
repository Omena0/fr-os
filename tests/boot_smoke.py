#!/usr/bin/env python3
"""
boot_smoke.py -- boot the image and prove the kernel reached its init prompt.

Why this exists
---------------

`make check` runs a static ISA self-test. It compiles a dozen probes and
never boots anything, so every fact this project actually cares about -- that
PID 1 is created, that a ring-3 entry works, that init prints its banner --
was established by a person running QEMU and reading a serial log by hand.
All of it can regress silently, and a boot that half-succeeds and then panics
looks exactly like a boot that is merely slow, in a terminal, at 1am.

So: boot the image, read the serial log, and assert on the milestones.

What is asserted, and why each one earns its place
--------------------------------------------------

The chain is asserted *in order*, which is the part that carries the weight.
Presence alone would accept a log containing every string in any arrangement;
the order is what makes "the loader ran, the kernel came up, PID 1 was
created, the scheduler entered it, ring-3 code ran, init finished its banner"
a claim about one boot rather than a bag of substrings. Truncation, a
doubled buffer, or a stale log all break ordering or presence, and all three
have been read as healthy boots in this project.

  1 boot-mbr        "[boot] stage2 loaded"
        stage1 ran off the MBR and read stage2 off the disk. Nothing before
        this point is Fr OS at all.
  2 boot-stage2     "[boot2] running at 0x"
        stage2's main loop ran. Its absence localises a failure to stage1's
        disk read rather than to anything the kernel did.
  3 kernel-up       " Fr Core "
        the kernel reached its own banner in kmain. With 2 present and 3
        absent, the handover itself is what failed. Deliberately NOT the
        version, the build stamp, the git revision or the address lines that
        surround it: those change for reasons that have nothing to do with
        whether the boot worked.
  4 pid1-created    "init task created"
        PID 1's task struct exists.
  5 kernel-to-user  "init: pid 1 loaded, entering the scheduler"
        the kernel entered the scheduler with PID 1 as its first user task.
        This is the last thing the kernel says before handing the CPU away.
  6 userspace-alive "Fr Init: "
        ring-3 code executed. Those bytes were produced by user-mode
        instructions, so this -- and not milestone 5 -- is the evidence that
        the kernel reached userspace at all.
  7 init-at-prompt  "Fr Init: type 'help' for the command list, EOF to stop"
        init's banner is complete. The statement after this printf is
        fputs("Fr Init> ") followed by the blocking read, so this line
        having been printed and the process still running is "init reached
        its prompt".

        The literal prompt string is NOT asserted, and cannot be. stdout is
        _IOLBF (src/libc/src/stdio.c:83) and the prompt has no newline in
        it, so line buffering never flushes it; it sits in the buffer
        forever. Every healthy serial log in this project ends with milestone
        7 and no "Fr Init>", which is the buffering working, not a fault.
        Asserting the prompt would be asserting a kernel bug.

Note what is *not* here: "[boot2] entering long mode, jumping to kernel at",
the loader's last line, is a milestone this test used to assert and does not.
It says nothing milestones 2 and 3 do not already say between them -- the
kernel banner is only reachable through the loader's jump -- and it is
empirically the line the loader's log writer loses. See "the NUL runs"
below; the chain is not weakened by dropping it, it is one line shorter and
it no longer depends on a byte the loader is currently emitting wrongly.

And their absence:

  KERNEL PANIC                     src/kernel/panic.c:170
  #EXC                             src/kernel/idt.c:410
  unrecoverable page fault         src/kernel/idt.c:453
  could not create the init process  src/kernel/main.c:368
  [boot2] FATAL:                   src/boot/stage2.c:218

These are checked over the WHOLE log, not only up to the last milestone. A
boot that reaches init and then panics is the failure mode this test exists
for, and it is the one a milestone chain alone would report as a pass.

The NUL runs
------------

Both log writers -- the loader's serial_puts and the kernel's klog -- are
plain byte-at-a-time loops with no buffer, so a NUL byte in a serial log has
no legitimate source. There is a live defect producing them: runs of 3, 42, 48
and 250 NULs were measured at offsets 1084, 1594, 1712 and 5682 of one real
boot, each one swallowing the line or part of the line that followed it.
That is why this harness strips NULs before matching, and why every failure
report states how many NUL runs the log contained.

It matters for a second reason, and this one is a limit rather than a
convenience: **a corruption long enough to swallow a milestone can also
swallow a failure signature.** The report below says so explicitly rather
than letting a clean scan read as proof. The alternative -- asserting the
NULs are absent -- would make this test red for a defect in somebody else's
file that is already reported, which is how a test teaches people to ignore
it. The defect is somebody else's to fix and to test.

One honest caveat on #EXC: the handler reports *some* exceptions and returns
(idt.c:467 "delivering signal", idt.c:475, idt.c:503 "killing process"). On
today's init path none of those occur -- a demand-fill #PF returns at idt.c:451
without reporting -- so absence of #EXC is the right claim for this boot. If a
future userspace program is *meant* to take a fault the kernel reports and
recovers from, this check will need narrowing to the fatal action strings,
and that should be a deliberate edit rather than a discovery here.

Nothing volatile is asserted: no timestamps, no addresses, no PIDs beyond the
existence of PID 1, no build stamp, no git revision, no line count. Several of
those needles sit next to volatile text on the same line ("init task created,
entry 402100") and the needle is only the fixed part.

Exit status
-----------

  0   pass -- the kernel booted to init's prompt with no failure signature
  1   fail -- a milestone is missing, a failure signature is present, or the
             harness could not run the guest (stale image, QEMU refused to
             start, full filesystem, wrong path)
  77  skip -- no QEMU, or no usable /dev/kvm and TCG not permitted

77 is the automake/TAP convention for "skipped" and is deliberately distinct
from 0. A smoke test that did not run has verified nothing, and reporting it
as a pass is worse than not having it. `make check` turns 77 into a loud
"not exercised" line and never prints "all checks passed" in that case.

Usage
-----

    python3 tests/boot_smoke.py --image build/os.img
    python3 tests/boot_smoke.py --self-test     # test this classifier

Environment
-----------

  FR_TMPDIR       scratch directory for the image copy and the serial log.
                  Defaults to $TMPDIR, then to /home/omena0/.fr-tmp. Do not
                  point this at /tmp: it has hit its quota in this project,
                  and a full filesystem here produces a 0-byte serial log
                  and a zero exit code, which reads as a healthy guest.
  BOOT_SMOKE_TIMEOUT
                  wall-clock seconds to wait for the last milestone (25).
                  0 is refused, not honoured: an unbounded boot cannot fail,
                  it can only hang, and a test that cannot fail is not one.
  BOOT_SMOKE_ALLOW_TCG
                  set to 1 to run under TCG when /dev/kvm is unusable. TCG
                  boots this image (verified, ~0.3 s to the exec milestone
                  against ~0.8 s for KVM here), so the only reason to skip is
                  that an unaccelerated run is not wanted by default.
  FORCE_TCG=1     honoured by run.sh; pins TCG even when KVM works.
"""

import os
import re
import shutil
import signal
import subprocess
import sys
import time

EXIT_PASS = 0
EXIT_FAIL = 1
EXIT_SKIP = 77

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DEFAULT_TIMEOUT = 25.0

# DRAIN_TIMEOUT is how long this harness waits for run.sh to finish after it
# asks QEMU to stop. run.sh gives the chardev up to 5 s to drain before it
# exits; this is that wait, plus slack for a loaded machine. Reading the
# serial log before QEMU has closed it is how a complete boot gets read as a
# hang at the last line printed, so this wait is not optional politeness, and
# a run that blows through it says so rather than reporting a truncated log as
# a verdict.
DRAIN_TIMEOUT = 30.0


# --------------------------------------------------------------------------
# The assertions.
# --------------------------------------------------------------------------

# (id, needle, what its presence proves)
MILESTONES = (
    ("boot-mbr", "[boot] stage2 loaded",
     "stage1 ran off the MBR and read stage2 off the disk"),
    ("boot-stage2", "[boot2] running at 0x",
     "stage2's main loop ran"),
    ("kernel-up", " Fr Core ",
     "the kernel reached its own banner in kmain; with boot-stage2 present "
     "and this absent, the loader's handover is what failed"),
    ("pid1-created", "init task created",
     "PID 1's task struct was created"),
    ("kernel-to-user", "init: pid 1 loaded, entering the scheduler",
     "the kernel entered the scheduler with PID 1 as its first user task"),
    ("userspace-alive", "Fr Init: ",
     "ring-3 code executed: the kernel really reached userspace"),
    ("init-at-prompt",
     "Fr Init: type 'help' for the command list, EOF to stop",
     "init finished its banner; the next statement is the prompt and the "
     "blocking read"),
)

# (name, needle, source of truth)
SIGNATURES = (
    ("kernel panic", "KERNEL PANIC", "src/kernel/panic.c:170"),
    ("exception report", "#EXC", "src/kernel/idt.c:410"),
    ("unrecoverable page fault", "unrecoverable page fault",
     "src/kernel/idt.c:453"),
    ("init process creation failed", "could not create the init process",
     "src/kernel/main.c:368"),
    ("bootloader fatal", "[boot2] FATAL:", "src/boot/stage2.c:218"),
)


def load_text(data):
    """Decode a serial log for matching.

    NUL bytes are dropped, and deliberately so. The loader's log writer
    currently emits runs of NUL where a line's prefix should be (reported to
    the tree owner as a stage2.c defect), which can split a marker such as
    "[boot2] running at 0x" into "[boot2]\0\0\0 running at 0x". Matching on
    the corrupted bytes would fail this test for a reason that is not "the
    kernel is broken". The NULs are corruption, not content, so they are
    removed before matching -- and nothing here asserts that the corruption
    is absent, because that is somebody else's bug to fix and to test.
    """
    return data.replace(b"\x00", b"").decode("utf-8", "replace")


def nul_runs(data):
    """(count, total_nuls, longest) for the NUL runs in a raw serial log."""
    runs = [m.end() - m.start() for m in re.finditer(b"\x00+", data)]
    return len(runs), sum(runs), max(runs) if runs else 0


def scan(text):
    """Match the milestones in order and look for failure signatures.

    Returns (found, missing, signatures, present) where `found` is the list of
    matched (id, index) in order, `missing` is the first (id, needle, why) that
    did not appear at or after its predecessor's end, `signatures` is a list of
    (name, needle, line_number, line) for every failure signature present
    anywhere in the log, and `present` is the set of milestone ids found
    anywhere at all, ignoring order.

    `present` is what separates "the boot stopped here" from "this line was
    dropped". A milestone absent from `found` but present in `present` was
    printed somewhere the chain could not follow -- out of order, or, when the
    log is corrupt, nowhere near where it belongs. Reporting only the first of
    those two as "the kernel stopped here" has been how a log-writer defect
    was read as a kernel fault.

    This is a pure function of the log text, which is what makes it testable
    without booting anything -- see --self-test.
    """
    found = []
    missing = None
    pos = 0
    for mid, needle, why in MILESTONES:
        i = text.find(needle, pos)
        if i < 0:
            missing = (mid, needle, why)
            break
        found.append((mid, i))
        pos = i + len(needle)

    present = {mid for mid, needle, _w in MILESTONES if needle in text}

    signatures = []
    for name, needle, where in SIGNATURES:
        # The klog prefix is "<timestamp> cpuN <file>/<L>] " and every log
        # line carries a number that changes every boot, so the line number
        # is the only stable way to point a human at the offending line.
        at = text.find(needle)
        if at < 0:
            continue
        line_no = text.count("\n", 0, at) + 1
        line_start = text.rfind("\n", 0, at) + 1
        line_end = text.find("\n", at)
        if line_end < 0:
            line_end = len(text)
        signatures.append((name, needle, line_no,
                           text[line_start:line_end].strip()[:160]))

    return found, missing, signatures, present


# --------------------------------------------------------------------------
# Environment probes.
# --------------------------------------------------------------------------

def kvm_expected():
    """Whether run.sh will pick KVM for this machine, and why.

    This is deliberately the *same* coarse test run.sh uses -- /dev/kvm
    present and readable+writable -- rather than a stronger one, so the smoke
    test and `make run` agree about which accelerator they are on. Disagreeing
    with the code that actually launches the guest is how a harness ends up
    skipping on a machine that boots perfectly well.

    A stronger probe was written here first and thrown away: ioctl-ing
    /dev/kvm for KVM_GET_API_VERSION (0xAE00) is the textbook usability test,
    and on this machine it returns EINVAL for every KVM ioctl -- from Python
    and from C alike -- while `qemu-system-x86_64 -accel kvm -cpu host` boots
    this image and writes a complete serial log. So the ioctl is not a test
    this environment honours, and treating it as one would have made the smoke
    test skip on a machine that is fine. The accelerator's usability is now
    established from QEMU's own behaviour instead, in classify_startup().
    """
    if not os.path.exists("/dev/kvm"):
        return False, "/dev/kvm does not exist"
    if not (os.access("/dev/kvm", os.R_OK) and os.access("/dev/kvm", os.W_OK)):
        return False, "/dev/kvm is not readable and writable by this user"
    return True, "/dev/kvm is present and usable by run.sh's test"


def which_qemu():
    path = shutil.which("qemu-system-x86_64")
    return path, (None if path else "qemu-system-x86_64 is not on PATH")


def free_bytes(path):
    st = os.statvfs(path)
    return st.f_bavail * st.f_frsize


# --------------------------------------------------------------------------
# The stale-image guard.
# --------------------------------------------------------------------------
#
# Twice in this project's history a boot log that stopped mid-way turned out
# to be an image make had not rebuilt, and it was read as a real bug. The
# Makefile depends on $(DISK), which is the real fix: `make check` cannot run
# this without the image being up to date. This is the backstop for running
# the harness by hand, and it is an allowlist rather than a blocklist on
# purpose -- a blocklist over build/ has to exclude every scratch file ever
# written there (build/serial.log from `make run`, build/verify-isa/ from the
# ISA self-test, build/.build-inputs.stamp from the header digest), and each
# one added later is a false "stale image".

STALE_ARTEFACTS = ("libk.a", "libc.a", "initrd.img", "kernel.elf",
                   "stage1.elf", "stage2.elf")


def newest_input(root, image):
    """(newest_mtime, path) among the inputs the image is made of.

    An allowlist: every file under src/, every file under tools/ except Python
    bytecode caches, and the compiled objects plus the linked artefacts. If a
    future build grows a new output this misses it, which is the safe
    direction -- `make check` still gets the guarantee from the dependency.
    """
    newest = (0.0, None)
    img_real = os.path.realpath(image)

    def consider(path):
        nonlocal newest
        try:
            m = os.stat(path).st_mtime
        except OSError:
            return
        if m > newest[0]:
            newest = (m, path)

    for base in ("src", "tools"):
        top = os.path.join(root, base)
        for dirpath, dirnames, filenames in os.walk(top):
            dirnames[:] = [d for d in dirnames if d != "__pycache__"]
            for name in filenames:
                if name.endswith(".pyc"):
                    continue
                consider(os.path.join(dirpath, name))

    for dirpath, _dirnames, filenames in os.walk(os.path.join(root, "build", "obj")):
        for name in filenames:
            if name.endswith(".o") or name.endswith(".d"):
                consider(os.path.join(dirpath, name))

    for name in STALE_ARTEFACTS:
        consider(os.path.join(root, "build", name))

    if newest[1] and os.path.realpath(newest[1]) == img_real:
        newest = (0.0, None)
    return newest


def image_root(image):
    """The source tree an image belongs to, or None if it cannot be found.

    The image path is honoured all the way down rather than being assumed to
    be under this checkout's build/. An earlier version always compared
    against ROOT, so pointing --image at an image built from a different tree
    compared that image against *this* tree's sources and reported it stale
    when it was perfectly current -- a false failure caused by a bug in the
    check, which is the one kind of bug in a test that cannot be defended as
    "the test is being careful".
    """
    build_dir = os.path.dirname(os.path.abspath(image))
    if os.path.basename(build_dir) != "build":
        return None
    root = os.path.dirname(build_dir)
    if not os.path.isdir(os.path.join(root, "src")):
        return None
    return root


def image_is_fresh(root, image):
    """(fresh, detail). Fresh means: the image is not older than its inputs."""
    try:
        img_mtime = os.stat(image).st_mtime
    except OSError as exc:
        return False, "cannot stat the image %s: %s" % (image, exc)
    newest, path = newest_input(root, image)
    if path is None:
        return True, "no build inputs found under %s" % root
    if newest > img_mtime:
        return False, ("the image is older than its input %s "
                       "(image %.1fs, input %.1fs, %.1fs apart) -- this is the "
                       "stale-image trap, rebuild before reading anything into "
                       "this log"
                       % (os.path.relpath(path, root), img_mtime, newest,
                          newest - img_mtime))
    return True, "image is newer than every build input"


# --------------------------------------------------------------------------
# Running the guest.
# --------------------------------------------------------------------------

def direct_children(pid):
    """pids of the direct children of pid, without forking a helper."""
    try:
        with open("/proc/%d/task/%d/children" % (pid, pid)) as fh:
            return [int(x) for x in fh.read().split()]
    except OSError:
        pass
    out = []
    try:
        entries = os.listdir("/proc")
    except OSError:
        return out
    for entry in entries:
        if not entry.isdigit():
            continue
        try:
            with open("/proc/%s/stat" % entry) as fh:
                fields = fh.read().rsplit(")", 1)[1].split()
            if int(fields[1]) == pid:
                out.append(int(entry))
        except (OSError, IndexError, ValueError):
            continue
    return out


def find_qemu(run_sh_pid):
    for child in direct_children(run_sh_pid):
        try:
            with open("/proc/%d/comm" % child) as fh:
                if fh.read().startswith("qemu"):
                    return child
        except OSError:
            continue
    return None


def read_log(path):
    try:
        with open(path, "rb") as fh:
            return fh.read()
    except OSError:
        return b""


def run_boot(root, image, log, errlog, timeout, allow_tcg, extra_args):
    """Boot the image via run.sh and return a dict describing what happened.

    run.sh is reused rather than reimplemented. Its QEMU invocation is the one
    this project has debugged -- the SIGTERM-not-SIGKILL watchdog, the drain
    window, the chardev logfile, the -no-reboot that turns a triple fault into
    an exit instead of a loop -- and a second copy of that command line is a
    second thing to keep correct. What it also does that this test does not
    want is print the whole boot to stdout and default LOG to build/serial.log,
    which `make run` shares; both are redirected here rather than edited,
    because run.sh is not this test's file.

    The early stop is the same signal run.sh's own watchdog sends -- SIGTERM to
    the QEMU pid -- triggered when the last milestone appears instead of when
    the bound expires. That is what makes a passing run cost the boot (~1.5 s
    measured here) rather than the whole timeout. run.sh's watchdog is left in
    place as the backstop, so a bug in the polling loop costs time and not a
    hung test.
    """
    os.makedirs(os.path.dirname(log), exist_ok=True)
    for path in (log, errlog, log + ".watchdog"):
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass

    env = dict(os.environ)
    env.update({"IMAGE": image, "LOG": log, "HEADLESS": "1",
                # Strictly later than our own deadline: the watchdog is the
                # backstop for this loop failing, not the primary bound. Never
                # 0 -- run.sh documents that as the unbounded interactive run,
                # and inheriting it here would make CI able to hang forever.
                "RUN_TIMEOUT": str(int(timeout) + 10),
                "TMPDIR": scratch_dir(root)})
    if allow_tcg:
        env["FORCE_TCG"] = "1"

    started = time.time()
    with open(errlog, "wb") as errfh:
        proc = subprocess.Popen(["./run.sh"] + list(extra_args), cwd=root,
                                env=env, stdin=subprocess.DEVNULL,
                                stdout=subprocess.DEVNULL, stderr=errfh)
        qemu_pid = None
        # Why the poll loop stopped, recorded rather than inferred. An earlier
        # version inferred it from the watchdog marker and had three cases
        # where there are four, so a run cut short by this harness's own bound
        # was reported as having reached the last milestone. A diagnostic that
        # invents the reason for the run it is diagnosing is worse than none.
        stop_reason = "bound"
        try:
            while True:
                if time.time() - started > timeout:
                    stop_reason = "bound"
                    break
                if proc.poll() is not None:
                    stop_reason = "run_sh_exited"
                    break
                text = load_text(read_log(log))
                _found, missing, _sig, _pres = scan(text)
                if missing is None and _found:
                    stop_reason = "milestones"
                    break
                if qemu_pid is None:
                    qemu_pid = find_qemu(proc.pid)
                time.sleep(0.025)
        finally:
            # SIGTERM, always. QEMU's chardev writes are asynchronous and
            # SIGKILL drops the tail of the serial log, which then reads as a
            # hang at the last line printed rather than as what happened.
            if qemu_pid is None:
                qemu_pid = find_qemu(proc.pid)
            if qemu_pid:
                try:
                    os.kill(qemu_pid, signal.SIGTERM)
                except OSError:
                    pass
            try:
                proc.wait(timeout=DRAIN_TIMEOUT)
            except subprocess.TimeoutExpired:
                # run.sh is wedged. SIGKILL it -- the guest has already had its
                # SIGTERM, so QEMU has had its chance to drain, and this is
                # reported rather than hidden so the log below is known to be
                # suspect.
                proc.kill()
                proc.wait()
                timed_out_draining = True
            else:
                timed_out_draining = False

    elapsed = time.time() - started
    watchdog = os.path.exists(log + ".watchdog")
    try:
        with open(errlog, "rb") as fh:
            err = fh.read().decode("utf-8", "replace")
    except OSError:
        err = ""
    return {
        "elapsed": elapsed,
        "run_status": proc.returncode,
        # Three independent signals about how the run ended, because no single
        # one of them is the verdict -- the log is. run.sh exits 0 when its
        # watchdog stopped QEMU and non-zero when QEMU failed, which is why
        # its exit code cannot be used as a verdict: QEMU handles SIGTERM
        # itself and reports non-zero for it. `stop_reason` is what this
        # harness's own loop decided; `watchdog` is run.sh's independent view.
        "stop_reason": stop_reason,
        "watchdog": watchdog,
        "timed_out_draining": timed_out_draining,
        "stderr": err,
        "log_bytes": len(read_log(log)),
    }


def qemu_complaints(info):
    """The QEMU error lines in a run's stderr, excluding the normal SIGTERM.

    The "qemu-system-x86_64: " prefix is stripped. It is on every line QEMU
    writes and would otherwise contribute the word "x86" to every message,
    which is one of the two ways a message about anything at all can be
    mistaken for a message about the accelerator.
    """
    out = []
    for ln in info["stderr"].splitlines():
        if not ln.startswith("qemu-system-x86_64: "):
            continue
        if "terminating on signal" in ln:
            continue
        out.append(ln[len("qemu-system-x86_64: "):])
    return out


# An accelerator that will not start is an environment, not a kernel. These
# are matched against QEMU's own words rather than guessed at: the harness has
# to be able to say which of the two it found, and it can only do that from
# what QEMU printed.
#
# Matched against the message with the "qemu-system-x86_64: " prefix removed,
# and that removal is load-bearing rather than cosmetic. An earlier version
# matched "x86" and therefore matched *every* QEMU message ever printed --
# including "-serial chardev:s0: could not connect serial device to character
# backend", which is a wrong invocation and was being reported as an
# unusable accelerator, i.e. a skip. A classifier that turns a broken harness
# into a skip is worse than no classifier, because it hides the breakage in
# the one place the run was supposed to fail.
ACCEL_WORDS = ("kvm", "/dev/kvm", "accel", "accelerat")


def classify_startup(info):
    """'skip', 'fail' or None, for a run whose serial log is 0 bytes.

    A 0-byte log is never a healthy guest and never a kernel verdict either: it
    means the guest never produced a byte, so the only honest readings are
    "the machine could not run it" and "the harness got the invocation wrong".
    The two are told apart by what QEMU said.
    """
    complaints = qemu_complaints(info)
    blob = " ".join(complaints).lower()
    if complaints and any(word in blob for word in ACCEL_WORDS):
        return "skip", complaints
    return "fail", complaints


def scratch_dir(root):
    base = (os.environ.get("FR_TMPDIR") or os.environ.get("TMPDIR")
            or "/home/omena0/.fr-tmp")
    return os.path.join(base, "boot-smoke")


# --------------------------------------------------------------------------
# Reporting.
# --------------------------------------------------------------------------

def tail_lines(text, count):
    lines = [ln.rstrip() for ln in text.splitlines() if ln.strip()]
    return lines[-count:]


def corruption_note(raw):
    """One line about NUL runs in the raw log, or None.

    This is reported on every outcome, pass included. On a pass it says the
    "no failure signature" claim is weaker than it looks; on a failure it is
    the difference between "the kernel stopped here" and "the log writer ate
    the line".
    """
    count, total, longest = nul_runs(raw)
    if not count:
        return None
    return ("%d NUL run(s), %d bytes, longest %d -- the loader/klog log writer "
            "is emitting NULs where text should be (see the note at the top of "
            "this file); lines around them, milestones AND failure "
            "signatures, can be missing because of it"
            % (count, total, longest))


def report_fail(reason_lines, log, log_path, info, raw=b""):
    out = ["boot-smoke: FAIL"]
    out += ["  " + ln for ln in reason_lines]
    note = corruption_note(raw)
    if note:
        out.append("  NOTE: %s" % note)
    if info.get("timed_out_draining"):
        out.append("  WARNING: QEMU did not finish within %.0fs of being asked "
                   "to stop, so the log below may be truncated and this verdict "
                   "is less trustworthy than it looks"
                   % DRAIN_TIMEOUT)
    out.append("  serial log: %s (%d bytes)" % (log_path, info["log_bytes"]))
    # How the run ended, from the reason this harness's own poll loop stopped,
    # corroborated by run.sh's watchdog marker. Reporting the loop's own reason
    # rather than inferring it is what keeps "the bound expired" from being
    # printed as "the last milestone was reached" -- an earlier version had
    # three cases where there are four and made exactly that substitution.
    why = {
        "milestones": "this harness stopped it on the last milestone",
        "run_sh_exited": "run.sh was already gone (the guest stopped itself)",
        "bound": "the bound expired before the last milestone",
    }.get(info.get("stop_reason"), "stopped for an unrecorded reason")
    out.append("  run.sh exited %s; %s%s; %.1fs"
               % (info["run_status"], why,
                  "; run.sh's watchdog also fired" if info["watchdog"] else "",
                  info["elapsed"]))
    qemu_err = [ln for ln in info["stderr"].splitlines()
                if ln.startswith("qemu-system-x86_64: ")
                and "terminating on signal" not in ln]
    if qemu_err:
        out.append("  qemu said:")
        out += ["    " + ln for ln in qemu_err[:6]]
    tail = tail_lines(log, 12)
    if tail:
        out.append("  last lines of the log:")
        out += ["    " + ln[:150] for ln in tail]
    return "\n".join(out)


def evaluate(text, log_path, info, raw=b""):
    found, missing, signatures, present = scan(text)

    if signatures:
        lines = ["failure signature(s) in the serial log -- the boot reached "
                 "its prompt or nearly did and then broke:"]
        for name, _needle, line_no, line in signatures:
            lines.append("%s at line %d: %s" % (name, line_no, line))
        return report_fail(lines, text, log_path, info, raw)

    if missing is not None:
        mid, needle, why = missing
        n = len(found) + 1
        lines = [
            "milestone %d of %d never appeared: %s"
            % (n, len(MILESTONES), mid),
            "  expected in the serial log: %r" % needle,
            "  its absence means: %s" % why,
            "  milestones reached in order: %s"
            % (", ".join(f[0] for f in found) or "none"),
        ]
        # Milestones found somewhere but not followable in the chain. Naming
        # them is what stops "the boot stopped at milestone 3" being read as
        # "the kernel died there" when what happened is that a line was
        # written out of order or dropped.
        stray = [m[0] for m in MILESTONES if m[0] in present
                 and m[0] not in {f[0] for f in found}]
        if stray:
            lines.append("  present in the log but NOT in the chain: %s -- so "
                         "this is a log that is out of order or missing text, "
                         "not a boot that stopped here"
                         % ", ".join(stray))
        if not found and info["log_bytes"] == 0:
            lines.append("  the log is 0 bytes: that is a bad command, a full "
                         "filesystem or a wrong path, not a silent guest")
        return report_fail(lines, text, log_path, info, raw)

    return None


# --------------------------------------------------------------------------
# Self-test: the classifier, without booting anything.
# --------------------------------------------------------------------------

GOOD = """\r
[boot] stage1\r
[boot] stage2 loaded\r
[boot2] running at 0x0000000000008000, boot drive 0x0000000000000080\r
[boot2]   kernel loaded, 175873 bytes\r
[boot2] entering long mode, jumping to kernel at ffffffff80000180\r
=========================================================\r
 Fr Core 0.1.0 (Oct  5 2026 10:31:27, rev unknown)\r
 boot: entry 0xffffffff80000180, image 0x0000000000100000, drive 0x80\r
=========================================================\r
[  284.013 cpu0 idt/I] idt: 256 entries at ffffffff8002ec30, 4095 bytes\r
[  506.469 cpu0 sched/I] scheduler up on cpu 0, idle task pid 0\r
[  574.760 cpu0 process/I] init task created, entry 402020\r
init: pid 1 loaded, entering the scheduler\r
Fr Init: Fr Init 0.1.0 (build 2026-10-05T07:31:27Z, rev e0297e9)\r
Fr Init: Fr OS system bring-up -- pid 1, page size 4096 bytes\r
Fr Init: running on Fr Core 0.1.0\r
Fr Init: type 'help' for the command list, EOF to stop\r
[  630.549 cpu0 vmm/I] vmm: CR3 = 2a0000, direct map and kernel window live\r
"""


def self_test():
    failures = []
    total = [0]

    def check(name, cond, detail=""):
        total[0] += 1
        if cond:
            print("  ok   %s" % name)
        else:
            failures.append(name)
            print("  FAIL %s %s" % (name, detail))

    print("boot-smoke self-test: the classifier")

    text = load_text(GOOD.encode())
    found, missing, sigs, present = scan(text)
    check("a healthy log passes", missing is None and not sigs,
          "missing=%r sigs=%r" % (missing, sigs))
    check("all %d milestones matched in order" % len(MILESTONES),
          [f[0] for f in found] == [m[0] for m in MILESTONES])
    positions = [f[1] for f in found]
    check("milestone offsets are strictly increasing",
          positions == sorted(positions) and len(set(positions)) == len(positions))

    # Truncation at every point: the first missing milestone must be the one
    # that was cut. This is the property that makes the diagnostic useful --
    # "which milestone was missing" has to actually mean which.
    for cut in range(1, len(MILESTONES)):
        head = text[:found[cut - 1][1] + len(MILESTONES[cut - 1][1])]
        _f, miss, _s, _p = scan(head)
        want = MILESTONES[cut][0]
        check("truncated after %d reports %s" % (cut - 1, want),
              miss is not None and miss[0] == want,
              "got %r" % (miss[0] if miss else None))

    _f, miss, _s, _p = scan("")
    check("an empty log reports the first milestone missing",
          miss is not None and miss[0] == MILESTONES[0][0])

    # Every string present, one of them in the wrong place. Presence alone
    # accepts this; the ordered scan must not. The moved line is the last
    # milestone, placed before the two that precede it, so the scan is
    # expected to fail on exactly that one and to attribute it correctly.
    reordered = text.replace(
        "Fr Init: type 'help' for the command list, EOF to stop\r\n", "")
    reordered = reordered.replace(
        " Fr Core 0.1.0 (Oct  5 2026 10:31:27, rev unknown)\r\n",
        " Fr Core 0.1.0 (Oct  5 2026 10:31:27, rev unknown)\r\n"
        "Fr Init: type 'help' for the command list, EOF to stop\r\n")
    check("the reorder really did move it",
          reordered.find("EOF to stop") < reordered.find("init task created"))
    _f, miss, _s, present = scan(reordered)
    check("out-of-order milestones are rejected",
          miss is not None and miss[0] == "init-at-prompt",
          "got %r" % (miss[0] if miss else None))
    check("the out-of-order log still reports that milestone as printed",
          "init-at-prompt" in present, "present=%r" % sorted(present))

    # Every failure signature, inserted where a real one lands.
    for name, needle, _where in SIGNATURES:
        late = text + "\r\n" + ("some prefix | %s | and then it stops\r\n"
                                % needle)
        _f, miss, sigs, _p = scan(late)
        check("%s after the prompt still fails" % name,
              miss is None and any(s[0] == name for s in sigs),
              "missing=%r sigs=%r" % (miss, sigs))

    # The NUL-corruption case, sized from a real log out of this tree: three
    # runs of 3, 42 and 48 bytes at offsets 1084, 1594 and 1712, plus a
    # 250-byte one at 5682, each swallowing the text around it. A marker split
    # by NULs must still match; a line the writer ate must be named as eaten
    # rather than as "the boot stopped here"; and the corruption must not be
    # asserted on, because it is somebody else's live defect.
    corrupted = text.replace("[boot2] running at",
                            "[boot2]\x00\x00\x00 running at")
    _f, miss, _s, _p = scan(load_text(corrupted.encode()))
    check("NUL runs inside a marker do not hide it", miss is None,
          "got %r" % (miss[0] if miss else None))

    dropped = text.replace("[boot2] running at 0x", "[\x00" * 42)
    _f, miss, _s, _p = scan(load_text(dropped.encode()))
    check("a milestone eaten by NULs is still reported missing",
          miss is not None and miss[0] == "boot-stage2",
          "got %r" % (miss[0] if miss else None))
    check("a clean log reports no corruption",
          corruption_note(GOOD.encode()) is None)
    check("a corrupt log is announced, not blamed on the kernel",
          corruption_note(dropped.encode()) is not None)
    info = {"log_bytes": 1, "run_status": 0, "watchdog": True,
            "stop_reason": "bound", "timed_out_draining": False,
            "elapsed": 1.0, "stderr": ""}
    report = evaluate(load_text(dropped.encode()), "log", info,
                      dropped.encode())
    check("the failure report names the NUL runs",
          report is not None and "NUL run" in report)

    # The prompt string must NOT be required: it is line-buffered and never
    # reaches the port. A harness that required it would fail on every
    # healthy boot.
    _f, miss, _s, _p = scan(text.replace(
        "Fr Init: type 'help' for the command list, EOF to stop\r\n",
        "Fr Init: type 'help' for the command list, EOF to stop\r\n"))
    check("the newline-less prompt is not required", miss is None)
    check("no needle is a timestamp, address or revision",
          not any(re.search(r"\d\d:\d\d:\d\d|0x[0-9a-f]{8}|rev [0-9a-f]{7}",
                            needle) for _i, needle, _w in MILESTONES))

    # The 0-byte-log classifier: an accelerator that will not start is an
    # environment, anything else is a broken invocation, and the two must not
    # be confused. The second case is here because an earlier version of this
    # matched "x86" and so matched the program name on every QEMU message
    # ever printed -- which turned a wrong -serial into a skip.
    def startup(stderr):
        return {"log_bytes": 0, "run_status": 1, "watchdog": False,
                "stop_reason": "run_sh_exited", "timed_out_draining": False,
                "elapsed": 0.1, "stderr": stderr}

    check("a normal SIGTERM is not a complaint",
          qemu_complaints(startup(
              "qemu-system-x86_64: terminating on signal 15 from pid 1 (timeout)"
              "\nrun.sh: stopped after 25s\n")) == [])
    check("an unusable KVM is a skip",
          classify_startup(startup(
              "qemu-system-x86_64: Could not access KVM kernel module\n"))[0]
          == "skip")
    check("an absent /dev/kvm is a skip",
          classify_startup(startup(
              "qemu-system-x86_64: failed to initialize KVM: /dev/kvm\n"))[0]
          == "skip")
    check("a wrong -serial is a harness failure, not a skip",
          classify_startup(startup(
              "qemu-system-x86_64: -serial chardev:s0: could not connect "
              "serial device to character backend 'chardev:s0'\n"))[0] == "fail",
          "the program name alone must not read as an accelerator problem")
    check("a missing image is a harness failure, not a skip",
          classify_startup(startup(
              "qemu-system-x86_64: -drive file=x.img: Could not open "
              "'x.img': No such file or directory\n"))[0] == "fail")
    check("an image lock conflict is a harness failure, not a skip",
          classify_startup(startup(
              "qemu-system-x86_64: Failed to get \"write\" lock\n"
              "Is another process using the image [x.img]?\n"))[0] == "fail")

    # The stale-image guard, against a synthetic tree. Testing it against the
    # live one would be a self-test that fails whenever a parallel agent has an
    # unbuilt edit -- which is the guard working, not the test being wrong.
    fake = os.path.join(scratch_dir(ROOT), "staletest-%d" % os.getpid())
    try:
        os.makedirs(os.path.join(fake, "src", "kernel"), exist_ok=True)
        os.makedirs(os.path.join(fake, "build", "obj", "kernel"), exist_ok=True)
        img = os.path.join(fake, "build", "os.img")
        src = os.path.join(fake, "src", "kernel", "sched.c")
        obj = os.path.join(fake, "build", "obj", "kernel", "sched.c.o")
        for path in (src, obj, img):
            with open(path, "w") as fh:
                fh.write("x")
        os.utime(src, (900, 900))
        os.utime(obj, (1000, 1000))
        os.utime(img, (2000, 2000))
        fresh, detail = image_is_fresh(fake, img)
        check("an image newer than its inputs is accepted", fresh, detail)
        os.utime(src, (3000, 3000))
        fresh, detail = image_is_fresh(fake, img)
        check("an image older than a source is refused", not fresh)
        check("the refusal names the offending input",
              "src/kernel/sched.c" in detail and "stale-image trap" in detail,
              detail)
        os.utime(src, (500, 500))
        os.utime(img, (1000, 1000))
        os.unlink(obj)
        os.utime(img, (2000, 2000))
        fresh, _detail = image_is_fresh(fake, img)
        check("a source older than the image is accepted", fresh)
        fresh, detail = image_is_fresh(fake, os.path.join(fake, "nope.img"))
        check("a missing image is refused, not judged", not fresh, detail)
        check("a tree is found from <tree>/build/os.img",
              image_root(img) == fake)
        check("an image with no tree beside it is refused, not judged",
              image_root(os.path.join(scratch_dir(ROOT), "os.img")) is None)
    finally:
        shutil.rmtree(fake, ignore_errors=True)

    print("boot-smoke self-test: %d checks, %d failed"
          % (total[0], len(failures)))
    return EXIT_PASS if not failures else EXIT_FAIL


# --------------------------------------------------------------------------

def main(argv):
    if "--self-test" in argv:
        return self_test()

    image = "build/os.img"
    timeout_s = os.environ.get("BOOT_SMOKE_TIMEOUT", str(DEFAULT_TIMEOUT))
    allow_tcg = os.environ.get("BOOT_SMOKE_ALLOW_TCG", "") not in ("", "0")
    extra = []
    args = argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--image" and i + 1 < len(args):
            image = args[i + 1]
            i += 2
        else:
            extra.append(args[i])
            i += 1
    if not os.path.isabs(image):
        image = os.path.join(ROOT, image)

    try:
        timeout = float(timeout_s)
    except ValueError:
        print("boot-smoke: BOOT_SMOKE_TIMEOUT=%r is not a number; treating it "
              "as the default %.0f" % (timeout_s, DEFAULT_TIMEOUT), file=sys.stderr)
        timeout = DEFAULT_TIMEOUT
    if timeout <= 0:
        print("boot-smoke: REFUSING to run unbounded. BOOT_SMOKE_TIMEOUT=%s "
              "would make this test unable to fail, only able to hang. Use a "
              "positive bound (default %.0f)." % (timeout_s, DEFAULT_TIMEOUT),
              file=sys.stderr)
        return EXIT_FAIL

    # ---- skip: is there anything here that can run a guest at all? --------
    qemu, why = which_qemu()
    if not qemu:
        print("boot-smoke: SKIP -- %s" % why)
        print("           the kernel boot path was NOT exercised")
        return EXIT_SKIP

    kvm_ok, kvm_why = kvm_expected()
    if not kvm_ok and not allow_tcg:
        print("boot-smoke: SKIP -- %s" % kvm_why)
        print("           the kernel boot path was NOT exercised")
        print("           (set BOOT_SMOKE_ALLOW_TCG=1 to run under emulation "
              "instead; TCG boots this image)")
        return EXIT_SKIP
    if not kvm_ok:
        print("boot-smoke: %s; BOOT_SMOKE_ALLOW_TCG=1, running under TCG"
              % kvm_why)

    # ---- fail before booting: a stale image makes every verdict a lie -----
    src_root = image_root(image)
    if src_root is None:
        print("boot-smoke: harness error: cannot work out which source tree "
              "%s belongs to, so its freshness cannot be checked and this run "
              "would prove nothing. Expected <tree>/build/os.img with a src/ "
              "beside <tree>." % image, file=sys.stderr)
        return EXIT_FAIL
    fresh, detail = image_is_fresh(src_root, image)
    if not fresh:
        # Refused before booting, so there is no log and nothing to point at.
        # Saying "serial log: <the image>" here would be a small lie that
        # sends the next reader to a 64 MiB binary instead of a log.
        print("boot-smoke: FAIL -- harness error, nothing was booted")
        print("  %s" % detail)
        print("  image: %s" % image)
        print("  source tree: %s" % src_root)
        return EXIT_FAIL

    scratch = scratch_dir(ROOT)
    try:
        os.makedirs(scratch, exist_ok=True)
    except OSError as exc:
        print("boot-smoke: harness error: cannot create the scratch directory "
              "%s (%s). A full filesystem here yields a 0-byte serial log and "
              "reads as a silent guest." % (scratch, exc), file=sys.stderr)
        return EXIT_FAIL
    if free_bytes(scratch) < 8 * 1024 * 1024:
        print("boot-smoke: harness error: less than 8 MiB free under %s. A "
              "serial log that cannot be written is a 0-byte log, and a 0-byte "
              "log has been read as a healthy guest in this project before."
              % scratch, file=sys.stderr)
        return EXIT_FAIL

    # Never boot the shared build/os.img: QEMU takes a write lock on it and
    # every other agent is booting from their own copy of it.
    mine = os.path.join(scratch, "os-%d.img" % os.getpid())
    log = os.path.join(scratch, "serial-%d.log" % os.getpid())
    errlog = log + ".err"
    try:
        shutil.copyfile(image, mine)
    except OSError as exc:
        print("boot-smoke: harness error: cannot copy the image to %s (%s)"
              % (mine, exc), file=sys.stderr)
        return EXIT_FAIL

    accel = "TCG" if (not kvm_ok or os.environ.get("FORCE_TCG")) else "KVM"
    try:
        info = run_boot(ROOT, mine, log, errlog, timeout, allow_tcg, extra)
    finally:
        # The copy goes and the log stays: the image is reproducible from the
        # tree, and leaving a stale copy lying around is how this project read
        # one as a real bug twice. The log is the evidence, so it is kept and
        # its path is printed either way.
        try:
            os.unlink(mine)
        except OSError:
            pass

    raw = read_log(log)
    text = load_text(raw)
    if info["log_bytes"] == 0:
        # 0 bytes is never a healthy guest, and it is never a kernel verdict
        # either: the guest produced no byte at all, so all this run can say is
        # that the machine could not run it or the harness got the invocation
        # wrong. Those two are told apart by what QEMU printed.
        verdict, complaints = classify_startup(info)
        if verdict == "skip":
            print("boot-smoke: SKIP -- QEMU would not start a guest")
            for ln in complaints[:4]:
                print("           %s" % ln)
            print("           the kernel boot path was NOT exercised")
            return EXIT_SKIP
        print(report_fail(
            ["harness error: the serial log is 0 bytes, so the guest produced "
             "no output at all and this run says nothing about the kernel",
             "  qemu found at: %s" % qemu,
             "  accelerator pre-check: %s" % kvm_why,
             "  if the log file exists and is empty, the log was never "
             "written: check the path, the free space under FR_TMPDIR, and "
             "the colon in -serial chardev:s0"],
            text, log, info), file=sys.stderr)
        return EXIT_FAIL

    failure = evaluate(text, log, info, raw)
    if failure is not None:
        print(failure, file=sys.stderr)
        return EXIT_FAIL

    print("boot-smoke: PASS -- %d/%d milestones in order, no failure "
          "signature, %.1fs (%s)"
          % (len(MILESTONES), len(MILESTONES), info["elapsed"], accel))
    note = corruption_note(raw)
    if info.get("timed_out_draining"):
        note = ((note + "; ") if note else "") + (
            "QEMU did not finish within %.0fs of being asked to stop, so the "
            "log may be truncated" % DRAIN_TIMEOUT)
    if note:
        print("           WARNING: %s" % note)
    print("           serial log: %s" % log)
    return EXIT_PASS


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except KeyboardInterrupt:
        sys.exit(130)