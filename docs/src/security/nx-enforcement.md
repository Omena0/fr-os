# NX / Execute-Disable Enforcement

## Overview

NX (No-eXecute) enforcement prevents data pages from being executed as code. This is the primary mitigation against classic shellcode injection attacks: even if an attacker writes malicious code into a buffer, they cannot execute it because the page is marked non-executable.

## Hardware Mechanism

On x86-64, the NX bit is bit 63 of a page table entry (PTE). When set, the CPU raises a `#PF` (page fault) with error code bit 4 set (indicating an instruction fetch violation) if code attempts to execute from that page.

The NX bit requires the `NXE` bit to be set in the `EFER` MSR:

```c
void enable_nx(void) {
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_NXE;
    wrmsr(MSR_EFER, efer);
}
```

This is done in early kernel initialization (immediately after entering long mode).

## Page Table Policy

| Region | NX bit | Rationale |
|---|---|---|
| Kernel text (`.text`) | Clear (executable) | Code must execute |
| Kernel data/BSS | Set (NX) | Data should not execute |
| Kernel heap (kmalloc) | Set (NX) | Heap not executable |
| User code segment | Clear | User code executes |
| User stack | Set | Stack not executable by default |
| User heap (mmap anon) | Set | Heap not executable by default |
| User `mmap(PROT_EXEC)` | Clear | Explicitly requested execute |
| User shared libraries | Clear | Library code must execute |
| VDSO | Clear | Kernel-provided code |

## `mmap` with `PROT_EXEC`

Userspace can create executable mappings explicitly:

```c
void *code = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
```

Note: Simultaneous `PROT_WRITE | PROT_EXEC` (W^X violation) is allowed for JIT compilers but logged as a security event. A strict mode (future feature) will prohibit `W+X` simultaneously and require the JIT to `mprotect` the region to remove write permissions before executing.

## SMEP (Supervisor Mode Execution Prevention)

SMEP prevents the kernel from executing code from user pages. This is enabled via `CR4.SMEP`:

```c
void enable_smep(void) {
    uint64_t cr4 = read_cr4();
    cr4 |= CR4_SMEP;
    write_cr4(cr4);
}
```

With SMEP, an attacker who gains kernel control (e.g., via a kernel vulnerability) cannot redirect execution to shellcode placed in a user page (ret2usr attacks).

## Page Fault Handling for NX Violations

When an NX violation occurs (instruction fetch from a non-executable page):

- The CPU delivers a `#PF` exception with error code bit 4 set.
- The page fault handler checks bit 4: this is an NX violation, not a missing page.
- **User mode NX violation**: deliver `SIGSEGV` to the process (si_code = `SEGV_ACCERR`).
- **Kernel mode NX violation**: `kpanic("kernel NX violation at 0x...")`.

## Related Documents

- [aslr.md](aslr.md)
- [stack-canaries.md](stack-canaries.md)
- [hardened-allocator.md](hardened-allocator.md)
- [memory/virtual-memory.md](../memory/virtual-memory.md)
- [kernel/privilege-levels.md](../kernel/privilege-levels.md)
