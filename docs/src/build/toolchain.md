# Toolchain Setup

## Required Tools

| Tool | Minimum Version | Purpose |
|---|---|---|
| `nasm` | 2.15 | Stage-1 bootloader assembly |
| `i686-elf-gcc` | 12.0 | Stage-2 bootloader (16-bit/32-bit C) |
| `x86_64-elf-gcc` | 12.0 | Kernel and userspace C |
| `x86_64-elf-ld` | 2.38 | Kernel and userspace linking |
| `x86_64-elf-objcopy` | 2.38 | Strip symbols from kernel binary |
| `qemu-system-x86_64` | 6.0 | OS emulation |
| `make` | 4.3 | Build orchestration |

## Building Cross-Compilers

The `i686-elf` and `x86_64-elf` cross-compilers target bare metal (no host OS libraries). Build them from source using the standard GNU toolchain build process:

### Step 1: Download Sources

```sh
export BINUTILS_VER=2.41
export GCC_VER=13.2.0
wget https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VER.tar.xz
wget https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VER/gcc-$GCC_VER.tar.xz
tar -xf binutils-$BINUTILS_VER.tar.xz
tar -xf gcc-$GCC_VER.tar.xz
```

### Step 2: Build Binutils

```sh
mkdir build-binutils && cd build-binutils
../binutils-$BINUTILS_VER/configure \
    --target=x86_64-elf \
    --prefix=/opt/cross \
    --disable-nls \
    --disable-werror
make -j$(nproc)
make install
```

Repeat for `i686-elf` by changing `--target`.

### Step 3: Build GCC (cross-compiler only)

```sh
mkdir build-gcc && cd build-gcc
../gcc-$GCC_VER/configure \
    --target=x86_64-elf \
    --prefix=/opt/cross \
    --disable-nls \
    --enable-languages=c \
    --without-headers
make all-gcc all-target-libgcc -j$(nproc)
make install-gcc install-target-libgcc
```

### Step 4: Add to PATH

```sh
export PATH="/opt/cross/bin:$PATH"
```

Add to `~/.bashrc` or `~/.zshrc` for persistence.

## Verifying the Toolchain

```sh
x86_64-elf-gcc --version   # Should print GCC 13.x.0
x86_64-elf-ld --version    # Should print GNU ld 2.41
nasm --version              # Should print NASM version 2.15.x
qemu-system-x86_64 --version  # Should print QEMU emulator version 6.x
```

## Optional Tools

| Tool | Purpose |
|---|---|
| `xorriso` | Build bootable ISO images |
| `gdb` (with `x86_64-elf` support) | Kernel debugging via QEMU's GDB stub |
| `objdump` (from cross binutils) | Disassemble kernel binary |
| `readelf` | Inspect ELF headers |

## Related Documents

- [overview.md](overview.md)
- [cross-compilation.md](cross-compilation.md)
- [makefile-structure.md](makefile-structure.md)
