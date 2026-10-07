# Privilege Levels

## Hardware Privilege Model

x86-64 provides four privilege rings (0–3). This OS uses two:

| Ring | Name | Usage |
|---|---|---|
| 0 | Kernel mode | All kernel code, interrupt handlers, kernel threads |
| 3 | User mode | All userspace code, including userspace drivers |

Rings 1 and 2 are unused. The hardware enforces ring separation: code running in ring 3 cannot execute privileged instructions or access kernel memory directly.

## Global Descriptor Table (GDT)

The GDT defines the segment descriptors used throughout the system. Each CPU has its own GDT (per-CPU, loaded in `GDTR`):

| Index | Descriptor | DPL | Description |
|---|---|---|---|
| 0 | Null | — | Required by x86 spec |
| 1 | Kernel code | 0 | 64-bit, execute-only, ring 0 |
| 2 | Kernel data | 0 | 64-bit, read/write, ring 0 |
| 3 | User code (32-bit) | 3 | 32-bit compat mode selector |
| 4 | User data | 3 | 64-bit, read/write, ring 3 |
| 5 | User code (64-bit) | 3 | 64-bit, execute-only, ring 3 |
| 6–7 | TSS | 0 | Task State Segment (128-bit entry) |

In 64-bit mode, segment base/limit values are ignored (flat model). GDT entries matter only for DPL (privilege level) enforcement.

The table above is normative, not illustrative. Two of its rows look redundant and are not:

- **Index 3 is a 32-bit descriptor that is never used to execute user code *on the SYSRET path*.** It exists because `SYSRET` derives CS and SS arithmetically from `STAR`, and the arithmetic only lands on the right pair if the ring-3 code base sits at index 3. See "Selector Arithmetic" below.
  **It is used on the SYSCALL path, and that is a live bug.** `SYSCALL` loads CS from `STAR[47:32] + 16`, which with `KERNEL_CODE_SELECTOR == 0x08` is `0x18` — index 3, a 32-bit descriptor — and loading a non-long code segment while `EFER.LMA = 1` raises `#GP`. Linux uses the same `STAR` value only because its index 3 is a 64-bit DPL-3 descriptor. Ours is not. See finding #58 in [`../../../MEGA_AUDIT.md`](../../../MEGA_AUDIT.md).
- **Index 5 is the user code descriptor `SYSRET` and `IRETQ` actually execute.** `SYSRET` adds 16 to the `STAR` base, landing on index 5, which is 64-bit. `IRETQ`, which does no arithmetic and takes its selectors from the stack, is given index 5 explicitly.

### Selector Arithmetic

`STAR` bits 63:48 hold the ring-3 base selector. The CPU computes:

```
SYSRET:  CS = STAR[63:48] + 16      SS = STAR[63:48] + 8
SYSCALL: CS = STAR[47:32]           SS = STAR[47:32] + 8
```

With `STAR[63:48] = 0x1B` (index 3, RPL 3):

| Register | Value | Descriptor |
|---|---|---|
| CS | `0x1B + 16` = `0x2B` | index 5 — 64-bit user code |
| SS | `0x1B + 8` = `0x23` | index 4 — user data |

Both computed selectors carry RPL 3, because the additions do not disturb bits 0-1. There is no way to tell the CPU a different SS, so the GDT has to be arranged to make the arithmetic correct rather than choosing selectors freely. `syscall.c` owns the `STAR` write; `src/include/gdt.h` owns the selector constants, and `USER_CODE_SELECTOR` is defined as `0x1B` precisely because it is the `STAR` base, not because it is the descriptor user code runs on.

### Descriptor Encodings

Descriptors are built by `GDT_ENTRY(access, flags, base, limit)` in `src/include/gdt.h`, which packs the raw bit fields so the bootloader and the kernel cannot disagree about an encoding. **The access byte and the flags/granularity byte are separate arguments**, and conflating them is the defect that 0.15 in [`../../../MEGA_AUDIT.md`](../../../MEGA_AUDIT.md) records: folding the flags nibble into the access byte left every kernel code segment with `L=0`, and each CS load `#GP`ed. The flags are passed in the `GDT_FLAG_*` positions of byte 6 — `GDT_FLAG_GRANULARITY` is the `G` bit (3), `GDT_FLAG_DB` the `B/DB` bit (2), `GDT_FLAG_64BIT`/`GDT_FLAG_LONG_MODE` the `L` bit (1). Two forms exist:

- **Code/data segments** are one 8-byte entry; the macro already accounts for the base being split across the low 24 bits and the high byte of the last qword.
- **The TSS is a system descriptor spanning two GDT slots.** In long mode the TSS base is 64 bits, which does not fit in the 24-bit base field, so the high 32 bits live in the second slot. There is no single 16-byte-producing macro: `GDT_TSS_DESC64_LOW(base, limit)` and `GDT_TSS_DESC64_HIGH(base)` build the two halves separately (`gdt.c:175` and `gdt.c:177`) and each is written as one 8-byte store, which is also how they appear in the GDT. `_LOW` carries `base[31:0]`; `_HIGH` carries **`base[63:32]` and nothing else**. Both mistakes here have been made in this tree: byte 7 built as an eight-bit `(base >> 24) & 0xFF` instead of four bits of `base[31:28]`, which double-counts `base[27:24]` and made the reconstructed TSS base non-canonical (0.16); and a `_HIGH` that also carried `base[24:55]`. The round-trip `_Static_assert`s at the bottom of `gdt.h` decode the encoders back into their fields, so a wrong shift is a build failure now — and that is the only reason the second of those was caught in seconds rather than days.

### The Reload Itself

`LGDT` does not take effect on its own. CS keeps its cached descriptor until something reloads it, and a near jump does not do that — the hardware's answer is a far return, which is what `gdt_flush` uses. The frame for a 64-bit far return is **two words**:

```
    pushq $KERNEL_CODE_SELECTOR     /* CS */
    pushq $.Lgdt_flush_resume       /* RIP */
    lretq
```

`LRETQ` pops RIP and CS and **nothing else**. It does not restore RFLAGS; only `IRET` pops flags. The 32-bit idiom — EFLAGS, CS, EIP, three dwords for a three-dword `LRET` — does not survive the widening, and the surplus word is not inert. It stays on the stack, and the `ret` at the resume label pops it as a return address, so control transfers to whatever was pushed there. With the frame this code was originally written against that is linear `0x202`, and the instruction fetch faults: `#PF`, error code `0x0010` (instruction fetch, protection violation), CR2 = `0x202`.

Measured, not derived: at the resume label the guest printed its own RSP rather than trusting a debugger, and RSP had advanced **16** while the word at RSP was still the `0x202` that had been pushed — one slot *below* the real return address. Identical under TCG and under KVM. The built artefact agrees from the other side: `objdump -d build/kernel.elf` shows `push $0x8` / `push $0xffffffff80010ccb` / `lretq` / `ret` in `gdt_flush`, and that binary boots past the reload into `sched_init`, PID 1 and the scheduler — which a three-word frame cannot do, because the third pop would land in `gdt_reload`'s own frame (`push %rbx; sub $0x10,%rsp`) and the following `ret` would jump to a saved `%rbx`.

**One trap for the next person, because the published pseudocode says otherwise.** The SDM's `LRET` description, in its IA-32e-mode section, shows a third pop — `RIP := Pop(); CS := Pop(); tempRFLAGS := Pop();` for the 64-bit operand size. That does not match the machine, and the machine is what this document is for. Do not read that pseudocode and "restore" the third push; the two measurements above are what the hardware does, and the standard stack-switching idiom agrees (it pushes two words and uses `lretq $8` to discard the old return address, which would be `lretq $16` if a third word were popped). If someone re-derives this from the manual alone, they will produce a confidently wrong three-word frame — which is the defect this section exists to describe.

`interrupt_entry.S:242-255` says all of this in the source, and the reason is worth the comment: the frame is not wrong-looking, the comment above it explained the far return correctly, and the extra word does nothing at all until the following `ret` consumes it.

Two more rules, both properties of the encoding rather than of the source, and both of which have cost real time here:

- **`pushq $sym` and `pushq sym` are different instructions.** With the `$`, GAS emits `68 <imm32>` (PUSH imm). Without it, `pushq .Llabel` is a memory dereference and GAS emits `ff 34 25 <disp32>` (PUSH r/m), which pushes the *contents* at that address — here, the label's own instruction bytes. objdump prints `push 0xffffffff80010aa2` for `ff 34 25 a2 0a 01 80`: it prints the memory operand's **target**, which is exactly the address a push-immediate would have named, and it prints no `$` to tell the two apart. A listing line that looks right is not a check of the encoding — read the opcode and the ModRM byte.
- **A 64-bit far jump cannot reach the kernel.** The far-jump opcode `EA` carries a 16-bit IP offset, so `ljmp $cs, $label` only works when the target is in the low 64 KiB. That is why the bootloader's far jumps are legal — stage2 runs at `0x8000` — and why the same instruction is unusable for a kernel at `0xffffffff8...`. The mode switch in `stage2_long.S` gets away with it because the jump is *executed* in 32-bit mode and lands low. The hand-built far-return frame above is the mechanism that works at any address, which is the other reason it exists.

### A Segment Reload Destroys That Register's Hidden Base

`gdt_flush` reloads GS with a selector (`interrupt_entry.S:222`). That is not optional: a MOV to a segment **selector** is the only thing that refreshes a segment register's cached descriptor after `LGDT`, and it is exactly what makes the reload real rather than decorative. It also has a side effect that nothing in the source mentions and nothing in the sequence undoes — **loading a selector replaces the hidden base with the one from the descriptor**, which for a flat segment is zero.

For FS and GS that hidden base is not a limit or an access byte, it is the per-CPU pointer: the whole `this_cpu()` mechanism is a GS-relative access through `IA32_GS_BASE`. So the reload that makes the segment registers correct throws the per-CPU area away at the same moment, and the next `this_cpu()` dereferences address 0. Measured: every subsystem logged through `cpuid`, `vmm`, `pmm`, `idt` and `pit`, and then `gdt_reload()`'s **own** `klog()` faulted at `this_cpu()->cpu_id` with CR2 = 0, one call after `gdt_flush()`.

Deleting `mov %ax, %gs` also makes it boot, and **is not a fix**: it leaves GS on a descriptor from the loader's table, which is the condition the reload exists to prevent. The correct sequence is reload-then-reinstall, and the reinstall lives in C where the CPU number is known:

```c
gdt_flush((uint64_t)&gdtr, KERNEL_CODE_SELECTOR, KERNEL_DATA_SELECTOR);
percpu_install_gs_base(cpu);          /* gdt.c:199 — the base the reload destroyed */
tss_flush(TSS_SELECTOR);
klog(KLOG_INFO, "gdt: cpu %u, …");    /* the very next statement reads this_cpu() */
```

`percpu_install_gs_base()` (`percpu.c:111`) **takes the CPU number as an argument** rather than calling `this_cpu()` to discover it, for a reason that is easy to get backwards: `this_cpu()` is precisely what has just been broken, so reading the CPU id here would fault on the line written to repair the fault. It is a separate function from `percpu_setup()` for the same reason — a *correct* segment reload destroys the base, and a boot-time setup function cannot be the thing that puts it back after every reload.

## Task State Segment (TSS)

Each CPU has a TSS. The TSS in 64-bit mode is used exclusively for:

- **RSP0**: Kernel stack pointer loaded on ring-3 → ring-0 transition (interrupt or syscall).
- **IST1–IST7**: Interrupt Stack Table — alternate stacks for specific exception handlers (NMI, double fault).

The TSS is updated on every context switch to point RSP0 at the incoming thread's kernel stack.

Only `RSP0` and the IST pointers are used. `RSP1`/`RSP2` (ring 1/2 stacks) exist in the structure and stay zero, because rings 1 and 2 are unused. The I/O permission bitmap base is set to the end of the structure, which denies all port I/O from ring 3 — there is no bitmap, so there is nothing to permit.

### TSS Layout

The 64-bit TSS is 80 bytes, not counting any I/O permission bitmap that may follow it. The offsets are architectural and the interrupt path depends on two of them:

| Offset | Size | Field |
|---|---|---|
| 0x00 | 4 | reserved |
| 0x04 | 8 | `RSP0` |
| 0x0C | 8 | `RSP1` |
| 0x14 | 8 | `RSP2` |
| 0x1C | 8 | reserved |
| 0x24 | 4×7 | `IST1`–`IST7` (32-bit each) |
| 0x40 | 8 | reserved |
| 0x48 | 8 | I/O permission bitmap base |

`RSP0` is a full 64-bit field in long mode. The 16-bit interpretation applies only in 32-bit mode, and reading it as 16 bits here would truncate a kernel stack address and fault on the first interrupt from ring 3.

`gdt.c` asserts every field offset at compile time rather than trusting the comment; if the struct and the table ever drift, the kernel does not build rather than faulting on the first user interrupt.

### IST Stacks

The IST field is 32 bits. The kernel is linked at `0xFFFFFFFF80000000` with its direct map at `0xFFFF800000000000`, so a page obtained from `vmalloc()` is not representable in that field — truncating it would point the CPU at a low linear address that is not mapped, and a double fault landing there would fault again with nowhere left to go.

IST stacks are therefore mapped at *fixed low linear addresses* by writing directly into the kernel page tables. `gdt_set_ist_stack()` is the entry point and asserts that the address it is given is representable in 32 bits. The boot log states which vectors have a dedicated stack.

### Kernel Interfaces

```c
void gdt_reload(uint32_t cpu);      /* build and load the GDT, then LTR the TSS */
void percpu_install_gs_base(uint32_t cpu);  /* re-install GS.base after a segment reload */
void tss_set_kernel_stack(void *stack_top);  /* set RSP0 */
```

`gdt_reload` is called during boot before the IDT exists, because the TSS supplies `RSP0` for every ring-3 transition: a CPU taking an interrupt with `RSP0` unset loads a null stack and faults inside the fault handler, before it can report anything.

`gdt_reload` also calls `percpu_install_gs_base` immediately after `gdt_flush` and before the `klog()` that follows, because the segment reload destroys `GS.base` — see "A Segment Reload Destroys That Register's Hidden Base" above. Any future code that reloads a segment register has the same obligation, and it is not optional.

`tss_set_kernel_stack` is called again on every context switch, so an interrupt always lands on the stack of the task currently running on that CPU.

## Syscall Mechanism

The SYSCALL/SYSRET instruction pair is the primary fast-path ring transition:

- `SYSCALL`: Transitions ring 3 → ring 0. Saves `rip` (into `rcx`) and `rflags` (into `r11`). Sets `CS` and `SS` from `STAR` MSR. Jumps to the kernel entry point in `LSTAR` MSR.
- `SYSRET`: Transitions ring 0 → ring 3. Restores `rip` from `rcx`, `rflags` from `r11`. Sets `CS` and `SS` back to user segments from `STAR`.

MSR configuration (set during CPU initialization):

```
LSTAR  = address of kernel syscall entry stub
STAR   = (kernel_cs << 32) | (user_cs << 48)
SFMASK = RFLAGS bits to clear on SYSCALL entry (IF, DF, AC)
```

## Interrupt Ring Transitions

Interrupts and exceptions always transition to ring 0, regardless of the current CPL. The IDT entry specifies the handler's DPL:

- Hardware interrupts: DPL=0 (not callable from user mode via `INT n`).
- Software breakpoint (vector 3): DPL=3 (callable from user mode for debug purposes).

### The Gate Type Byte: Interrupt Gate vs Trap Gate

The IDT entry's type/attribute byte is `P | DPL | 0 | type`: present in bit 7,
DPL in bits 6:5, the system bit (0 for a gate) in bit 4, and the four-bit gate
type in bits 3:0. In long mode there are **exactly two valid gate types**, and
they differ in one thing only:

| Type | `type_attr` (DPL 0) | Meaning |
|---|---|---|
| `0b1110` | `0x8E` | 64-bit **interrupt** gate — clears IF on entry, IRET restores it |
| `0b1111` | `0x8F` | 64-bit **trap** gate — leaves IF unchanged |

`IDT_TYPE_INTERRUPT_GATE` is `0x8E` (`idt.c:74`), which is the **interrupt** gate.

**There is no size bit, and no 32-bit gate in a 64-bit IDT.** The gate types that
are described as "16-bit" (`0x6`, `0x7`) and "32-bit" (`0xE`, `0xF`) belong to the
*32-bit* IDT, whose entries are 8 bytes and whose offset is 32 bits wide. A 64-bit
IDT is indexed with a 16-byte stride and its offset field is 64 bits wide, so the
`offset_high` half is loaded in full. The same type values mean "64-bit" there.
`0x8E` is the 64-bit interrupt gate; the three sources that agree on this are the
SDM's 64-bit gate-descriptor table, Linux's `arch/x86/include/asm/desc_defs.h`
(`GATE_INTERRUPT = 0xE`, `GATE_TRAP = 0xF`, in the same struct that carries
`offset_high`), and the fact that the 16-byte `struct idt_entry` at `idt.c:42-50`
splits the handler address across three fields and is correct — there is nothing
for a gate to truncate.

That last point is worth stating as a warning, because the opposite belief is
already in this tree's history. The claim that "the low three bits of the type
field are the gate's size, `0b1110` is the 32-bit gate, and a 32-bit gate enters
a handler linked at `0xffffffff800108c7` at `0x000108c7`" is false, and it was
used as the stated reason to change the constant to `0x8F`. Nothing was fixed; a
correct interrupt gate became a trap gate. The reasoning survives in commit
`ed8b3cf`, in the comment at `idt.c:52-68` (which states that `0x8F` "clears IF on
entry", the one thing a trap gate does not do) and in `STATE.md` §2 and §9. See
correction **A1.10** in [`../../../MEGA_AUDIT.md`](../../../MEGA_AUDIT.md).

**Why the interrupt gate matters.** An interrupt gate clears IF on entry; a trap
gate does not. The entry stub in `interrupt_entry.S` has no `cli` and no `sti` —
`iretq` is the only thing that restores flags, and it restores the flags that were
saved on entry. So with `0x8E` (interrupt gate) an exception handler runs with
interrupts **disabled** for its duration; with `0x8F` (trap gate) it would run with
them **enabled**. No IST stack is installed, so a handler runs on the interrupted
stack, and `idt_init()` unmasks IRQ0 and programs the PIT at 100 Hz. If the gate
were a trap gate, a tick could land inside a page-fault handler, and a `#PF` raised
inside the `#PF` handler would recurse immediately instead of being contained —
the triple-fault path, on a stack with no dedicated alternative.

That is a reading of the mechanism, not a measured fault: a handler that runs long enough for a tick to land in one must use an interrupt gate so that the nested fault is contained rather than recursing. The generalisation is the one this document keeps having to relearn — **a gate type that clears one flag instead of another is a behavioural change with no signature at build time and none at boot time either.**

## User/Kernel Memory Separation

Page table entries carry a `U/S` (User/Supervisor) bit:

- **U=0 (Supervisor)**: Page accessible only in ring 0. All kernel memory uses this.
- **U=1 (User)**: Page accessible from ring 3.

**Neither SMAP nor SMEP is enabled.** Both bits are defined
(`CR4_SMEP`, `CR4_SMAP` in `src/kernel/include/cpu_features.h`) and neither is
ever written to CR4. Until they are, a ring-3 process can read and write the
kernel's per-CPU area: `MSR_GS_BASE` is programmed to `&percpu_data[cpu]`, a
`GS`-relative access resolves through that MSR regardless of CPL, and both the
kernel GS-base MSRs are set to the same value, which makes the syscall path's
`swapgs` a no-op. So `movq %gs:0x0, %rax; movq 0x10(%rax), %rbx` in ring 3 reads
`percpu_data[cpu].current` and writes it. This is a direct ring-3 → ring-0
kernel-pointer-write primitive. See finding #61.

Nothing on the boot path reports the hole, and a boot log is not where to look
for it. The kernel's own per-CPU self-check (`gs_base_install()`,
`percpu.c:69-91`) proves the base through three independent routes — the MSR,
`this_cpu()`, and a `GS`-relative load — and on a correct boot **all three
pass and it prints nothing**. It used to log `percpu: gs base verification failed`
and carry on, which is why the message appears in older logs; it now treats a
failed check as fatal (`percpu.c:143`), because the old behaviour left `online`
false and `num_cpus` at 0 with no base installed, and every `this_cpu()` caller
went on reading and writing whatever happened to be mapped at the address it
returned. That is the quietest shape of failure there is, and none of it says
anything about SMAP.

## Related Documents

- [interrupt-handling.md](interrupt-handling.md)
- [context-switching.md](context-switching.md)
- [syscall-abi.md](syscall-abi.md)
- [security/overview.md](../security/overview.md)
