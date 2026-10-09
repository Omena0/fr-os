#!/bin/sh
#
# verify_isa_test.sh -- test the ISA gate.
#
# The gate in tools/verify_isa.py is the only thing standing between a flag
# typo and a kernel that cannot boot, so it gets tested like any other code.
# There are two layers:
#
#   1. `verify_isa.py --self-test`, which is the exhaustive one. It checks the
#      classifier against a list of several hundred mnemonics in both
#      directions -- what must be reported and what must not -- and fails
#      loudly on either kind of mistake. The lists live in the tool.
#
#   2. This script, which is the end-to-end one. It compiles and assembles a
#      handful of small objects with the project's real flags and asserts what
#      each profile says about them. This is what proves the gate is actually
#      wired to the right flag lists, which a unit test of the classifier
#      cannot.
#
# Usage (from the Makefile):
#     sh tests/isa/verify_isa_test.sh <cc> <outdir> <kernel-cflags> <user-cflags>
#
# The two flag strings are single arguments containing spaces. They are used
# unquoted on purpose, because that is how make passes them to a recipe too.
#
# Exit status 0 if every expectation held, 1 otherwise.

set -u

CC=${1:?need a compiler}
OUT=${2:?need an output directory}
KFLAGS=${3:?need KERNEL_CFLAGS}
UFLAGS=${4:?need USER_CFLAGS}

here=$(dirname "$0")
root=$here/../..
tool=$root/tools/verify_isa.py

mkdir -p "$OUT" || exit 1

pass=0
fail=0

# expect <name> <profile> <want: clean|dirty> <object>
expect() {
    _name=$1
    _profile=$2
    _want=$3
    _obj=$4
    _err=$OUT/$_name.$_profile.err
    if python3 "$tool" --profile "$_profile" "$_obj" >/dev/null 2>"$_err"; then
        _got=clean
    else
        _got=dirty
    fi
    if [ "$_got" = "$_want" ]; then
        pass=$((pass + 1))
        printf '  ok   %-34s %-6s %s\n' "$_name" "$_profile" "$_got"
    else
        fail=$((fail + 1))
        printf '  FAIL %-34s %-6s got %s, wanted %s\n' \
            "$_name" "$_profile" "$_got" "$_want"
        sed 's/^/       | /' "$_err" | head -20
    fi
}

# expect_hits <name> <profile> <object> <mnemonic>...
# As above, plus: every mnemonic listed must appear in the report. This is the
# part that would have caught the original defect, because the report is what
# a person debugging a build actually reads.
expect_hits() {
    _name=$1
    _profile=$2
    _obj=$3
    shift 3
    _err=$OUT/$_name.$_profile.err
    python3 "$tool" --profile "$_profile" "$_obj" >/dev/null 2>"$_err"
    _rc=$?
    _missing=
    for _m in "$@"; do
        grep -qE "[[:space:]]$_m\$" "$_err" || _missing="$_missing $_m"
    done
    if [ "$_rc" -ne 0 ] && [ -z "$_missing" ]; then
        pass=$((pass + 1))
        printf '  ok   %-34s %-6s reported %s\n' "$_name" "$_profile" "$*"
    else
        fail=$((fail + 1))
        printf '  FAIL %-34s %-6s rc=%s missing:%s\n' \
            "$_name" "$_profile" "$_rc" "${_missing:- none}"
        sed 's/^/       | /' "$_err" | head -20
    fi
}

compile() {
    # compile <source> <object> <flags...>
    _src=$1
    _obj=$2
    shift 2
    # shellcheck disable=SC2086
    $CC "$@" -c "$_src" -o "$_obj" 2>"$_obj.cc.log" || {
        fail=$((fail + 1))
        printf '  FAIL compile %-28s exit %s\n' "$_src" "$?"
        sed 's/^/       | /' "$_obj.cc.log" | head -20
        return 1
    }
    return 0
}

assemble() {
    # assemble <source> <object> <extra-flags...>
    _src=$1
    _obj=$2
    shift 2
    # shellcheck disable=SC2086
    $CC -c -x assembler-with-cpp "$@" "$_src" -o "$_obj" 2>"$_obj.as.log" || {
        fail=$((fail + 1))
        printf '  FAIL assemble %-27s exit %s\n' "$_src" "$?"
        sed 's/^/       | /' "$_obj.as.log" | head -20
        return 1
    }
    return 0
}

echo "verify-isa-test: classifier self test"
if python3 "$tool" --self-test; then
    pass=$((pass + 1))
else
    fail=$((fail + 1))
fi

echo "verify-isa-test: the trap this gate exists for"
# KERNEL_CFLAGS followed by -msse2. On x86 the last -m flag for an ISA
# feature is the one that counts, so this is not a contradictory flag list --
# it is a working one, and it is what -msse4.2 and then -mavx2 each did to
# this kernel before the gate existed. The object has to be rejected.
if compile "$here/isa_probe.c" "$OUT/kernel_sse2.o" $KFLAGS -msse2; then
    expect_hits kernel-plus-msse2 kernel "$OUT/kernel_sse2.o" \
        movups paddq pxor movdqa
fi

echo "verify-isa-test: the current flag lists are clean"
if compile "$here/isa_probe.c" "$OUT/kernel_flags.o" $KFLAGS; then
    expect kernel-flags kernel clean "$OUT/kernel_flags.o"
fi
if compile "$here/isa_probe.c" "$OUT/user_flags.o" $UFLAGS; then
    expect user-flags user clean "$OUT/user_flags.o"
    expect user-flags kernel dirty "$OUT/user_flags.o"
fi

echo "verify-isa-test: the last -m flag wins, in both directions"
# This case used to assert that appending -march=x86-64-v3 to USER_CFLAGS
# produces VEX, which would make the gate fire. It does not, and the reason is
# worth writing down: an explicit -m<feat>/-mno-<feat> is NOT overridden by a
# -march=, in either order. Measured with `gcc -Q --help=target`:
#
#   -mno-avx -march=x86-64-v3   -> avx disabled
#   -march=x86-64-v3 -mno-avx   -> avx disabled
#   -mno-avx -mavx              -> avx enabled
#
# Order decides between two explicit -m flags, and never between an -m and a
# -march. So USER_CFLAGS' four -mno- flags hold against any -march, and the
# object built here contains no VEX at all -- movdqu, pxor and cvtsi2sd, all
# legacy. The old check was therefore testing gcc's option resolution, and
# expecting "clean" would have been a green test that tests gcc.
#
# What is worth pinning is the property that does exist: the last -m flag
# decides, and the gate catches the object when it goes the wrong way.
if compile "$here/isa_probe.c" "$OUT/user_v3.o" $UFLAGS -march=x86-64-v3; then
    expect user-plus-v3 user clean "$OUT/user_v3.o"
    expect user-plus-v3 kernel dirty "$OUT/user_v3.o"
fi
if compile "$here/isa_probe.c" "$OUT/user_avx_last.o" $UFLAGS -mavx2; then
    expect user-plus-avx-last user dirty "$OUT/user_avx_last.o"
fi
if compile "$here/isa_probe.c" "$OUT/user_sse2.o" $UFLAGS -msse2; then
    expect user-plus-sse2 user clean "$OUT/user_sse2.o"
    expect user-plus-sse2 kernel dirty "$OUT/user_sse2.o"
fi

echo "verify-isa-test: legacy SSE is a kernel failure and not a userspace one"
# The original defect, in one line. movaps/movups/movapd/movupd are *single*
# and *double*, not *aligned* and *unaligned*, and the old list spelled them
# movap[au]/movup[au] -- so it matched nothing at all.
if assemble "$here/legacy_simd.s" "$OUT/legacy_simd.o" -msse4.2; then
    expect legacy-simd kernel dirty "$OUT/legacy_simd.o"
    expect legacy-simd user clean "$OUT/legacy_simd.o"
    expect_hits legacy-simd-names kernel "$OUT/legacy_simd.o" \
        movaps movups movapd movupd movss movsd \
        movddup movshdup movsldup pshufhw pshuflw movmskps \
        lddqu pmovmskb psadbw ptest paddq psubq psubsb psubsw \
        psubusb psubusw addps mulps sqrtps minpd maxsd \
        unpckhpd shufps cvtsi2sd movdqa movdqu psubd \
        por pand pxor aeskeygenassist emms flds
fi

echo "verify-isa-test: VEX, AVX-512 opmask and XSAVE are userspace failures too"
if assemble "$here/vex_avx.s" "$OUT/vex_avx.o" \
        -mavx2 -mavx512f -mavx512bw -mavx512vl -mavx512dq; then
    expect vex-avx kernel dirty "$OUT/vex_avx.o"
    expect vex-avx user dirty "$OUT/vex_avx.o"
    expect_hits vex-avx-names user "$OUT/vex_avx.o" \
        vmovdqu vpaddq vzeroupper vfmadd132ps vmovdqu64 \
        kmovw kortestw kandw vpxor
fi
if compile "$here/isa_probe.c" "$OUT/avx2.o" -O2 -mavx2; then
    expect avx2-c kernel dirty "$OUT/avx2.o"
    expect avx2-c user dirty "$OUT/avx2.o"
fi

echo "verify-isa-test: the two exemptions, and their neighbours"
if assemble "$here/fxsave_only.s" "$OUT/fxsave_only.s.o" -msse4.2; then
    # fxsave/fxrstor are allowed; fxsave64/fxrstor64, fld, emms, ldmxcsr and
    # cvtsi2sd in the same file are not, and the report has to say so.
    expect_hits fxsave-exemption kernel "$OUT/fxsave_only.s.o" \
        fxsave64 flds fxrstor64 ldmxcsr cvtsi2sd emms
fi

echo "verify-isa-test: no false positives on GPR-only images"
if assemble "$here/gpr_movs.s" "$OUT/gpr_movs.o" -mbmi -mbmi2 -mlzcnt; then
    expect gpr-movs kernel clean "$OUT/gpr_movs.o"
    expect gpr-movs user clean "$OUT/gpr_movs.o"
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "verify-isa-test: $pass checks passed"
    exit 0
fi
echo "verify-isa-test: $fail of $((pass + fail)) checks FAILED"
exit 1
