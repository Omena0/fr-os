/*
 * malloc.c — segregated-list allocator with a size-class cache and mmap-backed arenas.
 *
 * Design:
 *   - Size classes: 16-byte granularity up to 128 KiB (8192 classes). Small
 *     allocations pop from a per-thread free list in O(1) with no search.
 *   - Cache (tcache): a batch of free chunks per size class, held in one mmap'd
 *     block. Refill/flush happens in bulk from the arena, mirroring the
 *     kernel's per-CPU magazine design.
 *   - Arenas: 1 MiB regions from mmap (PROT_READ|PROT_WRITE, MAP_PRIVATE|ANON).
 *     A bump pointer carves fresh chunks; a free list reuses holes.
 *   - Large allocations (> 128 KiB): dedicated mmap, freed with munmap.
 *     Detected by class_idx == NCLASS.
 *   - One header for both sizes. A large block carries a full `struct chunk`
 *     with class_idx == NCLASS rather than a smaller private prefix, so that
 *     free(), realloc() and malloc_usable_size() can all reach a block's
 *     metadata with the single `ptr - sizeof(struct chunk)` computation and
 *     never touch a byte below the mmap base.
 *   - realloc shrinks in place when the remainder is at least two alignment
 *     units and allocates-and-copies otherwise. It does *not* grow in place:
 *     there is no adjacency tracking between chunks, so a neighbouring free
 *     block is invisible here.
 *   - aligned_alloc/posix_memalign over-allocate and store the real pointer
 *     immediately before the aligned address.
 *   - No recursion: refill failure returns NULL, never panics.
 *   - Corruption detection: a magic field, validated on free/realloc, and
 *     cleared when the chunk goes back on a free list, so a double free is
 *     caught on the second free rather than handing the same block out twice.
 *   - Thread safety: global spinlock on the arena (futex hook marked for SMP).
 */
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <assert.h>

/* Internal syscall wrappers (defined in syscall.c) */
extern void *sys_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
extern int sys_munmap(void *addr, size_t length);
extern long sys_brk(void *addr);


/* -------------------------------- constants -------------------------------- */

#define ALIGNMENT       16
#define MAX_SMALL_SIZE  (128 * 1024)      /* 128 KiB */
#define NCLASS          (MAX_SMALL_SIZE / ALIGNMENT)  /* 8192 */
#define ARENA_SIZE      (1024 * 1024)     /* 1 MiB */
#define CHUNK_MAGIC     0xDEADBEEFDEADBEEFUL
#define TCACHE_BATCH    32                /* refill/flush batch size */
#define MIN_CHUNK_SIZE  (sizeof(struct chunk))

/* ------------------------------- chunk header ------------------------------ */

struct chunk {
	uint64_t magic;       /* CHUNK_MAGIC when valid */
	size_t   size;        /* user-visible size (aligned) */
	size_t   class_idx;   /* size class index, or NCLASS for large */
	struct chunk *next;   /* free-list link */
	/* user payload follows immediately */
};

/* Large allocations carry the same header as small ones, with class_idx set to
 * NCLASS. One header shape is what lets free() and realloc() validate any
 * pointer with the same arithmetic instead of special-casing a second prefix
 * whose size differs from the first -- which is exactly how the previous 16-byte
 * large prefix put `ptr - sizeof(struct chunk)` 16 bytes *below* the mmap base. */

/* -------------------------------- size class cache --------------------------- */

struct tcache {
	struct chunk *freelist[NCLASS];
	uint16_t      count[NCLASS];
	uint64_t      init_magic;
};

/*
 * Not `__thread`. A __thread variable compiles to an %fs-relative access, and
 * nothing on this target establishes an FS base: the kernel's ELF loader has no
 * PT_TLS handling and no syscall sets FS (there is no arch_prctl in the ABI), so
 * %fs is 0 for every process and the very first instruction of the allocator
 * would fault. The whole allocator is therefore process-global. When the kernel
 * grows PT_TLS + a set-FS syscall, this becomes __thread again with no other
 * change: the field is already only ever touched through this pointer.
 */
static struct tcache *tcache = NULL;

/* -------------------------------- arena ------------------------------------ */

struct arena {
	struct chunk *free_list[NCLASS];
	void *base;               /* start of arena region */
	void *bump;               /* current bump pointer */
	void *limit;              /* end of arena region */
	unsigned long lock;       /* spinlock: 0 = free, 1 = held */
	struct arena *next;       /* next arena in list */
};

static struct arena *arena_list = NULL;
static unsigned long arena_lock = 0;  /* protects arena_list and new arena creation */

/* -------------------------------- helpers ---------------------------------- */

static inline void spin_lock(unsigned long *lock)
{
	while (__atomic_test_and_set(lock, __ATOMIC_ACQUIRE))
		__builtin_ia32_pause();
}

static inline void spin_unlock(unsigned long *lock)
{
	__atomic_clear(lock, __ATOMIC_RELEASE);
}

/* Futex hook for future SMP: replace spin_lock/spin_unlock with
 * futex_wait/futex_wake on the lock word when threads exist. */

static inline size_t align_up(size_t n, size_t a)
{
	return (n + a - 1) & ~(a - 1);
}

/*
 * align_up for a caller that must be told when the answer is meaningless.
 * `n + a - 1` wraps for n within a-1 of SIZE_MAX and returns 0, which
 * size_to_class() then turns into class index SIZE_MAX -- an index that a
 * caller then uses to subscript an array. Every public entry point rounds a
 * caller-supplied size through this and rejects the overflow instead.
 */
static inline int align_up_checked(size_t n, size_t a, size_t *out)
{
	if (a == 0 || n > SIZE_MAX - (a - 1))
		return -1;
	*out = (n + a - 1) & ~(a - 1);
	return 0;
}

static inline size_t size_to_class(size_t size)
{
	return (size + ALIGNMENT - 1) / ALIGNMENT - 1;
}

static inline size_t class_to_size(size_t class_idx)
{
	return (class_idx + 1) * ALIGNMENT;
}

static inline struct chunk *ptr_to_chunk(void *ptr)
{
	return (struct chunk *)((char *)ptr - sizeof(struct chunk));
}

static inline void *chunk_to_ptr(struct chunk *ch)
{
	return (void *)((char *)ch + sizeof(struct chunk));
}

static inline int chunk_is_valid(struct chunk *ch)
{
	return ch->magic == CHUNK_MAGIC;
}

static inline int is_large_class(size_t class_idx)
{
	return class_idx >= NCLASS;
}

#define TCACHE_INIT_MAGIC 0xFEEDFACEDEADBEEFUL


static void tcache_init(void)
{
	if (tcache && tcache->init_magic == TCACHE_INIT_MAGIC) {
		return;
	}
	tcache = (struct tcache *)sys_mmap(NULL, sizeof(struct tcache),
		PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (tcache == MAP_FAILED) {
		/*
		 * No cache, not a private one. The cache is an optimisation; the
		 * arenas are the allocator. Leaving the pointer NULL and letting
		 * every path below fall through to the arena keeps the allocator
		 * working with one fewer dependency at startup.
		 *
		 * There used to be a `static struct tcache fallback` here. It was
		 * unreachable in practice and expensive in fact: 8192 class slots
		 * of freelist plus count is 8192*10 = 80 KiB of .bss in every
		 * program that links malloc, and since a user image is embedded
		 * whole in the kernel, that is 80 KiB of address space handed to
		 * PID 1 to hold a path that only runs when mmap fails.
		 */
		tcache = NULL;
		return;
	}
	memset(tcache, 0, sizeof(struct tcache));
	tcache->init_magic = TCACHE_INIT_MAGIC;
}

static void tcache_free(struct chunk *ch)
{
	size_t class_idx = ch->class_idx;

	if (is_large_class(class_idx))
		return;
	if (tcache && tcache->count[class_idx] < TCACHE_BATCH) {
		ch->next = tcache->freelist[class_idx];
		tcache->freelist[class_idx] = ch;
		tcache->count[class_idx]++;
	} else if (arena_list) {
		/* Flush to the arena: either the cache is full or there is none. */
		spin_lock(&arena_lock);
		ch->next = arena_list->free_list[class_idx];
		arena_list->free_list[class_idx] = ch;
		spin_unlock(&arena_lock);
	}
}

static struct chunk *tcache_alloc(size_t class_idx)
{
	struct chunk *ch;

	if (is_large_class(class_idx))
		return NULL;
	if (tcache && tcache->count[class_idx] == 0) {
		/* Refill from arena */
		spin_lock(&arena_lock);
		if (arena_list) {
			struct chunk *batch = arena_list->free_list[class_idx];
			int n = 0;
			while (batch && n < TCACHE_BATCH) {
				struct chunk *next = batch->next;
				batch->next = tcache->freelist[class_idx];
				tcache->freelist[class_idx] = batch;
				tcache->count[class_idx]++;
				batch = next;
				n++;
			}
			arena_list->free_list[class_idx] = batch;
		}
		spin_unlock(&arena_lock);
	}
	if (!tcache || tcache->count[class_idx] == 0)
		return NULL;
	ch = tcache->freelist[class_idx];
	tcache->freelist[class_idx] = ch->next;
	tcache->count[class_idx]--;
	/*
	 * free() clears the magic on the way onto a list, so a chunk that comes
	 * back out of either the cache or an arena free list has to be
	 * re-initialised here rather than trusted to still be marked live.
	 */
	ch->magic = CHUNK_MAGIC;
	return ch;
}


static struct arena *arena_create(void)
{
	void *base = sys_mmap(NULL, ARENA_SIZE, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED)
		return NULL;
	struct arena *ar = (struct arena *)base;
	ar->base = base;
	ar->bump = (char *)base + sizeof(struct arena);
	ar->limit = (char *)base + ARENA_SIZE;
	ar->lock = 0;
	memset(ar->free_list, 0, sizeof(ar->free_list));
	spin_lock(&arena_lock);
	ar->next = arena_list;
	arena_list = ar;
	spin_unlock(&arena_lock);
	return ar;
}

static struct chunk *arena_alloc(struct arena *ar, size_t class_idx)
{
	size_t size = class_to_size(class_idx);
	size_t total = size + sizeof(struct chunk);
	/* Try free list first */
	struct chunk *ch = ar->free_list[class_idx];
	if (ch) {
		ar->free_list[class_idx] = ch->next;
		ch->magic = CHUNK_MAGIC;
		ch->size = size;
		ch->class_idx = class_idx;
		return ch;
	}
	/* Bump allocate */
	char *bump = (char *)ar->bump;
	bump = (char *)align_up((size_t)bump, ALIGNMENT);
	if (bump + total > (char *)ar->limit)
		return NULL;
	ch = (struct chunk *)bump;
	ar->bump = bump + total;
	ch->magic = CHUNK_MAGIC;
	ch->size = size;
	ch->class_idx = class_idx;
	ch->next = NULL;
	return ch;
}

static struct chunk *allocate_small(size_t size)
{
	size_t class_idx = size_to_class(size);
	struct chunk *ch;

	/*
	 * The class index is derived from an aligned size of at least
	 * ALIGNMENT, so it is in range by construction here. The check is
	 * repeated rather than assumed because every other caller of the class
	 * space (tcache_alloc, tcache_free, arena_free) guards it, and a single
	 * unguarded path is enough to subscript past the end of the array.
	 */
	if (is_large_class(class_idx))
		return NULL;

	tcache_init();
	ch = tcache_alloc(class_idx);
	if (ch) {
		return ch;
	}
	/* Slow path: allocate from arena */
	spin_lock(&arena_lock);
	if (!arena_list) {
		spin_unlock(&arena_lock);
		arena_create();
		spin_lock(&arena_lock);
	}
	spin_unlock(&arena_lock);
	spin_lock(&arena_lock);
	for (struct arena *ar = arena_list; ar; ar = ar->next) {
		ch = arena_alloc(ar, class_idx);
		if (ch) {
			spin_unlock(&arena_lock);
			return ch;
		}
	}
	spin_unlock(&arena_lock);
	/* Need new arena */
	struct arena *new_ar = arena_create();
	if (!new_ar)
		return NULL;
	spin_lock(&arena_lock);
	ch = arena_alloc(new_ar, class_idx);
	spin_unlock(&arena_lock);
	return ch;
}

/*
 * A large block is one mmap with a full `struct chunk` in front of the payload
 * and class_idx == NCLASS, so the same header arithmetic finds it again in
 * free(), realloc() and malloc_usable_size(). The previous version used a
 * 16-byte `struct large_header` and returned base + 16, which put
 * `ptr - sizeof(struct chunk)` at base - 16: every free(), realloc() and
 * malloc_usable_size() of a large block began by reading 16 bytes *before* the
 * mapping.
 */
static void *allocate_large(size_t size)
{
	size_t total;
	void *base;
	struct chunk *ch;

	if (size > SIZE_MAX - sizeof(struct chunk)) {
		__errno = ENOMEM;
		return NULL;
	}
	total = size + sizeof(struct chunk);
	base = sys_mmap(NULL, total, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED)
		return NULL;
	ch = (struct chunk *)base;
	ch->magic = CHUNK_MAGIC;
	ch->size = size;
	ch->class_idx = NCLASS;
	ch->next = NULL;
	return chunk_to_ptr(ch);
}

void *malloc(size_t size)
{
	struct chunk *ch;

	if (size == 0)
		size = 1;
	/* Rejected rather than rounded: align_up of a size within 15 of
	 * SIZE_MAX wraps to 0, size_to_class(0) is SIZE_MAX, and that index is
	 * then used to subscript the class arrays. */
	if (align_up_checked(size, ALIGNMENT, &size)) {
		__errno = ENOMEM;
		return NULL;
	}
	if (size > MAX_SMALL_SIZE) {
		return allocate_large(size);
	}
	ch = allocate_small(size);
	if (!ch)
		return NULL;
	/* Poison the payload to catch use-after-free */
	memset(chunk_to_ptr(ch), 0xFE, ch->size);
	return chunk_to_ptr(ch);
}

void free(void *ptr)
{
	struct chunk *ch;
	size_t class_idx, size;

	if (!ptr)
		return;
	ch = ptr_to_chunk(ptr);
	/*
	 * The magic is only live while the block is live: free() clears it
	 * before the chunk reaches a list. A second free of the same pointer
	 * therefore lands here with magic == 0 and aborts, instead of pushing
	 * the same chunk onto the free list a second time and letting the next
	 * two mallocs of that class hand the identical address to two owners.
	 */
	if (!chunk_is_valid(ch)) {
		abort();  /* Corrupted or already-freed chunk */
	}
	class_idx = ch->class_idx;
	size = ch->size;
	if (is_large_class(class_idx)) {
		/* Large allocation: the mmap base *is* the chunk header. */
		ch->magic = 0;
		sys_munmap(ch, size + sizeof(struct chunk));
		return;
	}
	/*
	 * Poison, and mark the chunk free. The zeroing pass this used to do for
	 * small blocks made the poison meaningless -- a use-after-free read came
	 * back as zeros, which looks like a fresh allocation rather than like
	 * freed memory -- and cost a second pass over every small free.
	 */
	memset(ptr, 0xFE, size);
	ch->magic = 0;
	tcache_free(ch);
}

void *calloc(size_t nmemb, size_t size)
{
	if (nmemb != 0 && size > SIZE_MAX / nmemb) {
		__errno = ENOMEM;
		return NULL;
	}
	size_t total = nmemb * size;
	void *ptr = malloc(total);
	if (ptr)
		memset(ptr, 0, total);
	return ptr;
}


/*
 * realloc never grows in place. There is no adjacency tracking between
 * chunks -- nothing records that the block after this one is free and starts
 * exactly here -- so the old header's claim that it could was aspirational.
 * Shrinking does split, because the tail of a chunk is known to be ours.
 */
void *realloc(void *ptr, size_t size)
{
	struct chunk *ch;
	size_t old_size, class_idx, new_class, remainder_size;
	void *new_ptr;

	if (!ptr)
		return malloc(size);
	if (size == 0) {
		free(ptr);
		return NULL;
	}
	if (align_up_checked(size, ALIGNMENT, &size)) {
		__errno = ENOMEM;
		return NULL;
	}
	ch = ptr_to_chunk(ptr);
	if (!chunk_is_valid(ch)) {
		abort();
	}
	old_size = ch->size;
	class_idx = ch->class_idx;
	if (is_large_class(class_idx)) {
		/* Large allocation: the header is reachable, so this validates. */
		void *new_ptr = malloc(size);

		if (!new_ptr)
			return NULL;
		size_t copy = old_size < size ? old_size : size;

		memcpy(new_ptr, ptr, copy);
		free(ptr);
		return new_ptr;
	}
	/* Small allocation: split on shrink, allocate-and-copy on grow. */
	new_class = size_to_class(size);
	if (new_class == class_idx) {
		/* Same size class, nothing to do */
		return ptr;
	}
	if (size < old_size) {
		/*
		 * Split only if what is left over can hold a header *and* a
		 * whole alignment unit of payload. A remainder smaller than
		 * ALIGNMENT rounds its class index down to 0, so the arena would
		 * hand it out as a 16-byte chunk that overlaps the space it does
		 * not own.
		 */
		remainder_size = old_size - size - sizeof(struct chunk);
		if (remainder_size >= ALIGNMENT) {
			struct chunk *remainder = (struct chunk *)((char *)ptr + size);

			remainder->size = remainder_size;
			remainder->class_idx = size_to_class(remainder_size);
			remainder->next = NULL;
			remainder->magic = 0;   /* free() clears magic; match it */
			ch->size = size;
			ch->class_idx = new_class;
			tcache_free(remainder);
		}
		return ptr;
	}
	new_ptr = malloc(size);
	if (!new_ptr)
		return NULL;
	memcpy(new_ptr, ptr, old_size);
	free(ptr);
	return new_ptr;
}

void *reallocarray(void *ptr, size_t nmemb, size_t size)
{
	if (nmemb != 0 && size > SIZE_MAX / nmemb) {
		__errno = ENOMEM;
		return NULL;
	}
	return realloc(ptr, nmemb * size);
}

/*
 * The two aligned entry points share everything but the C11 rule they have to
 * disagree about: aligned_alloc(7) requires size to be a multiple of alignment
 * and returns NULL when it is not, while posix_memalign(3) has no such
 * requirement and rounds. Both over-allocate alignment + sizeof(void*) bytes so
 * there is room to slide the returned address up and to stash the raw pointer
 * in the sizeof(void*) bytes immediately below it.
 */
static void *aligned_alloc_impl(size_t alignment, size_t size)
{
	size_t rounded;
	void *raw;
	uintptr_t raw_addr, aligned;

	if (align_up_checked(size, alignment, &rounded))
		return NULL;
	if (rounded > SIZE_MAX - alignment - sizeof(void *))
		return NULL;
	raw = malloc(rounded + alignment + sizeof(void *));
	if (!raw)
		return NULL;
	raw_addr = (uintptr_t)raw;
	aligned = align_up(raw_addr + sizeof(void *), alignment);
	*(void **)(aligned - sizeof(void *)) = raw;
	return (void *)aligned;
}

void *aligned_alloc(size_t alignment, size_t size)
{
	if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
		__errno = EINVAL;
		return NULL;
	}
	/* C11 7.22.3.1: an implementation shall return NULL if size is not an
	 * integral multiple of alignment. Silently rounding is how a caller
	 * asking for 64-byte-aligned 24-byte records ends up with a buffer that
	 * ends 8 bytes into the next record's alignment slot. */
	if (size == 0 || size % alignment != 0) {
		__errno = EINVAL;
		return NULL;
	}
	return aligned_alloc_impl(alignment, size);
}

int posix_memalign(void **memptr, size_t alignment, size_t size)
{
	void *p;

	if (memptr == NULL)
		return EINVAL;
	if (alignment == 0 || (alignment & (alignment - 1)) != 0 ||
	    alignment < sizeof(void *)) {
		return EINVAL;
	}
	if (size == 0)
		size = alignment;
	p = aligned_alloc_impl(alignment, size);
	if (!p)
		return ENOMEM;
	*memptr = p;
	return 0;
}

size_t malloc_usable_size(void *ptr)
{
	struct chunk *ch;

	if (!ptr)
		return 0;
	ch = ptr_to_chunk(ptr);
	if (!chunk_is_valid(ch))
		return 0;
	return ch->size;
}

int malloc_trim(size_t pad)
{
	(void)pad;
	/* No-op for now; could implement arena release in future */
	return 0;
}

/*
 * The strong definition that malloc_fork.c's weak fallback is waiting for.
 *
 * fork() gives the child a byte-for-byte copy of the parent's address space,
 * including every chunk sitting in the cache. Without a reset the child's first
 * malloc can hand it a block that the parent also believes it owns, and the two
 * write through the same physical pages.
 *
 * The arenas themselves are left mapped: the child's copy is private, so they
 * are safe to use, they are not reachable from the parent's cache, and tearing
 * them down would need an address-space walk this allocator does not have.
 * Returning the cached chunks to the arenas is enough to make the child's heap
 * self-consistent, and it costs one pass over the class counts.
 */
void __malloc_fork_child(void)
{
	unsigned long i;

	for (i = 0; i < NCLASS && tcache; i++) {
		struct chunk *ch = tcache->freelist[i];
		unsigned long n = tcache->count[i];

		tcache->freelist[i] = NULL;
		tcache->count[i] = 0;
		if (!arena_list)
			continue;
		while (ch && n--) {
			struct chunk *next = ch->next;

			ch->next = arena_list->free_list[i];
			arena_list->free_list[i] = ch;
			ch = next;
		}
	}
}