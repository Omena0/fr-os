# Performance Counters

## Overview

Modern x86-64 processors have a **Performance Monitoring Unit (PMU)** — a set of hardware registers that count low-level microarchitectural events: retired instructions, cache misses, branch mispredictions, TLB misses, etc.

## Hardware Counter Registers

On x86-64 (Intel/AMD), the PMU provides:
- **Fixed counters** (3): always-on, count specific events:
  - `IA32_FIXED_CTR0`: instructions retired.
  - `IA32_FIXED_CTR1`: core CPU cycles unhalted.
  - `IA32_FIXED_CTR2`: reference cycles unhalted (at TSC frequency).
- **Programmable counters** (4–8, depends on CPU model): configurable to count any supported event via `IA32_PERFEVTSELx` registers.

## Common Programmable Events

| Event code | Mask | Description |
|---|---|---|
| `0x3C` | `0x00` | Unhalted core cycles |
| `0xC0` | `0x00` | Instructions retired |
| `0x2E` | `0x4F` | Last-level cache misses |
| `0xC5` | `0x00` | Branch mispredictions |
| `0x08` | `0x01` | DTLB load misses |
| `0x49` | `0x01` | DTLB store misses |

## Programming Counters

```c
// Set up programmable counter 0 to count LLC misses:
uint64_t evtsel = 0;
evtsel |= 0x2E;        // EventSelect = 0x2E (LLC)
evtsel |= (0x4F << 8); // UMask = 0x4F
evtsel |= (1 << 16);   // USR: count in ring 3
evtsel |= (1 << 17);   // OS: count in ring 0
evtsel |= (1 << 22);   // EN: enable the counter
wrmsr(IA32_PERFEVTSEL0, evtsel);

// Zero the counter:
wrmsr(IA32_PMC0, 0);

// Read the counter:
uint64_t count = rdpmc(0);  // RDPMC instruction (ring 3 accessible if CR4.PCE=1)
```

## Userspace Access via RDPMC

The kernel can set `CR4.PCE` (Performance Counter Enable) to allow userspace to read PMU counters directly with the `RDPMC` instruction — without a syscall. This is used by profiling tools for very low overhead sampling.

Per-process PMU context is saved/restored on context switch (via `XSAVE` extended state if supported, or explicit `RDMSR`/`WRMSR`).

## Overflow Interrupts

Counters can generate an interrupt when they overflow (count rolls over from `0xFFFFFFFF` to 0). This is used by **sampling profilers**:
1. Set the counter to `(2^32 - N)` (will overflow after N events).
2. On overflow: a `#PMI` (Performance Monitoring Interrupt) fires.
3. The interrupt handler records the current `RIP` (instruction pointer) and other state.
4. Resets the counter and resumes.

This produces a statistical sample of where the CPU spends its time.

## Userspace API

The kernel exposes PMU access via:
1. `/debug/pmu/<cpu>/fixed0` etc.: virtual files to read current counter values.
2. `sys_perf_event_open`-equivalent syscall: programmatic counter configuration and event streaming.

## Related Documents

- [overview.md](overview.md)
- [profiling.md](profiling.md)
- [kernel-tracing.md](kernel-tracing.md)
- [observability-api.md](observability-api.md)
