/*
 * errno.c — the errno storage.
 *
 * `__thread`, which is what POSIX requires and what this was before the kernel
 * had a way to make one reachable.
 *
 * The access is %fs-relative and nothing in ring 3 can establish %fs on its
 * own: a process arrives from an iretq that restored RIP, CS, RFLAGS, RSP and
 * SS, and MSR_FS_BASE is 0 until something in the kernel writes it. So the
 * variable was a plain global, and the reason was the absence of two kernel
 * features rather than anything about threads. Both exist now:
 *
 *   - elf.c maps the image's PT_TLS and records the initial thread pointer in
 *     mm->tls_ptr, which is the address one past the end of the block, which is
 *     the value a local-exec access is compiled against.
 *   - arch_prctl(ARCH_SET_FS) writes MSR_FS_BASE from ring 3, and libc's
 *     startup calls it before anything else runs. See __libc_setup_tls in
 *     crt1.c.
 *
 * There are still no threads: fork is the only concurrency primitive that
 * works, and a forked child gets its own copy of this variable along with
 * everything else. That is what makes the variable correct for this libc
 * either way -- the point of the change is that a *second thread* would get its
 * own, which a plain global cannot promise, not that one exists yet.
 */
#include <errno.h>

__thread int __errno;
