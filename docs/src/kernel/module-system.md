# Kernel Module System

## Overview

The kernel module system allows kernel functionality to be compiled as separate ELF shared objects and loaded/unloaded at runtime without rebooting. Modules have access to all exported kernel symbols and can register new drivers, filesystems, network protocols, and other kernel subsystems.

## Module Format

Kernel modules are ELF64 shared objects (`.ko` files, by convention). They contain:

- **Code and data sections**: Standard `.text`, `.data`, `.rodata`, `.bss`.
- **`__ksymtab` section**: Table of symbols the module exports to other modules.
- **`__kcrctab` section**: CRC checksums for each exported symbol (for versioned symbol validation).
- **`__modinfo` section**: Module metadata (name, version, author, description, dependencies, license).
- **`init_module` symbol**: Called on module load.
- **`cleanup_module` symbol**: Called on module unload (optional; if absent, module is non-unloadable).

## Symbol Versioning

Exported kernel symbols are versioned using CRC-32 checksums derived from the symbol name and the full type signature. A module may only link against a symbol if the CRC in the module's `__kcrctab` matches the CRC in the kernel's exported symbol table.

If a CRC mismatch is detected at load time, the load is rejected with `ENOSYS`. This prevents loading a module compiled against a different kernel API version.

The kernel's exported symbol table is generated at build time from `EXPORT_SYMBOL(name)` and `EXPORT_SYMBOL_GPL(name)` annotations.

## Dependency Resolution

Modules declare dependencies by name in their `__modinfo` section. The module loader:

1. Reads the dependency list from the module being loaded.
2. Recursively loads any unloaded dependencies first.
3. Resolves all symbol references against the combined kernel + loaded module symbol tables.
4. Performs ELF relocations.
5. Calls `init_module()`.

Circular dependencies are detected and rejected.

## Load/Unload Lifecycle

### Loading

```
sys_finit_module(fd, params) or sys_init_module(buf, len, params)
  → module_load()
    → ELF validation
    → dependency resolution
    → symbol binding + relocation
    → memory allocation (module text in kernel vmalloc space)
    → call init_module()
    → register module in module list
    → module state: LIVE
```

### Unloading

```
sys_delete_module(name, flags)
  → check reference count (other modules depending on this one)
  → call cleanup_module()
  → unregister from module list
  → free module memory
```

Unloading fails with `EBUSY` if any other module or kernel subsystem holds a reference to this module's symbols.

## Module Memory

Module code and data live in a dedicated region of the kernel virtual address space (module space, `0xFFFF_C800_0000_0000`). This region is:

- Executable (`X` bit set for `.text` pages).
- Not accessible from userspace (`U=0`).
- Backed by physical pages allocated at load time.

## Security

- Only processes with `CAP_SYS_MODULE` can load or unload modules.
- Module signing is planned for a future release (reject unsigned modules by policy).
- Modules run in ring 0 with full kernel access. A buggy module can corrupt the kernel — this is unavoidable for ring-0 code. Userspace drivers (ring 3) are preferred for hardware-specific functionality.

## Related Documents

- [kernel/overview.md](overview.md)
- [drivers/kernel-drivers.md](../drivers/kernel-drivers.md)
- [debugging/observability-api.md](../debugging/observability-api.md)
