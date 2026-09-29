/*
 * spinlock.h — ticket spinlocks.
 *
 * A ticket lock rather than a test-and-set: with many CPUs contending on one
 * lock (a global run queue, the console), a TAS lock lets every waiter hammer
 * the same cache line while only one progresses. A ticket lock hands out turn
 * order, so each waiter spins on a line that is not being written, and
 * hand-off is fair by construction.
 *
 * The lock must not be held across a context switch or a blocking operation.
 * spinlock_irqsave() disables interrupts for the duration, which is what makes
 * it safe to hold one across code that might be interrupted by a handler that
 * also takes the same lock.
 */
#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdbool.h>

#include <io.h>
#include <types.h>

typedef struct spinlock {
	volatile u32 next;    /* next ticket to be handed out */
	volatile u32 owner;   /* ticket currently being served */
} spinlock_t;

#define SPINLOCK_INIT { 0, 0 }

static inline void spinlock_init(spinlock_t *lock)
{
	lock->next = 0;
	lock->owner = 0;
	cpu_barrier();
}

/*
 * PAUSE in the spin loop is not decoration. Without it the core issues the load
 * at full rate, saturating the coherence fabric that the lock holder needs in
 * order to make progress. With it, the core yields pipeline slots and backs off
 * SMT siblings.
 */
static inline void spinlock_acquire(spinlock_t *lock)
{
	u32 ticket = __atomic_fetch_add(&lock->next, 1, __ATOMIC_RELAXED);

	while (__atomic_load_n(&lock->owner, __ATOMIC_ACQUIRE) != ticket) {
		pause_cpu();
	}
}

static inline void spinlock_release(spinlock_t *lock)
{
	/* A single store publishes the next ticket. The release ordering
	 * guarantees every critical-section write above is visible to the next
	 * holder before it can observe ownership. */
	__atomic_store_n(&lock->owner, lock->owner + 1, __ATOMIC_RELEASE);
}

static inline bool spinlock_trylock(spinlock_t *lock)
{
	u32 owner = __atomic_load_n(&lock->owner, __ATOMIC_RELAXED);

	if (owner != __atomic_load_n(&lock->next, __ATOMIC_RELAXED))
		return false;
	return __atomic_compare_exchange_n(&lock->owner, &owner, owner + 1,
					  false, __ATOMIC_ACQUIRE,
					  __ATOMIC_RELAXED);
}

static inline bool spinlock_is_held(spinlock_t *lock)
{
	return lock->owner != lock->next;
}

/*
 * Acquire with interrupts disabled. Returns the saved flags so the caller can
 * restore the previous interrupt state rather than unconditionally enabling
 * them, which would be wrong for a caller that was already running with
 * interrupts off.
 */
static inline u64 spinlock_irqsave(spinlock_t *lock)
{
	u64 flags = irq_save();

	spinlock_acquire(lock);
	return flags;
}

static inline void spinlock_unlock_irqrestore(spinlock_t *lock, u64 flags)
{
	spinlock_release(lock);
	irq_restore(flags);
}

/*
 * Read/write lock for the few places where a reader must not block a writer for
 * longer than the read itself. Used by the VFS dentry cache, where reads
 * dominate by orders of magnitude.
 */
typedef struct rwlock {
	volatile s32 readers;   /* >= 0: unlocked; < 0: writer holds it */
} rwlock_t;

#define RWLOCK_INIT { 0 }

static inline void rwlock_init(rwlock_t *l)
{
	l->readers = 0;
}

static inline void rwlock_read_lock(rwlock_t *l)
{
	for (;;) {
		s32 v = __atomic_load_n(&l->readers, __ATOMIC_ACQUIRE);

		if (v >= 0 && __atomic_compare_exchange_n(&l->readers, &v, v + 1,
							  true, __ATOMIC_ACQUIRE,
							  __ATOMIC_RELAXED))
			return;
		pause_cpu();
	}
}

static inline void rwlock_read_unlock(rwlock_t *l)
{
	__atomic_fetch_sub(&l->readers, 1, __ATOMIC_RELEASE);
}

static inline void rwlock_write_lock(rwlock_t *l)
{
	for (;;) {
		s32 v = __atomic_load_n(&l->readers, __ATOMIC_ACQUIRE);

		if (v == 0 && __atomic_compare_exchange_n(&l->readers, &v, -1,
							  false, __ATOMIC_ACQUIRE,
							  __ATOMIC_RELAXED))
			return;
		pause_cpu();
	}
}

static inline void rwlock_write_unlock(rwlock_t *l)
{
	__atomic_store_n(&l->readers, 0, __ATOMIC_RELEASE);
}

#endif /* SPINLOCK_H */
