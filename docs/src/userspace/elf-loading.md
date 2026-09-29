# ELF Loading

## Overview

The kernel's ELF loader is responsible for mapping executable files into a new process address space during `sys_exec`. It handles both statically linked executables (`ET_EXEC`) and position-independent executables (`ET_DYN`).

## ELF Header Validation

```c
// ELF64 header (first 64 bytes of the file)
typedef struct {
    uint8_t  e_ident[16]; // magic, class, endianness, version
    uint16_t e_type;      // ET_EXEC, ET_DYN
    uint16_t e_machine;   // EM_X86_64
    uint32_t e_version;   // 1
    uint64_t e_entry;     // entry point virtual address
    uint64_t e_phoff;     // offset of program header table
    uint64_t e_shoff;     // offset of section header table
    uint32_t e_flags;     // processor-specific flags
    uint16_t e_ehsize;    // size of this header (64)
    uint16_t e_phentsize; // size of one program header entry (56)
    uint16_t e_phnum;     // number of program headers
    uint16_t e_shentsize; // size of one section header entry
    uint16_t e_shnum;     // number of section headers
    uint16_t e_shstrndx;  // section name string table index
} Elf64_Ehdr;
```

The loader checks:
- `e_ident[0..3]` == `{0x7f, 'E', 'L', 'F'}` (magic number).
- `e_ident[4]` == `ELFCLASS64` (2).
- `e_ident[5]` == `ELFDATA2LSB` (1, little-endian).
- `e_machine` == `EM_X86_64` (62).
- `e_type` is `ET_EXEC` or `ET_DYN`.

## Program Headers

The loader processes only `PT_LOAD` and `PT_INTERP` segments:

```c
typedef struct {
    uint32_t p_type;    // PT_LOAD, PT_DYNAMIC, PT_INTERP, ...
    uint32_t p_flags;   // PF_R, PF_W, PF_X
    uint64_t p_offset;  // offset in file
    uint64_t p_vaddr;   // virtual address to map to
    uint64_t p_paddr;   // physical address (ignored)
    uint64_t p_filesz;  // number of bytes in the file
    uint64_t p_memsz;   // number of bytes in memory (>= filesz, BSS = memsz - filesz)
    uint64_t p_align;   // alignment (must be power of 2)
} Elf64_Phdr;
```

### `PT_LOAD` Processing

For each `PT_LOAD` segment:
1. Compute the ASLR load base (for `ET_DYN`): `base = aslr_base_for_executable()`.
2. `map_address = base + p_vaddr`, rounded down to page boundary.
3. `map_length` = round up `p_memsz` to page size.
4. `mmap(map_address, map_length, prot, MAP_FIXED | MAP_PRIVATE, exe_fd, file_offset)`.
5. Zero the BSS region: `memset(segment_end, 0, p_memsz - p_filesz)`.
6. Protection: `PF_R` → `PROT_READ`; `PF_W` → `PROT_WRITE`; `PF_X` → `PROT_EXEC`.

### `PT_INTERP` Processing

If a `PT_INTERP` segment is present:
1. Read the interpreter path (e.g., `/lib/ld.so`).
2. Load the dynamic linker (same ELF loading process, recursively).
3. Execution starts at the dynamic linker's entry point (not the application's).

## Argument Vector Setup

After loading, the kernel sets up the initial stack:

```
RSP (top of stack)
  argc         (uint64_t)
  argv[0]      (ptr to string)
  ...
  argv[argc-1]
  NULL
  envp[0]      (ptr to string)
  ...
  NULL
  auxv[0]      (AT_PHDR, AT_PHNUM, AT_ENTRY, AT_RANDOM, ...)
  ...
  AT_NULL
  --- strings ---
  argument strings
  environment strings
```

## Auxiliary Vector (`auxv`)

The kernel passes information to the dynamic linker and libc via the auxiliary vector:

| `AT_` type | Value |
|---|---|
| `AT_PHDR` | Address of ELF program headers |
| `AT_PHNUM` | Number of program headers |
| `AT_ENTRY` | Application entry point |
| `AT_RANDOM` | Address of 16 random bytes (for stack canaries) |
| `AT_PAGESZ` | System page size (4096) |
| `AT_BASE` | Load base of dynamic linker |
| `AT_SYSINFO_EHDR` | Address of VDSO ELF header |

## Related Documents

- [dynamic-linker.md](dynamic-linker.md)
- [libc.md](libc.md)
- [memory/virtual-memory.md](../memory/virtual-memory.md)
- [security/aslr.md](../security/aslr.md)
- [syscalls/process-syscalls.md](../syscalls/process-syscalls.md)
