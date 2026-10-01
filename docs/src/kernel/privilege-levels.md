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
| 3 | User code (32-bit) | 3 | For 32-bit compat mode (future) |
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

Descriptors are built by `GDT_ENTRY(flags, base, limit)` in `src/include/gdt.h`, which packs the raw bit fields so the bootloader and the kernel cannot disagree about an encoding. Two forms exist:

- **Code/data segments** are one 8-byte entry; the macro already accounts for the base being split across the low 24 bits and the high byte of the last qword.
- **The TSS is a system descriptor spanning two GDT slots.** In long mode the TSS base is 64 bits, which does not fit in the 24-bit base field, so the high 8 bits live in the second slot. `GDT_TSS_DESC64(base, limit)` produces the full 16 bytes; the two halves are written as separate 8-byte stores, which is also how they appear in the GDT.

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

### IST Stacks Are Not Yet Installed

The IST field is 32 bits. The kernel is linked at `0xFFFFFFFF80000000` with its direct map at `0xFFFF800000000000`, so a page obtained from `vmalloc()` is not representable in that field — truncating it would point the CPU at a low linear address that is not mapped, and a double fault landing there would fault again with nowhere left to go.

Installing them therefore needs a page mapped at a *fixed low linear address*, which means writing into the kernel page tables directly, and the VMM does not yet expose the kernel PGD. `gdt_set_ist_stack()` is the entry point and asserts that the address it is given is representable in 32 bits; until something can supply one, the IST pointers stay zero.

The IDT handles this rather than assuming: `idt.c` asks `gdt_have_ist()` before putting an IST index in a gate, and leaves it at zero when no stack exists. A gate that claims an IST slot the TSS cannot honour does not fall back to the normal stack — it takes a `#PF` on entry, which is strictly worse than having no IST. The boot log states which of the two vectors currently has a dedicated stack.

### Kernel Interfaces

```c
void gdt_reload(uint32_t cpu);      /* build and load the GDT, then LTR the TSS */
void tss_set_kernel_stack(void *stack_top);  /* set RSP0 */
```

`gdt_reload` is called during boot before the IDT exists, because the TSS supplies `RSP0` for every ring-3 transition: a CPU taking an interrupt with `RSP0` unset loads a null stack and faults inside the fault handler, before it can report anything.

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

## User/Kernel Memory Separation

Page table entries carry a `U/S` (User/Supervisor) bit:

- **U=0 (Supervisor)**: Page accessible only in ring 0. All kernel memory uses this.
- **U=1 (User)**: Page accessible from ring 3.

**Neither SMAP nor SMEP is enabled.** Both bits are defined
(`CR4_SMEP`, `CR4_SMAP` in `src/kernel/include/cpu_features.h`) and neither is
ever written to CR4. Until they are, a ring-3 process can read and write the
kernel's per-CPU area: `MSR_GS_BASE` is programmed to `percpu_data`, a
`GS`-relative access resolves through that MSR regardless of CPL, and both the
kernel GS-base MSRs are set to the same value, which makes the syscall path's
`swapgs` a no-op. So `movq %gs:0x0, %rax; movq 0x10(%rax), %rbx` in ring 3 reads
`percpu_data[0].current` and writes it. This is a direct ring-3 → ring-0
kernel-pointer-write primitive. See finding #61.

The kernel's own boot log reports it:
`percpu: gs base verification failed`.

## Related Documents

- [interrupt-handling.md](interrupt-handling.md)
- [context-switching.md](context-switching.md)
- [syscall-abi.md](syscall-abi.md)
- [security/overview.md](../security/overview.md)
