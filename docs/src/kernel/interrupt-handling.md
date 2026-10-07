# Interrupt Handling

## Architecture

The OS uses a fully preemptible interrupt architecture with per-CPU routing via the Advanced Programmable Interrupt Controller (APIC)[^intel-sdm-interrupts][^amd-apm-interrupts][^acpi-apic].

## Interrupt Descriptor Table (IDT)

The IDT has 256 entries (vectors 0–255)[^intel-sdm-interrupts]:

| Vector Range | Usage |
|---|---|
| 0–31 | CPU exceptions (fault/trap/abort types per Intel spec) |
| 32–47 | Legacy IRQ remapping (if using 8259A PIC in compatibility mode) |
| 48–239 | APIC-routed device interrupts |
| 240–254 | Inter-processor interrupts (IPIs) — scheduler, TLB shootdown, etc. |
| 255 | Spurious interrupt vector |

Each IDT entry specifies:

- Handler address (64-bit)
- Privilege level required (ring 0 for all except software interrupts used by legacy code)
- Interrupt Stack Table (IST) slot (for NMI and double-fault, which use dedicated stacks)

## APIC Configuration

Each CPU has a Local APIC (LAPIC)[^intel-sdm-interrupts][^acpi-apic]. The I/O APIC routes external interrupt lines to LAPIC vectors on specific CPUs[^acpi-apic].

**LAPIC initialization** (per CPU, during SMP bring-up):

1. Enable LAPIC (`SVR` register, bit 8).
2. Set spurious interrupt vector (255).
3. Configure LAPIC timer for scheduler tick (periodic mode, calibrated against HPET/TSC).
4. Set task priority register (TPR = 0, accept all interrupt priorities).

**I/O APIC routing** (CPU 0 during early boot):

- Each interrupt source is assigned a fixed vector in the 48–239 range.
- Routing is set to the BSP (CPU 0) initially; per-CPU affinity is configured by the interrupt affinity subsystem.

## Per-CPU Interrupt Routing

Device interrupts can be routed to specific CPUs using I/O APIC redirection table entries. The interrupt affinity subsystem distributes interrupt load across CPUs:

- Default: interrupts distributed round-robin across online CPUs.
- Override: device driver or admin can pin an interrupt to a specific CPU via the `irq_set_affinity()` kernel API.

## Interrupt Entry and Exit

**Entry** (assembly stub, one per vector):

1. Push error code (synthetic zero if not present).
2. Push vector number.
3. Save the argument registers `rdi, rsi, rdx, rcx, r8, r9, r10, r11`.
4. Call `interrupt_dispatch(regs)`.
5. Restore, discard the vector and error code, `iretq`.

**The stub does not switch stacks.** By the time the first stub instruction runs, the CPU has already chosen the stack:

| Condition | Stack the CPU selected |
|---|---|
| `IST != 0` | the dedicated IST stack for that vector |
| `IST == 0`, CPL changed | `TSS.RSP0` — the current task's kernel stack |
| `IST == 0`, CPL unchanged | the interrupted stack, already a kernel stack |

This is worth stating because doing it anyway is a real bug rather than a harmless redundancy. A stub that loaded `RSP0` itself would overwrite the IST stack for double fault, which is the one case the IST exists for: double fault fires precisely when the previous handler's stack is already unusable. Overwriting it converts a recoverable panic into an unbounded recursion.

It also means the kernel never runs C on a user stack. That guarantee comes from the CPU, not from the stub, which is why removing the switch does not weaken it.

Step 1 always pushes a word, even for the 25 vectors that do not fault with an error code. The alternative — a variable-size frame — means every C handler has to know which of seven entry points it is, and the one that gets it wrong reads a garbage error code. A synthetic zero costs one word of stack and makes the frame a fixed size.

## Interrupt Frame

The stub builds this layout, which `struct interrupt_frame` in `interrupt.h` mirrors field for field. Offsets are from the stack pointer at the moment `interrupt_dispatch` is entered:

| Offset | Field | Source |
|---|---|---|
| 0x00 | `rax` | stub |
| 0x08 | `rdi` | stub |
| 0x10 | `rsi` | stub |
| 0x18 | `rdx` | stub |
| 0x20 | `rcx` | stub |
| 0x28 | `r8` | stub |
| 0x30 | `r9` | stub |
| 0x38 | `r10` | stub |
| 0x40 | `r11` | stub |
| 0x48 | `vector` | stub |
| 0x50 | `error_code` | CPU, or synthetic 0 |
| 0x58 | `rip` | CPU |
| 0x60 | `cs` | CPU |
| 0x68 | `rflags` | CPU |
| 0x70 | `rsp` | CPU |
| 0x78 | `ss` | CPU |

The last five words are the `IRETQ` frame the CPU pushed[^intel-sdm-interrupts], and they are the interrupted context. The `ss` field is only meaningful when `cs` shows ring 3; for a kernel-origin interrupt the CPU pushes a zero there, and a handler must not use it.

`interrupt_dispatch` must not modify the CPU-saved half of the frame (`rip` through `ss`) in place. A handler that wants to change where the interrupted code resumes — delivering a signal, for instance — records the new values in the task structure; rewriting the frame here would be invisible to the scheduler, which reloads the frame from that structure on the next switch.

The static assertions in `interrupt.h` (lines 244–257) pin these offsets at compile time against the assembly stubs in `interrupt_entry.S`.

**`interrupt_dispatch`** (C):

1. Increment per-CPU preemption depth.
2. Look up handler for vector.
3. Call handler.
4. Send End-Of-Interrupt (EOI) to LAPIC.
5. Decrement preemption depth.
6. If preemption depth == 0 and reschedule flag set: call scheduler.
7. Return to interrupted context.

Step 5 must happen even if the handler does not return: every exception that can panic unwinds through here, and leaving the depth incremented would make the panic path think it is nested in an interrupt and skip the final diagnostics.

**Exit**: `iretq` restores the registers and returns to the interrupted code, user or kernel. Nothing is popped in C; the whole frame is consumed by the instruction that ends the stub, which is why the stub restores the stack pointer it saved before switching rather than relying on anything the C code left behind.

## Kernel Interfaces

```c
void idt_init(void);       /* build the IDT, install the IST stacks, load IDTR */
void exceptions_init(void);/* install handlers for vectors 0-31 */
void idt_set_handler(uint8_t vector, irq_handler_t handler, uint8_t ist, uint8_t dpl);
void idt_request_reschedule(void);
```

Both init calls come from `main.c`, `exceptions_init()` first, so that the table is fully populated before the `IDTR` is loaded. An `IDTR` pointing at half-filled entries is not a hazard on its own, but there is no reason to expose one.

`idt_request_reschedule` is the timer driver's side of the dispatch step above: it sets a per-CPU request that the dispatch path consumes once the preemption depth is back to zero. Keeping it a function rather than a flag the driver pokes directly means the per-CPU indexing and the outermost-level check live in one place.

Step 4 of `interrupt_dispatch` acknowledges the 8259. The LAPIC EOI is a single MMIO write in the sequence. The 32–47 range acknowledgement is load-bearing for the timer interrupt path.

## Handler Registration

```c
void idt_set_handler(uint8_t vector, void (*handler)(struct interrupt_frame *),
                     uint8_t ist, uint8_t dpl);
```

Vectors 0–31 are filled by `exceptions_init()`, which is separate from `idt_init()` so that the table's policy — which exception is fatal, which is deliverable — is in one place rather than spread through the installer. Hardware IRQs are registered by the driver that owns the device; a vector with no handler installed is counted and otherwise ignored, so an unregistered device does not crash the machine.

`dpl` is 0 for everything except vector 3, which is 3 so a user process can raise `int3` for a breakpoint. Nothing else is callable from ring 3: a `DPL=3` entry for a device vector would let any process interrupt any other at will.

### IST Assignment

Two vectors are *meant* to get a dedicated stack, because a fault on the current stack leaves nothing to fault onto:

| Vector | IST | Reason |
|---|---|---|
| 8 — double fault | IST1 | The exception that fires when the *handler's* stack faults. Without a separate stack this is infinite recursion that resets the machine. |
| 2 — NMI | IST2 | NMI must be handled with interrupts off and is used as the last-resort halt path, so it cannot share a stack that may already be exhausted. |

All other vectors use IST 0, which means "the stack the CPU would otherwise use".

**These two are assigned dedicated IST stacks.** The IST field is 32 bits and the kernel lives in the high half, so the stacks are mapped at fixed low linear addresses. See "IST Stacks" in [privilege-levels.md](privilege-levels.md).

`idt.c` therefore asks `gdt_have_ist()` before writing an IST index into a gate and leaves it at zero when no stack is installed. A gate that claims an IST slot the TSS cannot honour takes a `#PF` on entry — strictly worse than running on the interrupted stack — so the gate and the TSS pointer are kept consistent by construction rather than by convention.

## Exception Handling

CPU exceptions are handled according to their type[^intel-sdm-interrupts]:

| Exception | Type | Handler |
|---|---|---|
| Page fault (#PF) | Fault | `page_fault_handler` — may allocate page or kill process |
| General protection fault (#GP) | Fault | Kill faulting process (user) or kernel panic |
| Double fault (#DF) | Abort | Kernel panic, always; uses IST stack |
| Division by zero (#DE) | Fault | Send SIGFPE to process |
| Breakpoint (#BP) | Trap | Deliver to process or kernel debugger |
| Invalid opcode (#UD) | Fault | Send SIGILL to process |

The #PF case delegates to `vmm_handle_page_fault(addr, error_code)`, which is where the VMM distinguishes a genuine bug from demand paging and from a stack growth. The exception layer classifies and reports; it does not decide what is recoverable, because that decision needs the page tables.

The "deliver to process" actions for #DE, #UD and #BP use the signal subsystem,
specified in [ipc/signals.md](../ipc/signals.md). The exception layer classifies
and reports; it does not decide what is recoverable, because that decision needs
the page tables.

## Inter-Processor Interrupts (IPIs)

IPIs are sent via LAPIC ICR (Interrupt Command Register)[^intel-sdm-interrupts][^acpi-apic]. Used for:

| Vector | Purpose |
|---|---|
| 240 | TLB shootdown (page table update propagation) |
| 241 | Scheduler reschedule request |
| 242 | CPU halt (system shutdown or panic broadcast) |
| 243–254 | Reserved for future IPI types |

## Fast Path

- IDT lookup: array access by vector number — O(1).
- LAPIC EOI: single MMIO write — no bus contention on fast path.
- Interrupt handler dispatch: direct function pointer call, no dynamic dispatch overhead.

## Related Documents

- [context-switching.md](context-switching.md)
- [scheduling/overview.md](../scheduling/overview.md)
- [kernel/early-boot.md](early-boot.md)

## References

- [Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A — Interrupts and Exceptions][intel-sdm-interrupts]
- [AMD64 Architecture Programmer's Manual, Volume 2 — Interrupts and Exceptions][amd-apm-interrupts]
- [Advanced Configuration and Power Interface (ACPI) Specification — APIC][acpi-apic]
- [Intel MultiProcessor Specification (MPS)][intel-mps]
- [System V Application Binary Interface AMD64 Architecture Processor Supplement — Interrupt Handling][sysv-abi-interrupts]

[intel-sdm-interrupts]: https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html#vol3a "Intel SDM Volume 3A: Interrupts and Exceptions"
[amd-apm-interrupts]: https://www.amd.com/en/developer/architecture-programmer-manuals.html "AMD64 Architecture Programmer's Manual Volume 2: Interrupts and Exceptions"
[acpi-apic]: https://uefi.org/specs/ACPI/6.5/05_ACPI_Software_Programming_Model/ACPI_Software_Programming_Model.html#advanced-programmable-interrupt-controller-apic "ACPI 6.5 - Advanced Programmable Interrupt Controller (APIC)"
[intel-mps]: https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/multiprocessor-specification.pdf "Intel MultiProcessor Specification 1.4"
[sysv-abi-interrupts]: https://gitlab.com/x86-psABIs/x86-64-ABI/-/blob/master/abi.md#interrupt-handling "System V AMD64 ABI - Interrupt Handling"
