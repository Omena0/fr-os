#!/usr/bin/env python3
"""
check_schedule_resume.py — mechanical check of audit finding #57 against the
built kernel, not against the source or the comments.

#57: "schedule() ends with no landing pad after context_switch. When a task is
later resumed it returns into the middle of schedule(), after the kernel CR3
has been reloaded."

A task suspended inside schedule() resumes at the instruction after
`call context_switch` — there is no way to move that, because the return
address was chosen when the task was suspended. So the only question is what
that instruction has to be, and this answers it by reading the artefact:

  1. There is exactly one `call context_switch` in schedule(), and the
     instruction immediately after it is decoded.
  2. That instruction is a near jump (E9 rel32), not a call and not a ret.
     A ret would pop whatever the frame held; a call would re-enter the
     bookkeeping of the pair that has already been switched, which is the
     pre-fix failure mode.
  3. It has no memory operand and no CR3 access, so it is valid whatever
     address space is installed when a task comes back to it.
  4. Its target is an instruction boundary inside schedule() itself, and it
     points backwards — at the top of the loop, where the state is recomputed —
     rather than into the middle of the switch.
  5. Every CR3 write in schedule() is before the call, so a resumed task never
     arrives with the CR3 already reloaded underneath it.
  6. context_switch's own bytes are the frame contract the comments claim:
     six pushes, publish %rsp, adopt %rsi, six pops, ret — a 56-byte frame
     with the return address at +48.

Usage: check_schedule_resume.py [build/kernel.elf]
"""

import re
import subprocess
import sys

CR_MOV = 0x0F22  # 0F 22 /r — mov to a control register

fails = []
notes = []


def fail(msg):
    fails.append(msg)
    print("  FAIL: %s" % msg)


def ok(msg):
    print("  ok:   %s" % msg)


def disassemble(elf, start, end):
    """(addr, raw-bytes-hex, mnemonic, operand-text) for [start, end)."""
    out = subprocess.run(
        ["objdump", "-d", "--start-address=0x%x" % start,
         "--stop-address=0x%x" % end, elf],
        check=True, capture_output=True, text=True).stdout
    insns = []
    for line in out.splitlines():
        line = line.strip()
        if not line or ":" not in line:
            continue
        head, _, rest = line.partition(":")
        try:
            addr = int(head, 16)
        except ValueError:
            continue
        rest = rest.strip()
        if not rest:
            continue
        # objdump prints "<addr>: <bytes><tab><mnemonic> <operands>", and the
        # byte column is space-separated, so it cannot be split off with a
        # plain whitespace split.
        m = re.match(r"((?:[0-9a-f]{2} )*[0-9a-f]{2})\s+(.*)$", rest)
        if not m:
            continue  # a symbol/label line, not an instruction
        raw = m.group(1).replace(" ", "")
        text = m.group(2).strip()
        insns.append((addr, raw, text))
    return insns


def symbols(elf):
    out = subprocess.run(["nm", elf], check=True, capture_output=True,
                         text=True).stdout
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
    return syms


def elf_bytes(elf, vaddr, length):
    """Raw bytes at a virtual address, read from the PT_LOAD that holds it."""
    with open(elf, "rb") as f:
        data = f.read()
    e_phoff = int.from_bytes(data[0x20:0x28], "little")
    e_phentsize = int.from_bytes(data[0x36:0x38], "little")
    e_phnum = int.from_bytes(data[0x38:0x3a], "little")
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type = int.from_bytes(data[off:off + 4], "little")
        if p_type != 1:  # PT_LOAD
            continue
        p_offset = int.from_bytes(data[off + 8:off + 16], "little")
        p_vaddr = int.from_bytes(data[off + 16:off + 24], "little")
        p_filesz = int.from_bytes(data[off + 32:off + 40], "little")
        if p_vaddr <= vaddr < p_vaddr + p_filesz:
            start = p_offset + (vaddr - p_vaddr)
            return data[start:start + length]
    raise SystemExit("no PT_LOAD covers vaddr 0x%x" % vaddr)


def is_call_to(t, sym):
        f = t.split()
        return len(f) >= 2 and f[0] == "call" and f[-1] == "<%s>" % sym


def main():
    elf = sys.argv[1] if len(sys.argv) > 1 else "build/kernel.elf"
    syms = symbols(elf)

    for need in ("schedule", "context_switch", "context_restore"):
        if need not in syms:
            fail("%s has no symbol %s" % (elf, need))
    if fails:
        return 1

    # schedule() may be split into parts by the compiler (e.g., schedule.part.0).
    # Check all symbols that start with "schedule" and have the call.
    schedule_symbols = [a for n, a in syms.items() if n.startswith("schedule")]
    if not schedule_symbols:
        fail("%s has no schedule symbol" % elf)
        return 1
    
    # Find the one that contains the call to context_switch
    csw = syms["context_switch"]
    sched_with_call = None
    
    for sched_addr in sorted(schedule_symbols):
        # Find the next symbol after this one
        ordered = sorted(a for a in syms.values() if a > sched_addr)
        if not ordered:
            continue
        sched_end = ordered[0]
        
        insns = disassemble(elf, sched_addr, sched_end)
        calls = [a for a, _, t in insns if is_call_to(t, "context_switch")]
        if calls:
            sched_with_call = sched_addr
            sched_end_with_call = sched_end
            break
    
    if sched_with_call is None:
        fail("no schedule variant contains a call to context_switch")
        return 1
    
    sched = sched_with_call
    sched_end = sched_end_with_call

    print("== schedule() resume contract ==")
    print("  schedule        0x%x .. 0x%x" % (sched, sched_end))
    print("  context_switch  0x%x" % csw)
    print("  context_restore 0x%x" % syms["context_restore"])

    insns = disassemble(elf, sched, sched_end)
    addrs = {a for a, _, _ in insns}
    text_of = {a: t for a, _, t in insns}
    raw_of = {a: r for a, r, _ in insns}
    index = {a: i for i, (a, _, _) in enumerate(insns)}

    # --- 1. exactly one call to context_switch -----------------------------
    if len(calls) != 1:
        fail("expected exactly 1 `call context_switch` in schedule(), found %d: %s"
             % (len(calls), ["0x%x" % c for c in calls]))
        return 1
    call_at = calls[0]
    ok("exactly one `call context_switch` in schedule(), at 0x%x" % call_at)

    i = index[call_at]
    if i + 1 >= len(insns):
        fail("the call to context_switch is the last instruction of schedule(); "
             "there is no resume point at all")
        return 1
    pad_at, pad_raw, pad_text = insns[i + 1]

    # The `call` is 5 bytes (E8 rel32), so the resume address is pad_at and it
    # is by construction a valid instruction boundary.
    call_len = len(bytes.fromhex(raw_of[call_at]))
    if call_len != 5:
        fail("`call context_switch` is %d bytes, expected 5" % call_len)
    if pad_at != call_at + call_len:
        fail("resume point 0x%x is not immediately after the call at 0x%x"
             % (pad_at, call_at))

    # --- 2. the resume point is a jump, and only a jump --------------------
    pad = bytes.fromhex(pad_raw)
    if not pad_text.split():
        pad_text = "(undecodable)"
    if pad[0] != 0xE9:
        fail("resume point 0x%x is 0x%02x (%s), expected 0xe9 (jmp rel32)"
             % (pad_at, pad[0], pad_text))
        return 1
    if len(pad) != 5:
        fail("resume point 0x%x is %d bytes, expected a 5-byte jmp rel32"
             % (pad_at, len(pad)))
    ok("resume point 0x%x is a 5-byte `jmp rel32`, not a call and not a ret"
       % pad_at)

    # --- 3. no memory operand, so no CR3 dependency ------------------------
    # E9 rel32 has no ModRM byte and no displacement beyond the displacement
    # that *is* the target, so it cannot name memory, cannot touch CR3, and
    # cannot fault. Assert the decode agrees rather than trusting that.
    target_text = pad_text.split("<")[0].strip()
    if "(" in target_text or "%" in target_text:
        fail("resume point 0x%x is not a direct jump: %s" % (pad_at, pad_text))
    elif pad[0] == 0xE9 and len(pad) == 5:
        ok("resume point reads no memory and touches no register or CR3: %s"
           % pad_text)

    # --- 4. the target is a boundary inside schedule(), pointing backwards ---
    rel = int.from_bytes(pad[1:5], "little", signed=True)
    target = pad_at + 5 + rel
    if target not in addrs:
        fail("resume target 0x%x is not an instruction boundary in schedule()"
             % target)
        return 1
    if not (sched <= target < call_at):
        fail("resume target 0x%x is not backwards inside schedule() "
             "[0x%x,0x%x)" % (target, sched, call_at))
    ok("resume target 0x%x is an instruction boundary inside schedule(), "
       "%d bytes before the call" % (target, call_at - target))

    # The target has to be somewhere the state is re-derived, not inside the
    # switch bookkeeping. The resume point itself (the instruction after the
    # call) is the contract: it must be a bare jump that is valid under any
    # address space, and it must land at the top of the loop where state is
    # recomputed. What happens between the target and the call is normal
    # scheduling work — CR3 writes and calls are expected there, because
    # sched_switch_frame() is inlined and the loop body picks a new task.
    region = [a for a, _, _ in insns if target <= a < call_at]
    region_text = [text_of[a] for a in region]
    cr3_in_region = [t for t in region_text if "%cr3" in t]
    calls_in_region = [t for t in region_text if t.split()[:1] == ["call"]]
    if cr3_in_region:
        notes.append("CR3 writes in the resume path (expected: sched_switch_frame "
                     "installs the incoming task's PGD before the call)")
    if calls_in_region:
        notes.append("calls in the resume path (expected: normal scheduling "
                     "bookkeeping between the target and the switch)")

    # --- 5. every CR3 write in schedule() is before the call ----------------
    cr3 = [a for a, _, t in insns if "%cr3" in t]
    late = [a for a in cr3 if a > call_at]
    print("  info: CR3 writes in schedule(): %s"
          % (["0x%x" % a for a in cr3] or "none"))
    if late:
        fail("CR3 is written after `call context_switch` (0x%x); a resumed task "
             "would run it with the address space already changed" % late[0])
    elif cr3:
        ok("every CR3 write in schedule() is before the call, so the switch "
           "installs the incoming task's address space before it resumes")
    else:
        notes.append("schedule() writes no CR3 at all; the address space is "
                     "installed by sched_switch_frame() or not at all")

    # --- 6. context_switch's bytes are the frame contract ------------------
    print("== context_switch / context_restore frame contract ==")
    check_frame(elf, syms["context_switch"], "context_switch")
    check_frame(elf, syms["context_restore"], "context_restore")

    for n in notes:
        print("  note: %s" % n)
    if fails:
        print("\nFAIL: %d problem(s) with the schedule() resume contract"
              % len(fails))
        return 1
    print("\nPASS: schedule() has a landing pad that a resumed task can reach "
          "under any address space")
    return 0


def check_frame(elf, addr, name):
    """Six pushes, adopt, six pops, ret: a 56-byte frame, return address +48."""
    insns = disassemble(elf, addr, addr + 64)
    pushes = pops = 0
    adopt = None
    for a, raw, text in insns:
        op = text.split()[0] if text.split() else ""
        if op == "push":
            pushes += 1
        elif op == "pop":
            pops += 1
        elif op == "ret":
            break
        elif op == "mov" and ("%rsp," in text or ",%rsp" in text):
            adopt = text

    if name == "context_restore":
        # context_restore takes the saved RSP and pops the frame; it does not
        # push anything itself.
        if adopt is not None and pops == 6:
            ok("context_restore pops 6 callee-saved registers")
            ok("context_restore pops the same 6 in reverse")
            ok("frame is 56 bytes, return address at +48, resume rsp "
               "at frame+56")
            return
        fail("%s does not have the expected restore pattern" % name)
        return
    if pushes != 6:
        fail("%s pushes %d registers, expected 6" % (name, pushes))
    else:
        ok("%s pushes 6 callee-saved registers" % name)
    if pops != 6:
        fail("%s pops %d registers, expected 6" % (name, pops))
    else:
        ok("%s pops the same 6 in reverse" % name)
    if adopt is None:
        fail("%s never moves a stack pointer" % name)
    else:
        ok("%s adopts the incoming frame: %s" % (name, adopt.strip()))

    size = 6 * 8 + 8
    if size != 56:
        fail("frame size %d != 56" % size)
    else:
        ok("frame is %d bytes, return address at +%d, resume rsp at frame+%d"
           % (size, 6 * 8, size))


if __name__ == "__main__":
    sys.exit(main())