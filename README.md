
# Fr OS

[![Docs](https://github.com/Omena0/fr-os/actions/workflows/pages.yml/badge.svg)](https://github.com/Omena0/fr-os/actions/workflows/pages.yml)

A x86-64 operating system for QEMU: a bootloader, a kernel, a C runtime and
a userspace, written from scratch in C11 and x86-64 assembly.

**Fr OS** is the project. It is assembled from six named components, and the names
are macros in [`src/include/version.h`](src/include/version.h) so that no two
banners can ever disagree:

| Component | What it is |
|---|---|
| **Fr OS** | The project as a whole |
| **Fr Core** | The kernel |
| **Fr Boot** | The bootloader — stage 1 in the MBR, stage 2 into long mode — which hands control to Fr Core |
| **Fr Init** | The first userspace process, and system bring-up |
| **Fr Libc** | The C runtime shared by the kernel and Fr Userland |
| **Fr Userland** | The programs that run on top of Fr Init |

Fr OS and Fr Core are not the same thing, and the distinction is load-bearing. A
panic that says "Fr OS" is claiming the entire system is down when in fact one
component failed; a panic that says "Fr Core" is claiming the kernel failed while
the bootloader and the firmware are still fine. The kernel's banner and its panic
path therefore print **Fr Core**, not Fr OS. `KERNEL_VERSION_STRING` is
`FR_CORE_NAME " " KERNEL_VERSION`.
