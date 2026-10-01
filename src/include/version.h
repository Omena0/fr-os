/*
 * version.h — build provenance.
 *
 * Kept as a header rather than a generated file so that a build from a bare
 * checkout works with no build-system state. The Makefile overrides
 * KERNEL_BUILD_STAMP with a reproducible value when it can (git describe plus
 * the commit time), falling back to __DATE__/__TIME__ when it cannot, which is
 * why the fallback is spelled out here instead of being a build-time error: an
 * unversioned kernel that boots is better than a version stamp that blocks one.
 */
#ifndef VERSION_H
#define VERSION_H

#ifndef KERNEL_VERSION
#define KERNEL_VERSION "0.1.0"
#endif

#ifndef KERNEL_BUILD_STAMP
#define KERNEL_BUILD_STAMP __DATE__ " " __TIME__
#endif

#ifndef KERNEL_GIT_REV
#define KERNEL_GIT_REV "unknown"
#endif

/*
 * Branding.
 *
 * Fr OS is the project. It is assembled from six components, each named here
 * once so that a banner, a panic, a sysconf or an init message can never drift
 * from the others:
 *
 *   Fr OS         the project as a whole
 *   Fr Core       the kernel
 *   Fr Boot       the bootloader, which hands control to Fr Core
 *   Fr Init       the first userspace process and the system bring-up
 *   Fr Libc       the C runtime the kernel and Fr Userland share
 *   Fr Userland   the programs that run on top of Fr Init
 *
 * The kernel's own name is FR_CORE_NAME: the thing that prints "Fr Core" is
 * Fr Core, and the thing that prints "Fr OS" is the project, not a component.
 * Keeping them separate is the point -- a kernel panic saying "Fr OS" would
 * claim the whole system on fire when only one component is.
 */
#define FR_PROJECT_NAME  "Fr OS"
#define FR_CORE_NAME     "Fr Core"
#define FR_BOOT_NAME     "Fr Boot"
#define FR_INIT_NAME     "Fr Init"
#define FR_LIBC_NAME     "Fr Libc"
#define FR_USERLAND_NAME "Fr Userland"

/* The name the kernel knows itself by in panic output and the version
 * syscall. Deliberately a macro so a symbol table search finds every use. */
#define KERNEL_VERSION_STRING FR_CORE_NAME " " KERNEL_VERSION

#endif /* VERSION_H */
