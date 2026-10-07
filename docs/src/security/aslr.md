# ASLR — Address Space Layout Randomization

## Overview

ASLR randomizes the virtual addresses of key process memory regions at load time[^pax-aslr]. An attacker who can execute arbitrary code (e.g., via a buffer overflow that overwrites the return address) must also know the target address. ASLR makes this knowledge probabilistically unavailable[^aslr-wiki].

## Randomized Regions

| Region | Entropy | Notes |
|---|---|---|
| PIE executable text | 30 bits | `mmap` base offset from a random start |
| Heap (`brk`) | 16 bits | `brk` initial address randomized |
| Stack | 22 bits | Stack top randomized within the user stack region |
| `mmap` allocations | 40 bits | mmap base for `MAP_ANONYMOUS` and library mappings |
| VDSO | 30 bits | Mapped at a random address in each process |

Effective entropy is limited by address space alignment requirements (typically 4 KB page alignment reduces raw entropy by 12 bits, but the stated values are after accounting for this).

## Implementation

ASLR is applied by the VMM when a new process address space is initialized (`exec_load_elf`):

```c
void aslr_init_address_space(struct address_space *as) {
    as->pie_base   = aslr_rand_addr(PIE_ASLR_BITS);
    as->heap_base  = aslr_rand_addr(HEAP_ASLR_BITS);
    as->stack_top  = USER_STACK_TOP - aslr_rand_bytes(STACK_ASLR_BITS);
    as->mmap_base  = aslr_rand_addr(MMAP_ASLR_BITS);
}
```

`aslr_rand_addr()` uses the kernel's CSPRNG (seeded from RDRAND during early boot):

```c
uint64_t aslr_rand_addr(int bits) {
    uint64_t r = csprng_next64();
    return (r & ((1ULL << bits) - 1)) << PAGE_SHIFT;
}
```

## KASLR — Kernel ASLR

The kernel itself is loaded at a randomized base address within the kernel virtual address range. KASLR is applied by the bootloader (stage 2) before jumping to `kernel_main`[^linux-aslr]:

- Kernel text is placed at a random 2 MB-aligned offset within the kernel window (`0xFFFF_FFFF_8000_0000` ± random).
- The randomization offset is stored in the `BootInfo` struct for the kernel to relocate itself.
- KASLR entropy: 9 bits (512 MB window / 2 MB alignment = 256 positions = 8 bits; in practice 9 bits with finer alignment).

## Information Leaks

ASLR is only effective if the randomized addresses are not disclosed[^aslr-effectiveness]. Policies:

- `/proc/self/maps` is readable only by the process itself and root.
- No kernel addresses are printed in userspace-accessible logs (kernel pointers are printed as `[hidden]` unless `CAP_SYS_ADMIN`).
- Stack traces in crash reports show symbol names (resolved via DWARF) rather than raw addresses.

## Limitations

- ASLR does not protect against local information leaks (an attacker who can read process memory can read the address of anything)[^aslr-effectiveness].
- Brute-force attacks are possible if the process is repeatedly restartable — mitigated by process restart rate limiting in the init system.
- 32-bit processes (compat mode) have lower ASLR entropy.

## Related Documents

- [stack-canaries.md](stack-canaries.md)
- [nx-enforcement.md](nx-enforcement.md)
- [hardened-allocator.md](hardened-allocator.md)
- [memory/virtual-memory.md](../memory/virtual-memory.md)

## References

- [PaX ASLR Documentation][pax-aslr]
- [Linux Kernel ASLR Implementation][linux-aslr]
- [Address Space Layout Randomization — Wikipedia][aslr-wiki]
- [Effectiveness of ASLR on 64-bit Linux][aslr-effectiveness]

[pax-aslr]: https://pax.grsecurity.net/docs/aslr.txt "PaX ASLR Design and Implementation"
[linux-aslr]: https://www.kernel.org/doc/html/latest/admin-guide/aslr.html "Linux Kernel ASLR Documentation"
[aslr-wiki]: https://en.wikipedia.org/wiki/Address_space_layout_randomization "Address Space Layout Randomization - Wikipedia"
[aslr-effectiveness]: https://www.usenix.org/conference/usenixsecurity17/technical-sessions/presentation/bittau "ASLR on the Line: Practical Cache Attacks on the MMU"
