#!/usr/bin/env python3
"""
verify_isa.py -- fail the build if a linked image contains register state that
nothing on the kernel side carries across a context switch.

Why a script and not a grep pipeline
------------------------------------
The previous version of this gate was one `objdump -d | grep -oP | grep -E`
pipeline living in the Makefile, with the mnemonic list as a regex in a make
variable. Two things about that are worth recording, because both of them were
found the hard way and both are silent:

  1. Its mnemonic extractor was `grep -oP '\\t\\K[a-z][a-z0-9]*'`, which
     matches the *first* lowercase token after any tab. In objdump's output
     layout that includes the raw-byte column, so every two-byte instruction
     column whose bytes happen to start with a letter was harvested as a
     "mnemonic" -- `ret` (0xc3) contributed a phantom `c3` on every build. It
     also matched instruction *prefixes* (`lock`, `rep`, `data16`) and stopped
     there, so the real mnemonic after a prefix was never examined.

  2. Its mnemonic list was incomplete in a way that happened to hide the two
     most common vector moves on x86. It spelled them `movap[au]`/`movup[au]`,
     which do not exist: MOVAPS/MOVAPD are *single*/*double*, not *aligned*,
     so the character class never matched `movaps` and the gate reported a
     clean kernel for a kernel containing eight of them.

Neither of those is a reason to distrust the idea of checking the linked
output. It is a reason to stop doing it with a shell pipeline whose regex
nobody can review in one screen, and to test the gate itself.

The two profiles
----------------
kernel   GPR-only. No x87, no XMM, no YMM, no ZMM, no opmask, no XSAVE.
         fxsave/fxrstor are exempt -- context.S needs them and they do not
         compute anything.

user     XMM is legal, everything wider is not. The x86-64 baseline already
         provides SSE2, and fpu_save/fpu_restore in context.S are
         FXSAVE/FXRSTOR: the legacy 512-byte image, which is x87 plus
         XMM0-XMM15 plus MXCSR and nothing else. So a legacy SSE instruction
         in userland is preserved across a preemption and a VEX one is not.
         This is the profile init.elf and hello.elf are held to, and it is
         what makes USER_CFLAGS' -mno-avx/-mno-avx2/-mno-fma/-mno-f16c a
         statement rather than a hope: those flags are the *policy*, and a
         later `-march=x86-64-v3` appended to the list would acquire AVX2
         without anybody noticing.

Usage
-----
    verify_isa.py --profile kernel FILE...
    verify_isa.py --profile user   FILE...

Exit status: 0 clean, 1 policy violation, 2 bad invocation.
"""

import argparse
import os
import re
import subprocess
import sys
from collections import Counter

OBJDUMP = os.environ.get("OBJDUMP", "objdump")

# --------------------------------------------------------------------------
# Disassembly front end
# --------------------------------------------------------------------------

# "  ffffffff80000180:" -- the address field of an instruction line.
_RE_ADDR = re.compile(r"^\s*[0-9a-f]+:\s*$")
# "48 8d 25 79 4e 1b 00 " -- the raw-byte column, absent under
# --no-show-raw-insn.
_RE_BYTES = re.compile(r"^[0-9a-f]{2}( [0-9a-f]{2})*$")

# Words objdump prints in front of the mnemonic. They are not the mnemonic and
# matching them is how a `lock cmpxchg16b` line used to be read as a `lock`.
_PREFIXES = frozenset(
    (
        "lock", "rep", "repz", "repnz", "data16", "addr32", "bnd", "notrack",
        "rex.W", "rex.R", "rex.X", "rex.B",
    )
)

_VECTOR_REGS = re.compile(r"%(xmm|ymm|zmm|mm|st|k)[0-9]|\b(xmm|ymm|zmm)\b")


def _tokens(line):
    """Yield the whitespace-separated tokens of an objdump instruction line."""
    return line.split()


def iter_instructions(path):
    """
    Yield (mnemonic, operand_text, raw_line) for every instruction in `path`.

    Handles the two objdump layouts (with and without the raw-byte column)
    because relying on tab positions was the original defect.
    """
    try:
        proc = subprocess.run(
            (OBJDUMP, "-d", "--no-show-raw-insn", path),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    except OSError as exc:
        raise SystemExit("verify-isa: cannot run %s: %s" % (OBJDUMP, exc))
    if proc.returncode != 0 and not proc.stdout:
        raise SystemExit(
            "verify-isa: %s could not disassemble %s: %s"
            % (OBJDUMP, path, proc.stderr.decode("utf-8", "replace").strip())
        )

    for line in proc.stdout.decode("utf-8", "replace").splitlines():
        if "\t" not in line:
            continue
        fields = line.split("\t")
        if not _RE_ADDR.match(fields[0]):
            continue
        # fields[1] is the byte column when it is one; otherwise fields[1] is
        # already the mnemonic.
        if _RE_BYTES.match(fields[1].strip()):
            body = fields[2] if len(fields) > 2 else ""
        else:
            body = fields[1]
        # Drop a trailing "# 0x1234 <sym>" comment before looking at operands.
        body = body.split("#", 1)[0].strip()
        if not body:
            continue
        toks = _tokens(body)
        while toks and toks[0] in _PREFIXES:
            toks.pop(0)
        if not toks:
            continue
        yield toks[0], " ".join(toks[1:]), line


def _uses_vector_register(operand_text):
    return _VECTOR_REGS.search(operand_text) is not None


# --------------------------------------------------------------------------
# Classification
# --------------------------------------------------------------------------

# Instructions whose *spelling* is shared between a general-purpose move and a
# vector one. objdump prints the string "move string dword" and the SSE2 "move
# scalar double" both as `movsd`, and it prints a sign-extending 32-bit
# immediate store to a 64-bit destination (`c7 04 24 00 00 00 00`) as `movq`.
# Both are GPR operations and neither may be reported as vector state.
#
# The operands settle it: every SSE form of these names an %xmm/%ymm/%zmm, and
# no GPR or string form ever does. So the mnemonic alone is ambiguous here and
# the operand list is not.
_OPERAND_AMBIGUOUS = frozenset(
    ("movsb", "movsw", "movsl", "movsq", "movsd", "movd", "movq")
)

# `p`-prefixed instructions that are not SIMD. Everything else beginning with
# "p" is packed-integer or packed-float, and there are many more of those than
# of these, which is why this is an exclusion list and not an inclusion one.
# prefetch* is excluded by prefix on purpose: it is a hint, not a write, and
# its VEX form (prefetchwt1) would otherwise be reported as a wide vector use
# by a rule meant to catch YMM.
_P_NOT_SIMD = re.compile(
    r"^(?:push|pop|pause|popcnt|prefetch)[a-z0-9]*$"
)

# `f`-prefixed instructions that are not x87. `fence` is a memory-ordering
# fence, not an x87 operation; the F-prefixed fences are lfence/mfence/sfence
# and do not collide.
_F_NOT_SIMD = re.compile(r"^(?:fence|sfence|lfence|mfence)$")

# VEX-prefixed instructions that are not vector. VMX has a v-prefixed family
# too (vmcall, vmlaunch, ...); those are listed for completeness even though no
# compiler emits them, and `verr`/`verw` are genuine GPR/memory instructions
# that would otherwise be misfiled.
_V_NOT_WIDE = frozenset(
    (
        "verr", "verw",
        "vmcall", "vmlaunch", "vmresume", "vmxoff", "vmxclear", "vmxon",
        "vmptrld", "vmptrst", "vmread", "vmwrite", "vmfunc", "vmlaunch",
    )
)

# AVX-512 opmask instructions. There is no non-vector x86-64 mnemonic that
# starts with "k", so the leading letter is itself the discriminator.
_RE_KMASK = re.compile(
    r"^k(?:ortest|andn|and|or|xor|not|shiftl|shiftr|mov|add|unpck|unord|test)[a-z0-9]*$"
)

# XSAVE/XRSTOR. These write vector state *and* the state-area header, and the
# kernel deliberately does not pass -mxsave: nothing reconciles a compiler's
# XSAVE header with what context.S wrote into the task's state area. XGETBV
# and XSETBV are excluded on purpose -- they are control-register operations
# and kmain/cpu_features already use XGETBV.
_RE_XSAVE = re.compile(r"^(?:xsave|xrstor)[a-z0-9]*$")

_RE_MNEMONIC = re.compile(r"^[a-z][a-z0-9]*$")

# The legacy SSE families, by stem. Each entry is a regex anchored at both
# ends; the "p"-packed-integer family and the "f"-x87 family are handled by
# first-letter rules above and are not repeated here.
_LEGACY_SSE = tuple(
    re.compile(p)
    for p in (
        # packed float, register form
        r"^andnp[sd]$", r"^andp[sd]$", r"^orp[sd]$", r"^xorp[sd]$",
        r"^addp[sd]$", r"^addsubp[sd]$", r"^subp[sd]$", r"^mulp[sd]$",
        r"^divp[sd]$", r"^minp[sd]$", r"^maxp[sd]$", r"^sqrtsp[sd]$",
        r"^rcpp[sd]$", r"^rsqrtsp[sd]$", r"^cmpp[sd]$",
        # packed float, scalar form
        r"^(?:add|sub|mul|div|min|max|sqrt|cmp)s[sd]$",
        r"^rcpss$", r"^rsqrtss$",
        r"^(?:comi|ucomi)[sp][sd]$",
        r"^round[ps]$", r"^round[pd]$", r"^rounds[sd]$",
        # shuffles, unpacks, horizontal
        r"^unpck[hl]p[sd]$", r"^shufp[sd]$", r"^shufps$",
        r"^haddp[sd]$", r"^hsubp[sd]$", r"^blendp[sd]$", r"^blendw$",
        r"^blendv(?:ps|pd)$",
        # moves
        r"^movap[sd]$", r"^movup[sd]$", r"^movdq[au]$",
        r"^movh[pl][sd]$", r"^movl[pl][sd]$", r"^movhlps$", r"^movlhps$",
        r"^movddup$", r"^movshdup$", r"^movsldup$",
        r"^movmsk[pd]$", r"^maskmov[qd]$",
        r"^movnt[pdq][a-z0-9]*$",
        r"^movs[sd]$",
        # the SSE2 two-register forms. objdump prints a GPR move as `mov`
        # (never `movd`/`movq`), so these spellings are SIMD-only here.
        r"^movdqa$", r"^movdqu$",
        # conversions, reductions, dot products
        r"^cvt[a-z0-9]+$",
        r"^insertps$", r"^extractps$",
        r"^dpps$", r"^dppd$", r"^mpsadbw$",
        # MXCSR and the unaligned load
        r"^ldmxcsr$", r"^stmxcsr$", r"^lddqu$",
        # MMX teardown
        r"^emms$", r"^femms$",
    )
)


def _is_wide(mnemonic, operands):
    """True if the instruction implies YMM/ZMM/opmask/XSAVE state."""
    if mnemonic in _V_NOT_WIDE:
        return False
    if mnemonic.startswith("v") and len(mnemonic) > 1 and _RE_MNEMONIC.match(mnemonic):
        return True
    if _RE_KMASK.match(mnemonic):
        return True
    if _RE_XSAVE.match(mnemonic):
        return True
    return False


def _is_vector(mnemonic, operands):
    """True if the instruction touches x87, XMM or MMX state."""
    if _is_wide(mnemonic, operands):
        return True
    if mnemonic in _OPERAND_AMBIGUOUS:
        return _uses_vector_register(operands)
    if _RE_MNEMONIC.match(mnemonic) is None:
        return False
    if _P_NOT_SIMD.match(mnemonic) is None and mnemonic.startswith("p"):
        return True
    if _F_NOT_SIMD.match(mnemonic) is None and mnemonic.startswith("f"):
        return True
    for pattern in _LEGACY_SSE:
        if pattern.match(mnemonic):
            return True
    return False


# --------------------------------------------------------------------------
# Policy
# --------------------------------------------------------------------------

# context.S defines fpu_save/fpu_restore on these two and the scheduler calls
# them on every task switch. They are state management, not computation: they
# move 512 bytes without interpreting any of it. The exemption is exactly two
# instructions, because the 512 bytes *are* the whole contract and a wider
# exemption would hide a compiler-emitted fld or cvtsi2sd, which is the bug
# this gate exists to catch.
KERNEL_ALLOWED = frozenset(("fxsave", "fxrstor"))

PROFILES = {
    "kernel": {
        "rejects": _is_vector,
        "allowed": KERNEL_ALLOWED,
        "rejects_label": "vector/x87",
        "ok": "%s is GPR-only "
              "(fxsave/fxrstor exempt: deliberate FPU context save)",
    },
    "user": {
        "rejects": _is_wide,
        "allowed": frozenset(),
        "rejects_label": "YMM/ZMM/opmask",
        "ok": "%s stays inside the 128-bit XMM state FXSAVE carries",
    },
}

_KERNEL_NOTE = """\
  The kernel is GPR-only. That is a policy, not an inference from XCR0, and the
  difference matters when someone reads this message and tries to "fix" it:

    * stage2_long.S DOES program XCR0. It tests CPUID.1:ECX.27 (AVX) and
      CPUID.1:ECX.28 (OSXSAVE) -- 27 is AVX and 28 is OSXSAVE, in that order --
      and on a CPU that reports both it sets CR4.OSXSAVE, masks whatever the
      firmware left in XCR0 against CPUID.0x0D subleaf 0, and writes
      XCR0 | 0x7 (x87 | SSE | YMM_Hi128). On such a CPU a VEX instruction in
      the kernel executes; it does not fault.

    * Only on a CPU whose CPUID reports neither bit does the loader take the
      .Lno_avx path and skip XSETBV entirely, leaving XCR0 as the firmware set
      it -- and *that* is the case where every VEX instruction is #UD. A build
      that works on the developer's -cpu max and dies on real hardware is this
      case, which is the portability half of the rule.

    * The failure that cannot announce itself is the other half. fpu_save and
      fpu_restore in context.S are FXSAVE/FXRSTOR, the legacy 512-byte image:
      x87, XMM0-XMM15, MXCSR. YMM_Hi128, ZMM_Hi256 and the opmask registers are
      not in it. sched.c calls those two on every task switch, and the kernel
      shares one CPU with the tasks, so a YMM upper half a task is holding is
      silently overwritten by whichever task runs next -- no fault, no log
      line, and a computation that comes out wrong somewhere else.
"""

_USER_NOTE = """\
  This image is held to the 128-bit XMM baseline and nothing wider.

  fpu_save/fpu_restore in context.S are FXSAVE/FXRSTOR, the legacy 512-byte
  image: x87, XMM0-XMM15, MXCSR, and nothing above bit 127. Every task switch
  carries exactly that. A legacy SSE instruction here stays inside those 128
  bits and survives a preemption, which is why the x86-64 baseline's SSE2 is
  fine and is left alone.

  A VEX or AVX-512 instruction does not. Its upper halves are not in the image,
  so they are destroyed by the next task switch with no fault and nothing in
  the log to say so.

  USER_CFLAGS' -mno-avx -mno-avx2 -mno-fma -mno-f16c is the flag side of this.
  The flags are the policy; this check is what makes it true regardless of what
  is appended to that list later. `-march=x86-64-v3` is the realistic way it
  breaks, because on x86 the last -m flag for an ISA feature is the one that
  counts and it silently acquires AVX2 and every YMM user with it.
"""


# --------------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------------


def check(path, profile):
    """Return a Counter of violating mnemonics in `path`."""
    name = PROFILES[profile]
    rejects = name["rejects"]
    allowed = name["allowed"]
    hits = Counter()
    for mnemonic, operands, _line in iter_instructions(path):
        if mnemonic in allowed:
            continue
        if rejects(mnemonic, operands):
            hits[mnemonic] += 1
    return hits


def main(argv):
    parser = argparse.ArgumentParser(
        prog="verify_isa.py",
        description="Reject vector register state in a linked image.",
    )
    parser.add_argument(
        "--profile", required=True, choices=sorted(PROFILES),
        help="kernel = GPR-only; user = XMM legal, YMM/ZMM/opmask not",
    )
    parser.add_argument("files", nargs="+", metavar="FILE")
    args = parser.parse_args(argv)

    name = PROFILES[args.profile]
    failed = False
    for path in args.files:
        if not os.path.isfile(path):
            sys.stderr.write("verify-isa: %s does not exist\n" % path)
            return 2
        hits = check(path, args.profile)
        if not hits:
            print("verify-isa: %s" % (name["ok"] % path))
            continue
        failed = True
        sys.stderr.write(
            "ERROR: %s instructions in %s\n"
            % (name["rejects_label"], path)
        )
        for mnemonic, count in sorted(hits.items(), key=lambda kv: (-kv[1], kv[0])):
            sys.stderr.write("         %6d  %s\n" % (count, mnemonic))
        sys.stderr.write(
            "\n%s" % (_KERNEL_NOTE if args.profile == "kernel" else _USER_NOTE)
        )
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
