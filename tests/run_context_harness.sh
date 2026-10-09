#!/bin/sh
# run.sh — build and run the host-side context switch harness.
#
# src/kernel/context.S is assembled for the host with no source changes: it is
# pure register-and-stack code, and gcc's x86-64 defaults already cover fxsave,
# fxrstor and rdmsr.
#
# Not wired into the top-level Makefile on purpose. The kernel build is shared
# state in this tree and other agents are rebuilding it constantly; a test
# target that fails for host-toolchain reasons would be indistinguishable from
# a kernel regression.
set -e

cd "$(dirname "$0")/.."

: "${TMPDIR:=/home/omena0/.fr-tmp}"
export TMPDIR
mkdir -p "$TMPDIR"

OUT="$TMPDIR/context-harness"

CC=${CC:-gcc}
CFLAGS="-O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -fno-omit-frame-pointer"

$CC $CFLAGS -c src/kernel/context.S -o "$TMPDIR/context.host.o"
$CC $CFLAGS -c tests/resume_stub.S -o "$TMPDIR/resume_stub.host.o"
$CC $CFLAGS -c tests/context_switch_harness.c -o "$TMPDIR/harness.o"
$CC $CFLAGS -o "$OUT" "$TMPDIR/context.host.o" "$TMPDIR/resume_stub.host.o" \
    "$TMPDIR/harness.o"

rc=0
"$OUT" || rc=$?
"$OUT" --restore-only || rc=$?

if [ "$rc" -eq 0 ]; then
    echo "PASS: context switch frame contract holds"
else
    echo "FAIL: context switch harness exited $rc"
fi
exit "$rc"