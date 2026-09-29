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

/* The name the kernel knows itself by in panic output and the version
 * syscall. Deliberately a macro so a symbol table search finds every use. */
#define KERNEL_VERSION_STRING KERNEL_VERSION

#endif /* VERSION_H */
