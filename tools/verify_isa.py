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
         statement rather than a hope: those flags are the *policy*, and an
         explicit `-mavx`/`-mavx2` appended after them would acquire YMM
         without anybody noticing.

         One claim that used to be written here was false, and it is worth
         keeping the correction because a test was built on it. It said that
         "the last -m flag for an ISA feature is the one that counts", so a
         later `-march=x86-64-v3` would silently acquire AVX2. Measured on the
         toolchain in use (gcc 16.2.1, `gcc -Q --help=target`):

             -mno-avx -march=x86-64-v3      avx disabled
             -march=x86-64-v3 -mno-avx      avx disabled
             -mno-avx -mavx                 avx enabled
             -mavx  -mno-avx                avx disabled
             -march=x86-64-v3 alone         avx enabled, avx2 enabled

         So order decides between two *explicit* -m flags and nowhere else: an
         explicit -m<feature>/-mno-<feature> is not overridden by -march= in
         either order. `-march=x86-64-v3` on its own is therefore not a way to
         break USER_CFLAGS, and a self-test that asserted it was testing gcc's
         option resolution, not this gate.

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
# pdep and pext are BMI2 and operate entirely on general-purpose registers,
# and they are the one pair here that a kernel has a real reason to emit, so
# they are excluded by name rather than being left to a false positive.
# prefetch* is excluded by prefix on purpose: it is a hint, not a write, and
# its VEX form (prefetchwt1) would otherwise be reported as a wide vector use
# by a rule meant to catch YMM.
_P_NOT_SIMD = re.compile(
    r"^(?:push|pop|pause|popcnt|pdep|pext|prefetch)[a-z0-9]*$"
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
        r"^divp[sd]$", r"^minp[sd]$", r"^maxp[sd]$",
        r"^sqrtp[sd]$", r"^rcp[ps][sd]$", r"^rsqrt[ps][sd]$",
        # packed float, scalar form
        r"^(?:add|sub|mul|div|min|max|sqrt)s[sd]$",
        r"^(?:comi|ucomi)[sp][sd]$",
        # every packed/scalar compare: cmpps, cmppd, cmpss, cmpsd, and the
        # cmpeq/cmplt/cmple/cmpne/cmpnlt/cmpnle/cmpunord/cmpord/... forms
        # objdump prints for the predicate-immediate encodings. The trailing
        # [ps][sd] is what makes it specific: no GPR mnemonic ends that way,
        # and the bare GPR compare is printed without a suffix.
        r"^cmp[a-z]*[ps][sd]$",
        r"^roundp[sd]$", r"^rounds[sd]$",
        # shuffles, unpacks, horizontal
        r"^unpck[hl]p[sd]$", r"^shufp[sd]$", r"^shufps$",
        r"^haddp[sd]$", r"^hsubp[sd]$", r"^blendp[sd]$", r"^blendw$",
        r"^blendv(?:ps|pd)$",
        # moves
        r"^movap[sd]$", r"^movup[sd]$", r"^movdq[au]$",
        r"^movh[pl][sd]$", r"^movl[pl][sd]$", r"^movhlps$", r"^movlhps$",
        r"^movddup$", r"^movshdup$", r"^movsldup$",
        r"^movmskp[sd]$", r"^maskmov[qd]$",
        r"^movnt[pdq][a-z0-9]*$",
        r"^movs[sd]$",
        # the aligned/unaligned pair-move forms. movdqa/movdqu are listed
        # here and movd/movq are *not*: objdump prints those two spellings for
        # a sign-extending GPR immediate store to a 64-bit destination, so they
        # are decided by their operands in _is_vector above.
        r"^movdqa$", r"^movdqu$",
        # conversions, reductions, dot products
        r"^cvt[a-z0-9]+$",
        r"^insertps$", r"^extractps$",
        r"^dpps$", r"^dppd$", r"^mpsadbw$",
        # AES and XSHA. Both are leading-letter families: nothing else on
        # x86-64 starts with "aes" or "sha".
        r"^aes[a-z0-9]+$", r"^sha[0-9][a-z0-9]+$",
        # MXCSR and the unaligned load
        r"^ldmxcsr$", r"^stmxcsr$", r"^lddqu$",
        # MMX teardown. objdump prints femms for the 3DNow! form.
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
  is appended to that list later.

  If you are trying to work out how this list gets broken, note that the order
  rule is narrower than it looks. On gcc 16.2.1, measured with
  `gcc -Q --help=target`: an explicit -m<feature> or -mno-<feature> is not
  overridden by -march= in either order, so appending -march=x86-64-v3 to the
  list does *not* acquire AVX2. What does acquire it is appending an explicit
  -mavx, -mavx2, -mfma or -m16c after the -mno- block, or dropping the -mno-
  block -- order decides between two explicit -m flags, last one wins.
"""


# --------------------------------------------------------------------------
# Self test
# --------------------------------------------------------------------------
#
# The gate is the only thing standing between a flag typo and a kernel that
# cannot boot, so the gate gets tested too. `make verify-isa-test` runs this
# and then compiles a handful of small objects through both profiles.
#
# The lists below are not aspirational. MUST_FLAG_VECTOR is what objdump
# actually printed for a probe holding one instance of every legacy
# x87/MMX/SSE..SSE4.2/AES/XSHA mnemonic, and MUST_NOT_FLAG is the union of the
# full mnemonic set of build/kernel.elf and a hand-written census of the
# GPR-only instruction set. Both were diffed against the classifier before
# being written down, which is how `movq` turned up in the wrong list: objdump
# prints a sign-extending 32-bit immediate store to a 64-bit destination as
# `movq`, so a plain "movq is SSE2" rule produces 161 false positives on this
# kernel.
#
# The two operand-sensitive lists exist because six mnemonics are spelled the
# same for a GPR operation and a vector one. Getting those wrong in either
# direction is a real failure: a missed movsd is a missed vector move, and a
# flagged movsq is a build that stops for no reason.

_MUST_FLAG_VECTOR = (
    "addpd", "addps", "addsd", "addss",
    "addsubpd", "addsubps", "aesdec", "aesdeclast",
    "aesenc", "aesenclast", "aesimc", "aeskeygenassist",
    "andnpd", "andnps", "andpd", "andps",
    "blendpd", "blendps", "blendvpd", "blendvps",
    "cmpeqpd", "cmpeqps", "cmpeqsd", "cmpeqss",
    "cmplepd", "cmpleps", "cmplesd", "cmpless",
    "cmpltpd", "cmpltps", "cmpltsd", "cmpltss",
    "cmpneqpd", "cmpneqps", "cmpneqsd", "cmpneqss",
    "cmpnleps", "cmpnltpd", "cmpnltps", "cmpordpd",
    "cmpordps", "cmpordsd", "cmpordss", "cmppeqpd",
    "cmppeqps", "cmpunordpd", "cmpunordps", "cmpunordsd",
    "cmpunordss", "comisd", "comiss", "cvtdq2pd",
    "cvtdq2ps", "cvtpd2dq", "cvtpd2ps", "cvtps2dq",
    "cvtps2pd", "cvtsd2si", "cvtsd2ss", "cvtsi2sd",
    "cvtsi2ss", "cvtss2sd", "cvtss2si", "cvttpd2dq",
    "cvttps2dq", "cvttsd2si", "cvttss2si", "divpd",
    "divps", "divsd", "divss", "dppd",
    "dpps", "emms", "extractps", "fabs",
    "faddp", "fadds", "fbld", "fbstp",
    "fchs", "fcmovb", "fcmovbe", "fcmove",
    "fcmovnb", "fcmovnbe", "fcmovne", "fcmovnu",
    "fcmovu", "fcomi", "fcos", "fdivp",
    "fdivrp", "fdivrs", "fdivs", "femms",
    "fildl", "fildll", "finit", "fistpl",
    "fistpll", "fisttps", "fld1", "fldcw",
    "fldenv", "fldl", "fldl2e", "fldl2t", "fldlg2",
    "fldln2", "fldpi", "flds", "fldt", "fldz",
    "fmulp", "fmuls", "fnclex", "fnop",
    "fnstcw", "fnstenv", "fnstsw", "fpatan",
    "fprem", "fprem1", "fptan", "frndint",
    "fscale", "fsin", "fsqrt", "fstps",
    "fsts", "fsubp", "fsubrp", "fsubrs",
    "fsubs", "fucomi", "fucomip", "fucomp",
    "fucompp", "fxam", "fxch", "fyl2x",
    "fyl2xp1", "haddpd", "haddps", "hsubpd",
    "hsubps", "insertps", "lddqu", "ldmxcsr",
    "maskmovd", "maskmovq", "maxpd", "maxps",
    "maxsd", "maxss", "minpd", "minps",
    "minsd", "minss", "movapd", "movaps",
    "movd", "movddup", "movdqa", "movdqu",
    "movhlps", "movhpd", "movhps", "movlhps",
    "movlpd", "movmskpd", "movmskps", "movntdq",
    "movntdqa", "movntpd", "movntps", "movntq",
    "movq", "movsd", "movshdup", "movsldup",
    "movss", "movupd", "movups", "mulpd",
    "mulps", "mulsd", "mulss", "orpd",
    "orps", "pabsb", "pabsw", "packssdw",
    "packsswb", "packuswb", "paddb", "paddd",
    "paddq", "paddsb", "paddsw", "paddw",
    "palignr", "pand", "pandn", "pavgusb",
    "pavgw", "pblendvb", "pblendw", "pclmulhqlqdq",
    "pclmullqlqdq", "pcmpeqb", "pcmpeqq", "pcmpestri",
    "pcmpestrm", "pcmpgtb", "pcmpgtq", "pcmpistri",
    "pcmpistrm", "pfadd", "pfcmpeq", "pfcmpge",
    "pfcmpgt", "pfdiv", "pfmadd", "pfmul",
    "pfrcp", "pfrsqrt", "pfsub", "phaddd",
    "phaddw", "phminposuw", "phsubw", "pmaddubsw",
    "pmaddwd", "pmaxsd", "pmaxsw", "pmaxub",
    "pmaxud", "pmaxuw", "pminsd", "pminsw",
    "pminub", "pminud", "pminuw", "pmovmskb",
    "pmovsxbw", "pmovsxdq", "pmovsxwd", "pmovzxbw",
    "pmovzxdq", "pmovzxwd", "pmuldq", "pmulhrsw",
    "pmulhuw", "pmulhw", "pmulld", "pmullw",
    "pmuludq", "por", "psadbw", "pshufb",
    "pshufhw", "pshuflw", "pshufw", "psignb",
    "psignd", "psignw", "pslld", "pslldq",
    "psllq", "pslw", "psrad", "psraw",
    "psrld", "psrldq", "psrlq", "psrlw",
    "psubb", "psubd", "psubq", "psubsb",
    "psubsw", "psubusb", "psubusw", "psubw",
    "ptest", "punpckhbw", "punpckhdq", "punpckhqdq",
    "punpckhwd", "punpcklbw", "punpckldq", "punpcklqdq",
    "punpcklwd", "pxor", "rcppd", "rcpps",
    "rcpss", "roundpd", "roundps", "roundsd",
    "roundss", "rsqrtpd", "rsqrtps", "rsqrtss",
    "sha1msg1", "sha1msg2", "sha1nexte", "sha1rnds4",
    "sha256msg1", "sha256msg2", "sha256rnds2", "shufps",
    "sqrtpd", "sqrtps", "sqrtsd", "sqrtss",
    "stmxcsr", "subpd", "subps", "subsd",
    "subss", "ucomisd", "ucomiss", "unpckhpd",
    "unpcklpd", "xorpd", "xorps",
)

# Flagged by the classifier but exempted by the kernel *profile*, which is
# where the exemption belongs: they are deliberate, not unknowable.
_MUST_FLAG_BUT_EXEMPT = ("fxsave", "fxrstor")

_MUST_NOT_FLAG = (
    "adc", "adcx", "add", "addl", "addq",
    "adox", "and", "andb", "andn", "bextr",
    "blcs", "blsi", "blsr", "bmi", "bsr",
    "bswap", "bt", "btr", "bts", "bzhi",
    "call", "cbw", "cdq", "clc", "cld",
    "cli", "cltq", "clts", "cmc", "cmova",
    "cmovae", "cmovb", "cmovbe", "cmove", "cmovg",
    "cmovle", "cmovne", "cmovs", "cmp", "cmpb",
    "cmpl", "cmpq", "cmpsb", "cmpsl", "cmpsq",
    "cmpw", "cpuid", "cqo", "cs", "cwde",
    "hlt", "idiv", "imul", "in", "inc",
    "int", "int3", "invd", "invlpg", "iretq",
    "ja", "jae", "jb", "jbe", "jc",
    "jcxz", "je", "jg", "jge", "jl",
    "jle", "jmp", "jna", "jnae", "jnb",
    "jnbe", "jnc", "jne", "jng", "jnge",
    "jnl", "jnle", "jno", "jnp", "jns",
    "jnz", "jo", "jp", "jpe", "jrcxz",
    "js", "jz", "lea", "leave", "lfence",
    "lgdt", "lidt", "ljmp", "lldt", "lods",
    "lret", "lretq", "ltr", "lzcnt", "mfence",
    "mov", "movabs", "movb", "movl", "movq",
    "movsb", "movsbl", "movsl", "movslq", "movsq",
    "movsw", "movw", "movzbl", "movzwl", "mul",
    "mwait", "neg", "nop", "nopd", "nopl",
    "nopw", "not", "or", "orl", "out",
    "pause", "pdep", "pext", "pop", "popa",
    "popad", "popcnt", "popf", "popfq", "prefetch",
    "push", "pusha", "pushad", "pushf", "pushfq",
    "rcl", "rcr", "rdfsbase", "rdmsr", "rdpid",
    "rdrand", "rdseed", "rdtsc", "rdtscp", "rep",
    "repnz", "repz", "ret", "retf", "retfq",
    "rol", "ror", "rorx", "rsm", "sal",
    "sar", "sarx", "sbb", "seta", "setae",
    "setb", "setbe", "setc", "sete", "setg",
    "setge", "setl", "setle", "setna", "setnae",
    "setnb", "setnbe", "setnc", "setne", "setng",
    "setnge", "setnl", "setno", "setns", "seto",
    "sets", "sfence", "shl", "shlx", "shr",
    "shrx", "smsw", "stc", "std", "sti",
    "sub", "subl", "subq", "swapgs", "syscall",
    "sysret", "sysretq", "test", "testb", "tzcnt",
    "wbinvd", "wrfsbase", "wrmsr", "xadd", "xchg",
    "xgetbv", "xor", "xsetbv",
)

_MUST_FLAG_WIDE = (
    "kaddb", "kandw", "kmovb", "kmovw", "knotw",
    "kortestw", "korw", "kunpckbw", "vaddpd", "vaddps",
    "vaddss", "vaesenc", "vaesenclast", "vandnps", "vandps",
    "vblendvps", "vbroadcastsd", "vcmpltps", "vcmpneqpd", "vcvtdq2ps",
    "vcvtsi2sd", "vdivpd", "vextracti128", "vfmadd132ps", "vfmadd231ps",
    "vfmsub213sd", "vfnmadd231ps", "vinserti128", "vlddqu", "vmaskmovps",
    "vmovaps", "vmovd", "vmovdqa", "vmovdqu", "vmovdqu32",
    "vmovdqu64", "vmovmskps", "vmovq", "vmovups", "vorps",
    "vpaddb", "vpaddq", "vpand", "vpandn", "vpbroadcastb",
    "vpclmulhqlqdq", "vpcmpeqb", "vpcmpeqd", "vpcmpgtq", "vpcompressd",
    "vpdpbusd", "vperm2i128", "vpermt2d", "vpexpandd", "vpmovmskb",
    "vpmuldq", "vpmulld", "vpmuludq", "vpor", "vpshufb",
    "vpshufd", "vpsubq", "vpternlogd", "vptest", "vpxor",
    "vrcp14ps", "vroundps", "vsqrtps", "vunpcklps", "vxorps",
    "vzeroall", "vzeroupper",
)

_MUST_NOT_FLAG_WIDE = _MUST_NOT_FLAG

# (mnemonic, operand text) pairs where the operand list decides the answer.
_MUST_FLAG_VECTOR_OPS = (
    ("movd", "%eax,%xmm0"),
    ("movd", "%xmm0,%eax"),
    ("movq", "%rax,%xmm0"),
    ("movq", "%xmm0,%xmm1"),
    ("movq", "%xmm0,%rax"),
    ("movsd", "(%rax),%xmm0"),
    ("movsd", "%xmm0,(%rax)"),
    ("movapd", "%xmm0,%xmm1"),
    ("movupd", "%xmm0,%xmm1"),
    ("movmskps", "%xmm0,%eax"),
    ("movmskpd", "%xmm0,%rax"),
)

_MUST_NOT_FLAG_OPS = (
    ("movq", "%rax,%rbx"),
    ("movq", "%rax,0x8(%rbp)"),
    ("movd", "%eax,0x4(%rbp)"),
    ("movq", "$0x0,0x8(%rsp)"),
    ("movsd", "%rsi,%rdi"),
    ("movsq", "%rsi,%rdi"),
    ("movsl", "%esi,(%rdi)"),
    ("movsb", "%sil,0x0(%rdi)"),
    ("movsw", "%si,0x0(%rdi)"),
    ("movq", "0x10(%rsp),%rax"),
)


def self_test():
    """Return a list of failure descriptions; empty means the gate agrees."""
    failures = []
    for m in _MUST_FLAG_VECTOR:
        if not _is_vector(m, "%xmm0,%xmm1"):
            failures.append("kernel profile misses %s" % m)
    for m in _MUST_NOT_FLAG:
        if _is_vector(m, "%rax,%rbx"):
            failures.append("kernel profile false-positives on %s" % m)
    for m in _MUST_FLAG_WIDE:
        if not _is_wide(m, "%ymm0,%ymm1"):
            failures.append("user profile misses %s" % m)
        if not _is_vector(m, "%ymm0,%ymm1"):
            failures.append("kernel profile misses %s" % m)
    for m in _MUST_NOT_FLAG_WIDE:
        if _is_wide(m, "%rax,%rbx"):
            failures.append("user profile false-positives on %s" % m)
    for m, ops in _MUST_FLAG_VECTOR_OPS:
        if not _is_vector(m, ops):
            failures.append("kernel profile misses %s with operands %s" % (m, ops))
    for m, ops in _MUST_NOT_FLAG_OPS:
        if _is_vector(m, ops):
            failures.append(
                "kernel profile false-positives on %s with operands %s" % (m, ops))
    for m in _MUST_FLAG_BUT_EXEMPT:
        if not _is_vector(m, "%rdi"):
            failures.append("%s should be recognised as vector state" % m)
        if m not in KERNEL_ALLOWED:
            failures.append("%s should be exempt from the kernel profile" % m)
    return failures


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
    argv = list(argv)
    if "--self-test" in argv:
        argv.remove("--self-test")
        failures = self_test()
        if failures:
            sys.stderr.write(
                "ERROR: verify_isa.py self test failed (%d):\n" % len(failures)
            )
            for line in failures:
                sys.stderr.write("         %s\n" % line)
            return 1
        print(
            "verify-isa: self test passed (%d legacy mnemonics flagged, "
            "%d GPR mnemonics clean, %d VEX/AVX-512 mnemonics wide)"
            % (
                len(_MUST_FLAG_VECTOR) + len(_MUST_FLAG_VECTOR_OPS),
                len(_MUST_NOT_FLAG) + len(_MUST_NOT_FLAG_OPS),
                len(_MUST_FLAG_WIDE),
            )
        )
        return 0
    if not argv:
        sys.stderr.write(
            "verify_isa.py: need --profile kernel|user FILE... or --self-test\n"
        )
        return 2

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
