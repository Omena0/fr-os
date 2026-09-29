# Dynamic Linker

## Overview

The dynamic linker (`/lib/ld.so`) is responsible for loading shared libraries needed by an executable at startup and resolving symbol references between them.

## How It Works

When the kernel executes a dynamically linked ELF binary:
1. The kernel maps the binary's ELF segments into the address space.
2. The kernel inspects the `PT_INTERP` program header — it names the dynamic linker (e.g., `/lib/ld.so`).
3. The kernel maps the dynamic linker into the address space (at a random ASLR offset).
4. Execution begins at the dynamic linker's entry point (not the application's entry point).

The dynamic linker then:
1. Reads the application binary's `PT_DYNAMIC` segment.
2. Finds needed shared libraries (`DT_NEEDED` entries): e.g., `libc.so.1`.
3. For each library: opens the file, maps its ELF segments into the address space.
4. Resolves all symbol references:
   - GOT (Global Offset Table) entries are patched with the resolved symbol addresses.
   - PLT (Procedure Linkage Table) stubs are set up for lazy symbol resolution.
5. Runs each library's `DT_INIT_ARRAY` constructors in dependency order.
6. Jumps to the application's entry point (`e_entry` from the ELF header).

## Symbol Resolution

The dynamic linker maintains a **symbol table** (combining all loaded libraries). For each undefined symbol in each library:
1. Search the symbol tables of all loaded libraries in load order.
2. The first matching symbol is used.
3. If not found: `ld.so` aborts with "symbol not found".

Version-controlled symbols: symbols can have version suffixes (`printf@GLIBC_2.5`). The linker matches version as well as name.

## Lazy Binding (PLT)

By default, function symbols are resolved lazily — on the first call:
1. The call goes to the PLT stub.
2. The stub jumps to the GOT entry (initially pointing back to the resolver).
3. The resolver (`_dl_fixup`) resolves the symbol, patches the GOT entry with the real address.
4. All subsequent calls go directly to the real function.

This reduces startup time. Forced immediate binding: `LD_BIND_NOW=1` environment variable or `DF_BIND_NOW` flag.

## Security: RELRO

**RELRO** (Relocation Read-Only): After the dynamic linker completes relocation, it `mprotect`s the GOT to `PROT_READ`:
```c
mprotect(got_start, got_size, PROT_READ);
```

This prevents exploits from overwriting GOT entries to redirect function calls. **Full RELRO** requires all relocations to be resolved at startup (no lazy binding) so the entire GOT can be made read-only.

## Environment Variables

| Variable | Effect |
|---|---|
| `LD_PRELOAD` | Load additional libraries before all others (debugging/interposing) |
| `LD_LIBRARY_PATH` | Additional directories to search for shared libraries |
| `LD_BIND_NOW` | Resolve all symbols at startup (no lazy binding) |
| `LD_DEBUG` | Debug output (`LD_DEBUG=all` for all; `LD_DEBUG=symbols` for symbol resolution) |

Note: `LD_PRELOAD` and `LD_LIBRARY_PATH` are ignored for setuid executables (security).

## Shared Library Versions

Shared libraries use a versioning scheme:
- `libfoo.so` → symlink → `libfoo.so.1` → symlink → `libfoo.so.1.2.3`.
- ABI compatibility: the major version is incremented on ABI-breaking changes. Applications link against `libfoo.so.1`.
- The linker looks up `libfoo.so` (via `DT_NEEDED` → `SONAME` in the library's ELF header → `libfoo.so.1` → actual file).

## Related Documents

- [elf-loading.md](elf-loading.md)
- [libc.md](libc.md)
- [memory/virtual-memory.md](../memory/virtual-memory.md)
- [security/aslr.md](../security/aslr.md)
