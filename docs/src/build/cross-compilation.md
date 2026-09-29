# Cross-Compilation

## Overview

Cross-compilation means building code on one machine (the **host**, typically an x86-64 Linux desktop) for a different target environment (the **target**: bare-metal x86-64 with no OS). The cross-compiler produces ELF binaries that will run in the target environment.

## Sysroot

A **sysroot** is a directory that acts as the root filesystem for the target. Cross-compiled libraries (libc, libpthread, etc.) are installed here, and the compiler is configured to find headers and libraries in the sysroot rather than the host's `/usr/`.

```
sysroot/
  usr/
    include/          — kernel and libc headers
      stdio.h
      stdlib.h
      string.h
      pthread.h
      sys/
        mman.h
        socket.h
      ...
    lib/              — cross-compiled libraries
      libc.so.1
      libpthread.so.1
      ld.so
    bin/              — userspace utilities
      sh
      ls
      ...
```

## Cross-Compiler Configuration

The cross-compiler (built without `--with-sysroot`) uses bare-metal mode:
```sh
x86_64-elf-gcc -ffreestanding -nostdlib ...
```

For userspace programs that link against libc:
```sh
x86_64-elf-gcc \
    --sysroot=sysroot \
    -o build/userspace/sh sysroot/usr/bin/sh.o \
    -L sysroot/usr/lib \
    -lc
```

## Header Organization

Kernel headers exposed to userspace (for libc to use):
```
src/kernel/include/uapi/   — user-visible kernel API headers
  sys/syscall.h             — syscall numbers
  sys/mman.h                — mmap flags
  sys/socket.h              — socket structures
  sys/stat.h                — struct stat
  linux/futex.h             — futex constants
```

These headers contain only data structure definitions and constants — no kernel-internal types.

## Building Userspace Programs

Userspace programs are compiled for the target with:
```sh
x86_64-elf-gcc \
    --sysroot=sysroot \
    -O2 -g \
    -fpie \          # position-independent executable (for ASLR)
    -o shell shell.c \
    -lc
```

The `--sysroot` flag tells the compiler to look for headers in `sysroot/usr/include` and libraries in `sysroot/usr/lib`.

## initrd Construction

The initial RAM disk (`initrd.tar`) is a tar archive containing the initial userspace:
```
/bin/sh
/lib/libc.so.1
/lib/ld.so
/lib/libpthread.so.1
/etc/init/
  network.unit
  sshd.unit
/sbin/init
```

The kernel extracts the initrd at boot time and mounts it as a temporary root filesystem before mounting the real root filesystem.

## Makefile Integration

```makefile
# Install a userspace binary into the sysroot:
$(SYSROOT)/usr/bin/%: $(BUILDDIR)/userspace/%
	install -Dm755 $< $@

# Build the initrd:
$(BUILDDIR)/initrd.tar: $(SYSROOT)
	tar -C $(SYSROOT) -cf $@ .
```

## Related Documents

- [overview.md](overview.md)
- [toolchain.md](toolchain.md)
- [makefile-structure.md](makefile-structure.md)
- [userspace/libc.md](../userspace/libc.md)
- [userspace/dynamic-linker.md](../userspace/dynamic-linker.md)
