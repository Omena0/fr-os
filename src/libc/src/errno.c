/*
 * errno.c — the thread-local errno storage.
 *
 * __thread is a GCC keyword that emits a TLS access, and on this freestanding
 * target the kernel's ELF loader sets up the thread pointer at clone time.
 * There is no dynamic TLS model to worry about: every thread gets its own
 * copy initialised to 0, so a fresh thread never inherits a stale errno.
 */
#include <errno.h>

__thread int __errno;