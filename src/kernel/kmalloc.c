/*
 * kmalloc: general kernel heap allocation.
 *
 * Two tiers, chosen by size. Requests up to KMALLOC_CACHE_MAX come from the
 * SLAB caches described in docs/src/memory/slab-allocator.md; anything larger
 * goes to vmalloc, which already hands out contiguous page-backed memory and
 * has no per-object overhead to amortise.
 *
 * The generic caches are the power-of-two set the document names, kmalloc-8
 * through kmalloc-4096. The 4 KiB ceiling is not arbitrary: it is the largest
 * class where a slab still holds a useful number of objects. An 8 KiB class
 * would need a 32 KiB slab to hold more than one object, so every such
 * allocation would cost four times its own size. Large objects are better off
 * on the vmalloc path, which is already correct for them.
 *
 * Locking is three-tier, cheapest first:
 *
 *   1. per-CPU magazine  -- one spinlock (irqsave) per CPU per cache
 *   2. cache lock        -- partial/full slab lists
 *   3. zone lock         -- inside pmm, for a new slab
 *
 * The document describes the magazine path as lock-free. It is not, and cannot
 * be while interrupts are enabled: an interrupt that allocates mid-push
 * observes a half-updated magazine, and the consequence is a duplicated
 * pointer handed to two callers. irqsave is a few cycles and closes the window.
 *
 * A slab's free count does not include objects parked in magazines, so a slab
 * whose objects are all in magazines looks "full" and is not returned to the
 * buddy allocator until those objects are flushed back. That is deliberately
 * conservative in the safe direction: reclaiming a slab early would free pages
 * that a magazine still hands out.
 */

#include <vmm.h>
#include <pmm.h>
#include <list.h>
#include <percpu.h>
#include <errno.h>
#include <string.h>
#include <types.h>
#include <spinlock.h>
#include <panic.h>

/* The largest class served from SLAB. See the file header. */
#define KMALLOC_CACHE_MAX	4096

/* Objects per cache per CPU, per docs/src/memory/per-cpu-caches.md. */
#define KMALLOC_MAG_SIZE	64

/* Slabs are grown until one holds at least this many objects, so the slab
 * header's fixed cost is never a large fraction of the slab. */
#define KMALLOC_MIN_PER_SLAB	4

/* Index meaning "end of the free list". Also the on-slab value when a slab
 * has no free objects. */
#define SLAB_NONE		0xFFFFFFFFu

/* -------------------------------------------------- generic size classes ---- */

static const size_t kcache_size[] = {
	8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096,
};

#define NUM_KCACHE	(sizeof(kcache_size) / sizeof(kcache_size[0]))

/* --------------------------------------------------------------- slab ------- */

/*
 * One buddy-allocated block, carved into objects. The free list is threaded
 * through the objects themselves as a chain of indices rather than as an
 * intrusive list of pointers. That costs 4 bytes per object instead of 16,
 * and it is what makes the 8-byte class possible at all: a list_head is two
 * pointers and would not fit.
 */
struct kmalloc_slab {
	struct list_head node;	/* on the cache's partial or full list */
	struct page *page;	/* the buddy block, for pmm_free_pages() */
	uint32_t total;		/* objects in this slab */
	uint32_t free;		/* objects on the freelist */
	uint32_t first;		/* index of the first free object, or SLAB_NONE */
};

struct kmalloc_magazine {
	uint32_t count;
	void *objects[KMALLOC_MAG_SIZE];
};

struct kmalloc_cache {
	size_t size;
	unsigned order;		/* buddy order of one slab */
	size_t slab_bytes;	/* PAGE_SIZE << order */
	size_t obj_off;		/* first object, past the slab header */
	uint32_t per_slab;
	struct list_head partial;
	struct list_head full;
	spinlock_t lock;
};

static struct kmalloc_cache kcaches[NUM_KCACHE];

/*
 * Magazines are indexed [cpu][cache] rather than hanging off struct percpu_data,
 * so this file does not need to reserve fixed space in a shared per-CPU
 * structure that other subsystems are also growing. At MAX_CPUS this is
 * NUM_KCACHE * KMALLOC_MAG_SIZE * 8 * MAX_CPUS bytes of BSS -- about 1.3 MiB,
 * all of it untouched until a CPU actually allocates from that class.
 */
static struct kmalloc_magazine kmags[MAX_CPUS][NUM_KCACHE];
static spinlock_t kmag_lock[MAX_CPUS][NUM_KCACHE];

static u64 stat_allocs;
static u64 stat_frees;
static u64 stat_bytes;		/* counted at class size, not request size */

/* ---------------------------------------------------------------- helpers --- */

static void *slab_obj(const struct kmalloc_cache *c,
		      const struct kmalloc_slab *s, uint32_t i)
{
	return (char *)s + c->obj_off + (size_t)i * c->size;
}

static uint32_t obj_index(const struct kmalloc_cache *c,
			  const struct kmalloc_slab *s, const void *o)
{
	return (uint32_t)(((const char *)o - ((const char *)s + c->obj_off)) /
			  c->size);
}

/*
 * The slab an object belongs to, found by masking off the low bits.
 *
 * This works only because a slab is a page-aligned block whose length is a
 * power of two, and because every object lies inside it at a slab-local
 * offset. It is what makes kfree() possible without a per-object back pointer
 * eating a header's worth of every allocation.
 */
static struct kmalloc_slab *slab_of(const struct kmalloc_cache *c, void *obj)
{
	return (struct kmalloc_slab *)((uintptr_t)obj & ~(c->slab_bytes - 1));
}

static int kcache_init_one(struct kmalloc_cache *c, size_t size)
{
	size_t obj_off = ALIGN_UP(sizeof(struct kmalloc_slab), 8);

	for (unsigned o = 0; o <= BUDDY_MAX_ORDER; o++) {
		size_t bytes = (size_t)PAGE_SIZE << o;

		if ((bytes - obj_off) / size >= KMALLOC_MIN_PER_SLAB) {
			c->size = size;
			c->order = o;
			c->slab_bytes = bytes;
			c->obj_off = obj_off;
			c->per_slab = (uint32_t)((bytes - obj_off) / size);
			return 0;
		}
	}
	return -ENOMEM;
}

/* ---------------------------------------------------------- slab growth ----- */

static struct kmalloc_slab *slab_new(struct kmalloc_cache *c)
{
	struct kmalloc_slab *s;
	struct page *p;
	phys_addr_t phys;

	p = pmm_alloc_pages(c->order, PG_SLAB);
	if (!p)
		return NULL;

	phys = page_to_phys(p);
	s = (struct kmalloc_slab *)phys_to_virt(phys);

	s->page = p;
	s->total = c->per_slab;
	s->free = c->per_slab;

	/*
	 * Thread the free list through the objects themselves, back to front so
	 * that allocation hands out ascending addresses. Back to front matters
	 * for locality the same way forward order does; ascending is simply the
	 * direction the hardware prefetcher expects.
	 */
	s->first = c->per_slab - 1;
	for (uint32_t i = c->per_slab - 1; i > 0; i--)
		*(uint32_t *)slab_obj(c, s, i - 1) = i - 1;
	*(uint32_t *)slab_obj(c, s, 0) = SLAB_NONE;

	return s;
}

static void slab_release(struct kmalloc_cache *c, struct kmalloc_slab *s)
{
	list_del_init(&s->node);
	pmm_free_pages(s->page, c->order);
}

/* ------------------------------------------------- cache-level alloc/free --- */

static void *cache_alloc(struct kmalloc_cache *c)
{
	struct kmalloc_slab *s;
	void *obj;

	if (list_empty(&c->partial)) {
		s = slab_new(c);
		if (!s)
			return NULL;
		list_add_tail(&s->node, &c->partial);
	} else {
		s = list_entry(c->partial.next, struct kmalloc_slab, node);
	}

	obj = slab_obj(c, s, s->first);
	s->first = *(uint32_t *)obj;
	s->free--;

	/* The last object just went out; the slab has no way to satisfy another
	 * request without growing, so it moves to the full list now rather than
	 * being rediscovered empty on the next call. */
	if (s->free == 0)
		list_move(&s->node, &c->full);

	return obj;
}

static void cache_free(struct kmalloc_cache *c, void *obj)
{
	struct kmalloc_slab *s = slab_of(c, obj);
	bool was_full = (s->free == 0);

	*(uint32_t *)obj = s->first;
	s->first = obj_index(c, s, obj);
	s->free++;

	if (was_full)
		list_move(&s->node, &c->partial);

	if (s->free == s->total)
		slab_release(c, s);
}

/* ------------------------------------------------------------- magazines ---- */

static void *mag_pop(struct kmalloc_magazine *m)
{
	return m->count ? m->objects[--m->count] : NULL;
}

/* ----------------------------------------------------------- size lookup ---- */

/* Index of the class serving `size`, or -1 if it is too big for SLAB. */
static int class_for(size_t size)
{
	if (size == 0)
		return 0;
	if (size > KMALLOC_CACHE_MAX)
		return -1;

	for (size_t i = 0; i < NUM_KCACHE; i++)
		if (size <= kcache_size[i])
			return (int)i;

	return -1;		/* unreachable: KMALLOC_CACHE_MAX is the last class */
}

/* ------------------------------------------------------------------ init ---- */

void kmalloc_init(void)
{
	for (size_t i = 0; i < NUM_KCACHE; i++) {
		struct kmalloc_cache *c = &kcaches[i];
		size_t size = kcache_size[i];

		list_init(&c->partial);
		list_init(&c->full);
		spinlock_init(&c->lock);

		if (kcache_init_one(c, size) < 0)
			panic("kmalloc: no slab order fits a %u-byte class", (unsigned)size);
	}
}

/* ----------------------------------------------------------------- kmalloc --- */

void *kmalloc(size_t size)
{
	struct kmalloc_magazine *m;
	struct kmalloc_cache *c;
	unsigned cpu = this_cpu_id();
	int idx;
	void *obj;
	u64 flags;

	idx = class_for(size);
	if (idx < 0)
		return vmalloc(size);

	c = &kcaches[idx];

	/* Fast path: this CPU has a spare object. */
	m = &kmags[cpu][idx];
	flags = spinlock_irqsave(&kmag_lock[cpu][idx]);
	obj = mag_pop(m);
	spinlock_unlock_irqrestore(&kmag_lock[cpu][idx], flags);
	if (obj) {
		__atomic_fetch_add(&stat_allocs, 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&stat_bytes, c->size, __ATOMIC_RELAXED);
		return obj;
	}

	/* Slow path: refill this CPU's magazine a slab's worth at a time. */
	flags = spinlock_irqsave(&c->lock);
	for (int i = 0; i < 8; i++) {
		obj = cache_alloc(c);
		if (!obj)
			break;
		/* Round-robin into the tail so a refill does not undo the
		 * ascending-address order the slab builder established. */
		m->objects[m->count++] = obj;
	}
	spinlock_unlock_irqrestore(&c->lock, flags);

	obj = mag_pop(m);
	if (!obj)
		return NULL;

	__atomic_fetch_add(&stat_allocs, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&stat_bytes, c->size, __ATOMIC_RELAXED);
	return obj;
}

void *kmalloc_zeroed(size_t size)
{
	void *p = kmalloc(size);

	if (p)
		memset(p, 0, size);
	return p;
}

void *kcalloc(size_t n, size_t size)
{
	/* n * size is computed in size_t, so the product of two large values
	 * wraps and would allocate something far too small for the caller to
	 * then write past. */
	if (n && size > (size_t)-1 / n)
		return NULL;
	return kmalloc_zeroed(n * size);
}

void kfree(void *ptr, size_t size)
{
	struct kmalloc_magazine *m;
	struct kmalloc_cache *c;
	unsigned cpu = this_cpu_id();
	int idx;
	u64 flags;

	if (!ptr)
		return;

	idx = class_for(size);
	if (idx < 0) {
		vfree(ptr);
		return;
	}

	c = &kcaches[idx];

	/* Fast path: park the object on this CPU. */
	m = &kmags[cpu][idx];
	flags = spinlock_irqsave(&kmag_lock[cpu][idx]);
	if (m->count < KMALLOC_MAG_SIZE) {
		m->objects[m->count++] = ptr;
		spinlock_unlock_irqrestore(&kmag_lock[cpu][idx], flags);
		__atomic_fetch_add(&stat_frees, 1, __ATOMIC_RELAXED);
		__atomic_fetch_sub(&stat_bytes, c->size, __ATOMIC_RELAXED);
		return;
	}
	spinlock_unlock_irqrestore(&kmag_lock[cpu][idx], flags);

	/* Magazine is full. Return it the slow way so the slab can reclaim. */
	flags = spinlock_irqsave(&c->lock);
	cache_free(c, ptr);
	spinlock_unlock_irqrestore(&c->lock, flags);

	__atomic_fetch_add(&stat_frees, 1, __ATOMIC_RELAXED);
	__atomic_fetch_sub(&stat_bytes, c->size, __ATOMIC_RELAXED);
}

void *krealloc(void *ptr, size_t old_size, size_t new_size)
{
	void *n;
	size_t copy;

	if (!ptr)
		return kmalloc(new_size);
	if (new_size == 0) {
		kfree(ptr, old_size);
		return NULL;
	}

	n = kmalloc(new_size);
	if (!n)
		return NULL;

	copy = old_size < new_size ? old_size : new_size;
	memcpy(n, ptr, copy);
	kfree(ptr, old_size);
	return n;
}

char *kstrdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = kmalloc(n);

	if (p)
		memcpy(p, s, n);
	return p;
}

void kmalloc_stats(u64 *allocs, u64 *frees, u64 *bytes_in_use)
{
	if (allocs)
		*allocs = __atomic_load_n(&stat_allocs, __ATOMIC_RELAXED);
	if (frees)
		*frees = __atomic_load_n(&stat_frees, __ATOMIC_RELAXED);
	if (bytes_in_use)
		*bytes_in_use = __atomic_load_n(&stat_bytes, __ATOMIC_RELAXED);
}
