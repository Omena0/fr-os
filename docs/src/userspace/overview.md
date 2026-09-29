# Userspace Overview

## What "Userspace" Covers

This section documents everything that runs in ring-3 as part of the OS distribution — the runtime libraries, process model, dynamic linker, C library, threading, and GUI stack.

## Components

| Component | Description | Document |
|---|---|---|
| Dynamic linker | Loads and links shared libraries on program start | [dynamic-linker.md](dynamic-linker.md) |
| C library (libc) | POSIX C runtime: stdio, malloc, string, ... | [libc.md](libc.md) |
| Thread library | POSIX threads (pthread) on top of futex/clone | [libc-threading.md](libc-threading.md) |
| ELF loader | Loading of position-independent executables | [elf-loading.md](elf-loading.md) |
| GUI architecture | Display server, window manager, widget toolkit | [gui-architecture.md](gui-architecture.md) |
| Init system | PID 1: system startup, service management | [init-system.md](init-system.md) |
| Shell | Command-line interface | [shell.md](shell.md) |
| Process model | fork/exec, process groups, sessions | [process-model.md](process-model.md) |
| Locale and I18N | Character encoding, locales | [locale.md](locale.md) |

## Libc Structure

The C library is organized as follows:
- **syscall wrappers**: Thin C wrappers around every syscall (sets `errno` on failure).
- **stdio**: `printf`, `fopen`, `fread`, buffered I/O.
- **stdlib**: `malloc` (the hardened allocator), `atoi`, sorting, etc.
- **string**: `memcpy`, `memset`, `strlen`, etc.
- **math**: `libm` (floating point math — not part of libc proper).
- **pthread**: Thread creation and synchronization (`pthread_create`, mutexes, condition variables).
- **locale**: Character classification (`isalpha`), locale-aware comparison.

## Related Documents

- [dynamic-linker.md](dynamic-linker.md)
- [libc.md](libc.md)
- [libc-threading.md](libc-threading.md)
- [elf-loading.md](elf-loading.md)
- [gui-architecture.md](gui-architecture.md)
- [init-system.md](init-system.md)
- [shell.md](shell.md)
- [process-model.md](process-model.md)
- [locale.md](locale.md)
