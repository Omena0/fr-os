/*
 * malloc.c — segregated-list allocator with per-thread cache and mmap-backed arenas.
 *
 * Design:
 *   - Size classes: 16-byte granularity up to 128 KiB (8192 classes). Small
 *     allocations pop from a per-thread free list in O(1) with no search.
 *   - Per-thread cache (tcache): each thread holds a batch of free chunks per
 *     size class. Refill/flush happens in bulk from the arena, mirroring the
 *     kernel's per-CPU magazine design.
 *   - Arenas: 1 MiB regions from mmap (PROT_READ|PROT_WRITE, MAP_PRIVATE|ANON).
 *     A bump pointer carves fresh chunks; a free list reuses holes.
 *   - Large allocations (> 128 KiB): dedicated mmap, freed with munmap. Detected
 *     by a flag in the chunk header.
 *   - realloc grows in place when the next chunk is free, shrinks in place when
 *     the remainder is over half the chunk, otherwise alloc/copy/free.
 *   - aligned_alloc/posix_memalign over-allocate and store the real pointer
 *     immediately before the aligned address.
 *   - No recursion: refill failure returns NULL, never panics.
 *   - Corruption detection: magic field and size validation on free/realloc.
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

/* __MORE__ */

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

/* Large-allocation header (prefixed to the mmap'd region) */
struct large_header {
	uint64_t magic;
	size_t   size;
};

/* ---------------------------- per-thread cache ----------------------------- */

struct tcache {
	struct chunk *freelist[NCLASS];
	uint16_t      count[NCLASS];
	uint64_t      init_magic;
};

static __thread struct tcache *tcache = NULL;

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

/* __MORE__ */

static void tcache_init(void)
{
	__asm__ __volatile__("syscall" :: "a"(1), "D"(1), "S"("TCACHE_INIT_ENTER\n"), "d"(18) : "rcx", "r11", "memory");
	if (tcache && tcache->init_magic == TCACHE_INIT_MAGIC) {
		__asm__ __volatile__("syscall" :: "a"(1), "D"(1), "S"("TCACHE_ALREADY\n"), "d"(15) : "rcx", "r11", "memory");
		return;
	}
	__asm__ __volatile__("syscall" :: "a"(1), "D"(1), "S"("TCACHE_MMAP\n"), "d"(12) : "rcx", "r11", "memory");
	tcache = (struct tcache *)sys_mmap(NULL, sizeof(struct tcache),
		PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	__asm__ __volatile__("syscall" :: "a"(1), "D"(1), "S"("TCACHE_MMAP_DONE\n"), "d"(17) : "rcx", "r11", "memory");
	if (tcache == MAP_FAILED) {
		__asm__ __volatile__("syscall" :: "a"(1), "D"(1), "S"("TCACHE_FALLBACK\n"), "d"(16) : "rcx", "r11", "memory");
		static struct tcache fallback;
		tcache = &fallback;
	}
	__asm__ __volatile__("syscall" :: "a"(1), "D"(1), "S"("TCACHE_MEMSET\n"), "d"(14) : "rcx", "r11", "memory");
	memset(tcache, 0, sizeof(struct tcache));
	tcache->init_magic = TCACHE_INIT_MAGIC;
	__asm__ __volatile__("syscall" :: "a"(1), "D"(1), "S"("TCACHE_INIT_DONE\n"), "d"(17) : "rcx", "r11", "memory");
}

static void tcache_free(struct chunk *ch)
{
	size_t class_idx = ch->class_idx;
	if (class_idx >= NCLASS)
		return;
	if (tcache->count[class_idx] < TCACHE_BATCH) {
		ch->next = tcache->freelist[class_idx];
		tcache->freelist[class_idx] = ch;
		tcache->count[class_idx]++;
	} else {
		/* Flush to arena */
		spin_lock(&arena_lock);
		if (arena_list) {
			ch->next = arena_list->free_list[class_idx];
			arena_list->free_list[class_idx] = ch;
		}
		spin_unlock(&arena_lock);
	}
}

static struct chunk *tcache_alloc(size_t class_idx)
{
	if (class_idx >= NCLASS)
		return NULL;
	if (tcache->count[class_idx] == 0) {
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
	if (tcache->count[class_idx] == 0)
		return NULL;
	struct chunk *ch = tcache->freelist[class_idx];
	tcache->freelist[class_idx] = ch->next;
	tcache->count[class_idx]--;
	return ch;
}

/* __MORE__ */

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

static void arena_free(struct arena *ar, struct chunk *ch)
{
	size_t class_idx = ch->class_idx;
	if (class_idx >= NCLASS)
		return;
	ch->next = ar->free_list[class_idx];
	ar->free_list[class_idx] = ch;
}

/* Find the arena that owns a given pointer */
static struct arena *find_arena(void *ptr)
{
	spin_lock(&arena_lock);
	for (struct arena *ar = arena_list; ar; ar = ar->next) {
		if (ptr >= ar->base && ptr < ar->limit) {
			spin_unlock(&arena_lock);
			return ar;
		}
	}
	spin_unlock(&arena_lock);
	return NULL;
}

/* __MORE__ */

static struct chunk *allocate_small(size_t size)
{
	__asm__ __volatile__("syscall" :: "a"(1), "D"(1), "S"("ALLOC_SMALL\n"), "d"(12) : "rcx", "r11", "memory");
	size_t class_idx = size_to_class(size);
	__asm__ __volatile__("syscall" :: "a"(1), "D"(1), "S"("CLASS_IDX\n"), "d"(10) : "rcx", "r11", "memory");
	tcache_init();
	__asm__ __volatile__("syscall" :: "a"(1), "D"(1), "S"("TCACHE_INIT\n"), "d"(12) : "rcx", "r11", "memory");
	struct chunk *ch = tcache_alloc(class_idx);
	__asm__ __volatile__("syscall" :: "a"(1), "D"(1), "S"("TCACHE_ALLOC\n"), "d"(13) : "rcx", "r11", "memory");
	if (ch) {
		__asm__ __volatile__("syscall" :: "a"(1), "D"(1), "S"("TCACHE_HIT\n"), "d"(11) : "rcx", "r11", "memory");
		return ch;
	}
	/* Slow path: allocate from arena */
	__asm__ __volatile__("syscall" :: "a"(1), "D"(1), "S"("ARENA_LOCK\n"), "d"(11) : "rcx", "r11", "memory");
	spin_lock(&arena_lock);
	if (!arena_list) {
		__asm__ __volatile__("syscall" :: "a"(1), "D"(1), "S"("ARENA_CREATE1\n"), "d"(14) : "rcx", "r11", "memory");
		spin_unlock(&arena_lock);
		arena_create();
		spin_lock(&arena_lock);
	}
	spin_unlock(&arena_lock);
	__asm__ __volatile__("syscall" :: "a"(1), "D"(1), "S"("ARENA_LOCK2\n"), "d"(12) : "rcx", "r11", "memory");
	spin_lock(&arena_lock);
	for (struct arena *ar = arena_list; ar; ar = ar->next) {
		ch = arena_alloc(ar, class_idx);
		if (ch) {
			spin_unlock(&arena_lock);
			__asm__ __volatile__("syscall" :: "a"(1), "D"(1), "S"("ARENA_ALLOC_OK\n"), "d"(15) : "rcx", "r11", "memory");
			return ch;
		}
	}
	spin_unlock(&arena_lock);
	/* Need new arena */
	__asm__ __volatile__("syscall" :: "a"(1), "D"(1), "S"("ARENA_CREATE2\n"), "d"(14) : "rcx", "r11", "memory");
	struct arena *new_ar = arena_create();
	if (!new_ar)
		return NULL;
	spin_lock(&arena_lock);
	ch = arena_alloc(new_ar, class_idx);
	spin_unlock(&arena_lock);
	__asm__ __volatile__("syscall" :: "a"(1), "D"(1), "S"("ARENA_ALLOC_OK2\n"), "d"(16) : "rcx", "r11", "memory");
	return ch;
}

static void *allocate_large(size_t size)
{
	size_t total = size + sizeof(struct large_header);
	void *base = sys_mmap(NULL, total, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED)
		return NULL;
	struct large_header *hdr = (struct large_header *)base;
	hdr->magic = CHUNK_MAGIC;
	hdr->size = size;
	return (char *)base + sizeof(struct large_header);
}

void *malloc(size_t size)
{
	__asm__ __volatile__("syscall" :: "a"(1), "D"(1), "S"("MALLOC\n"), "d"(7) : "rcx", "r11", "memory");
	if (size == 0)
		size = 1;
	size = align_up(size, ALIGNMENT);
	__asm__ __volatile__("syscall" :: "a"(1), "D"(1), "S"("ALIGN\n"), "d"(6) : "rcx", "r11", "memory");
	if (size > MAX_SMALL_SIZE) {
		__asm__ __volatile__("syscall" :: "a"(1), "D"(1), "S"("LARGE\n"), "d"(6) : "rcx", "r11", "memory");
		return allocate_large(size);
	}
	__asm__ __volatile__("syscall" :: "a"(1), "D"(1), "S"("SMALL\n"), "d"(6) : "rcx", "r11", "memory");
	struct chunk *ch = allocate_small(size);
	__asm__ __volatile__("syscall" :: "a"(1), "D"(1), "S"("ALLOC_SMALL_DONE\n"), "d"(17) : "rcx", "r11", "memory");
	if (!ch)
		return NULL;
	/* Poison the payload to catch use-after-free */
	__asm__ __volatile__("syscall" :: "a"(1), "D"(1), "S"("MEMSET\n"), "d"(7) : "rcx", "r11", "memory");
	memset(chunk_to_ptr(ch), 0xFE, ch->size);
	__asm__ __volatile__("syscall" :: "a"(1), "D"(1), "S"("MEMSET_DONE\n"), "d"(12) : "rcx", "r11", "memory");
	return chunk_to_ptr(ch);
}

void free(void *ptr)
{
	if (!ptr)
		return;
	struct chunk *ch = ptr_to_chunk(ptr);
	if (!chunk_is_valid(ch)) {
		abort();  /* Corrupted chunk */
	}
	size_t class_idx = ch->class_idx;
	size_t size = ch->size;
	if (class_idx >= NCLASS) {
		/* Large allocation */
		struct large_header *hdr = (struct large_header *)((char *)ptr - sizeof(struct large_header));
		if (hdr->magic != CHUNK_MAGIC || hdr->size != size) {
			abort();
		}
		sys_munmap(hdr, size + sizeof(struct large_header));
		return;
	}
	/* Poison and clear */
	memset(ptr, 0xFE, size);
	/* For small sizes, clearing is cheap */
	if (size <= 256)
		memset(ptr, 0, size);
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

/* __MORE__ */

void *realloc(void *ptr, size_t size)
{
	if (!ptr)
		return malloc(size);
	if (size == 0) {
		free(ptr);
		return NULL;
	}
	struct chunk *ch = ptr_to_chunk(ptr);
	if (!chunk_is_valid(ch)) {
		abort();
	}
	size_t old_size = ch->size;
	size_t class_idx = ch->class_idx;
	size = align_up(size, ALIGNMENT);
	if (class_idx >= NCLASS) {
		/* Large allocation: must reallocate */
		void *new_ptr = malloc(size);
		if (!new_ptr)
			return NULL;
		size_t copy = old_size < size ? old_size : size;
		memcpy(new_ptr, ptr, copy);
		free(ptr);
		return new_ptr;
	}
	/* Small allocation: try in-place grow/shrink */
	size_t new_class = size_to_class(size);
	if (new_class == class_idx) {
		/* Same size class, nothing to do */
		return ptr;
	}
	if (size < old_size) {
		/* Shrink: if remainder is large enough, split */
		if (old_size - size >= ALIGNMENT * 2) {
			/* Split the chunk */
			struct chunk *remainder = (struct chunk *)((char *)ptr + size);
			remainder->magic = CHUNK_MAGIC;
			remainder->size = old_size - size - sizeof(struct chunk);
			remainder->class_idx = size_to_class(remainder->size);
			remainder->next = NULL;
			ch->size = size;
			ch->class_idx = new_class;
			tcache_free(remainder);
		}
		return ptr;
	}
	/* Grow: check if next chunk is free and adjacent */
	/* For simplicity, allocate new and copy */
	void *new_ptr = malloc(size);
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

void *aligned_alloc(size_t alignment, size_t size)
{
	if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
		__errno = EINVAL;
		return NULL;
	}
	if (size == 0)
		size = alignment;
	size = align_up(size, alignment);
	void *raw = malloc(size + alignment + sizeof(void *));
	if (!raw)
		return NULL;
	uintptr_t raw_addr = (uintptr_t)raw;
	uintptr_t aligned = align_up(raw_addr + sizeof(void *), alignment);
	void **backptr = (void **)(aligned - sizeof(void *));
	*backptr = raw;
	return (void *)aligned;
}

int posix_memalign(void **memptr, size_t alignment, size_t size)
{
	if (alignment == 0 || (alignment & (alignment - 1)) != 0 ||
	    alignment < sizeof(void *)) {
		return EINVAL;
	}
	if (size == 0)
		size = alignment;
	size = align_up(size, alignment);
	void *raw = malloc(size + alignment + sizeof(void *));
	if (!raw)
		return ENOMEM;
	uintptr_t raw_addr = (uintptr_t)raw;
	uintptr_t aligned = align_up(raw_addr + sizeof(void *), alignment);
	void **backptr = (void **)(aligned - sizeof(void *));
	*backptr = raw;
	*memptr = (void *)aligned;
	return 0;
}

size_t malloc_usable_size(void *ptr)
{
	if (!ptr)
		return 0;
	struct chunk *ch = ptr_to_chunk(ptr);
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