/*
 * malloc_fork.c — the allocator's post-fork child hook.
 *
 * fork() calls this in the child before anything else touches the heap. The
 * child gets its own address space but inherits the parent's arenas
 * byte-for-byte, including any free chunks sitting in the parent's tcache, so
 * without a reset a malloc in the child can hand out a block the parent also
 * believes it owns.
 *
 * The definition here is the weak one. The allocator's real implementation
 * lives with the allocator, which owns the arena list and the thread cache and
 * is the only code that can legally reset them; a strong definition in
 * malloc.c displaces this at link time and the weak symbol costs nothing once
 * it is shadowed. Keeping the fallback out of malloc.c is deliberate: this
 * file can be rewritten without a lock on the allocator.
 */
#include <stddef.h>

__attribute__((weak)) void __malloc_fork_child(void)
{
    /*
     * No-op fallback. The inherited arenas are left mapped rather than
     * unmapped: reaping them here would need the arena list, and a child
     * that frees a block the parent is still using is already the bug this
     * hook exists to prevent, not one that more aggressive teardown could
     * paper over.
     */
}
