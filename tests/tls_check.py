#!/usr/bin/env python3
"""Re-derive a linked image's thread pointer and check the kernel's rule.

elf.c computes

    mm->tls_ptr = ALIGN_UP(p_vaddr + p_memsz, p_align)

and uapi/syscall.h hands that value to libc through SYS_get_tls_base.  If the
formula is wrong nothing crashes: local-exec TLS is addressed at *negative*
displacements from FS, so a thread pointer a few bytes off resolves every
`__thread` access to the wrong byte of the same mapped page.  That is silent
data corruption and it is invisible in review, so the formula is re-derived
from the artefact here instead of trusted from the source.

    1. read PT_TLS out of the image: p_vaddr, p_memsz, p_align
    2. read every %fs displacement out of the disassembly, both the immediate
       form (cmp %rax,%fs:0xfffffffffffffff0) and the register form
       (mov $imm,%rax ; ... ; %fs:(%rax))
    3. intersect the intervals that would put every access inside the block;
       the kernel's formula has to land in that intersection

Usage:  tls_check.py <image.elf> [label]
"""

import re
import subprocess
import sys


def program_headers(path):
    out = subprocess.run(["readelf", "-lW", path],
                         capture_output=True, text=True)
    if out.returncode:
        raise SystemExit("readelf failed on " + path)
    for line in out.stdout.splitlines():
        if line.strip().startswith("TLS"):
            return line.split()
    return None


def fs_displacements(path):
    out = subprocess.run(["objdump", "-d", path],
                         capture_output=True, text=True)
    if out.returncode:
        raise SystemExit("objdump failed on " + path)
    text = out.stdout
    immediate = set(int(m, 16) for m in re.findall(r"%fs:(0x[0-9a-f]+)", text))
    register = set(int(m, 16) for m in re.findall(
        r"mov\s+\$(0x[0-9a-f]+),%rax\n[^\n]*%fs:\(%rax\)", text))
    return sorted(((x - 2 ** 64) if x >= 2 ** 63 else x)
                  for x in (immediate | register))


def check(path, label):
    tls = program_headers(path)
    if tls is None:
        print("%s: SKIP (no PT_TLS)" % label)
        return 0
    vaddr, memsz, align = int(tls[2], 16), int(tls[5], 16), int(tls[7], 16)
    if align == 0:
        align = 1

    disps = fs_displacements(path)
    if not disps:
        print("%s: SKIP (no %%fs-relative access)" % label)
        return 0

    lo = hi = None
    for disp in disps:
        a, b = vaddr - disp, vaddr + memsz - disp
        lo = a if lo is None else max(lo, a)
        hi = b if hi is None else min(hi, b)

    end = vaddr + memsz
    want = (end + align - 1) // align * align

    common = (lo is not None and lo < hi)
    unique = common and (hi - lo == 1)
    note = "unique" if unique else ("one of %d" % (hi - lo) if common else "NONE")

    if not common:
        print("%s: FAIL no thread pointer puts all of %s inside the block"
              % (label, disps))
        return 1
    if not (lo <= want < hi):
        print("%s: FAIL ALIGN_UP(end,p_align)=%#x outside [%#x,%#x)"
              % (label, want, lo, hi))
        print("        vaddr=%#x memsz=%#x p_align=%#x disps=%s"
              % (vaddr, memsz, align, disps))
        print("        the unaligned end %#x would have given %s"
              % (end, "the same answer" if end == want else "a WRONG tp"))
        return 1

    tail = ""
    if end != want:
        if lo <= end < hi:
            print("%s: NOTE the shape does not discriminate: the unaligned "
                  "end %#x is also acceptable" % (label, end))
            tail = "  unaligned_end=%#x [indistinguishable here]" % end
        else:
            tail = "  unaligned_end=%#x [REJECTED by this shape]" % end
    print("%s: OK  tp=%#x (%s)  vaddr=%#x memsz=%#x p_align=%#x%s"
          % (label, want, note, vaddr, memsz, align, tail))
    return 0


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    path = argv[1]
    label = argv[2] if len(argv) > 2 else path
    return check(path, label)


if __name__ == "__main__":
    sys.exit(main(sys.argv))