#!/bin/sh
# tls_harness.sh -- check the kernel's thread-pointer rule against real artefacts.
#
# The rule under test is the one in elf.c:
#
#     mm->tls_ptr = ALIGN_UP(p_vaddr + p_memsz, p_align)
#
# If it is wrong nothing crashes. Local-exec TLS is addressed at *negative*
# displacements from FS, so a thread pointer a few bytes off resolves every
# `__thread` access into the wrong byte of the same mapped page: silent data
# corruption, invisible in review. So the value is re-derived from each linked
# image instead of trusted from the source -- see tests/tls_check.py for the
# derivation.
#
# A spread of synthetic shapes is generated first (memsz 1, 4, 5, 8, 0xb, 0xc,
# 0x10, 0x20 and p_align up to 0x40), because a rule only ever exercised against
# the one shape that exists today is not a rule. The real images are checked
# afterwards if they have been built.
#
# Usage:  sh tests/tls_harness.sh [image.elf ...]
# Exit:   0 if every shape and image agrees, 1 otherwise.

set -u

here=$(dirname "$0")
work=$(mktemp -d "${TMPDIR:-/tmp}/tls_harness.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT

fail=0

gen() {
    name=$1
    cat > "$work/$name.c" || return 1
    gcc -m64 -ffreestanding -fno-pic -fno-pie -c \
        -o "$work/$name.o" "$work/$name.c" || return 1
    ld -static -no-pie -Ttext=0x400000 -e _start \
        -o "$work/$name.elf" "$work/$name.o" || return 1
}

shapes="byte char5 int u64 u64_int u64_2int two_u64 four_u64 mix bigalign"

gen byte     <<'EOF'
static __thread unsigned char v; void f(void){ v++; } void _start(void){f();}
EOF

gen char5    <<'EOF'
static __thread char c[5]; void f(void){ c[0]++; } void _start(void){f();}
EOF

gen int      <<'EOF'
static __thread int v; void f(void){ v++; } void _start(void){f();}
EOF

gen u64      <<'EOF'
static __thread unsigned long v; void f(void){ v++; } void _start(void){f();}
EOF

gen u64_int  <<'EOF'
static __thread unsigned long v; static __thread int a;
void f(void){ v++; a++; } void _start(void){f();}
EOF

gen u64_2int <<'EOF'
static __thread unsigned long v; static __thread int a,b;
void f(void){ v++; a++; b++; } void _start(void){f();}
EOF

gen two_u64  <<'EOF'
static __thread unsigned long v,w;
void f(void){ v++; w++; } void _start(void){f();}
EOF

gen four_u64 <<'EOF'
static __thread unsigned long a,b,c,d;
void f(void){ a++; b++; c++; d++; } void _start(void){f();}
EOF

gen mix      <<'EOF'
static __thread unsigned long v; static __thread char c[3];
static __thread int i;
void f(void){ v++; c[0]++; i++; } void _start(void){f();}
EOF

gen bigalign <<'EOF'
static __thread unsigned long v;
static __thread unsigned long w __attribute__((aligned(64)));
void f(void){ v++; w++; } void _start(void){f();}
EOF

echo "== synthetic shapes =="
for s in $shapes; do
    if [ -f "$work/$s.elf" ]; then
        python3 "$here/tls_check.py" "$work/$s.elf" "$s" || fail=1
    fi
done

if [ $# -gt 0 ]; then
    images=$*
else
    images="build/init.elf build/hello.elf"
fi

echo
echo "== linked images =="
found=0
for img in $images; do
    if [ -f "$img" ]; then
        found=1
        python3 "$here/tls_check.py" "$img" "$img" || fail=1
    else
        echo "$img: absent (build it first)"
    fi
done
[ $found -eq 0 ] && echo "(no images built; only synthetic shapes were checked)"

echo
if [ $fail -eq 0 ]; then
    echo "TLS harness: PASS"
else
    echo "TLS harness: FAIL"
fi
exit $fail