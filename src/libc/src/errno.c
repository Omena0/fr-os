/*
 * errno.c — the errno storage.
 *
 * A plain global, not a __thread. __thread compiles to an %fs-relative access
 * and nothing on this target establishes an FS base: the kernel's ELF loader
 * ignores PT_TLS, and the syscall ABI has no arch_prctl and no way to write
 * MSR_FS_BASE from ring 3. So %fs is 0 in every process and the first
 * `__errno = ...` in the first failing syscall would fault at address 0.
 *
 * There are no threads either -- fork is the only concurrency primitive that
 * works today, and a forked child gets its own copy of this global along with
 * everything else. Single-threaded is the honest description of what this libc
 * runs on; making the variable __thread again is a one-word change once the
 * kernel sets FS, and nothing else in the tree has to move.
 */
#include <errno.h>

int __errno;
