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
 *
 * Ordering. kmalloc_init() is the only thing that makes this allocator usable,
 * and every entry point here treats a cache that has not been through it as an
 * allocator with no pages: kmalloc() returns NULL, and it returns it before it
 * reads this_cpu_id(), so a call made before percpu_setup() cannot index the
 * per-CPU magazines with an unestablished CPU number. Nothing in this file
 * depends on a cache's fields being non-zero to be safe: every field that would
 * otherwise become a loop bound or a divisor is range-checked against the
 * slab's own capacity before it is used, so a zeroed cache produces a NULL
 * rather than a four-billion-iteration write loop. An allocation failure is
 * therefore always an answer this file can give.
 *
 * The `size` argument of kfree() is advisory. It selects a first candidate
 * cache, and the slab header decides the truth: a block is returned to the
 * class it was carved from, never to the class the caller happened to name.
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

/* Objects a refill asks the cache for at once. A refill takes a slab's worth,
 * and no slab holds more than this, so the two numbers only meet at the top. */
#define KMALLOC_MAG_REFILL	8

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
	uint32_t class_idx;	/* the cache this slab was carved for */
};

/*
 * The class tag is what lets kfree() find the block a pointer came from without
 * trusting the caller's size. Eight of the ten classes use a 4 KiB slab, so
 * masking the pointer with slab_bytes distinguishes them not at all: the tag
 * is the only thing that can.
 *
 * It occupies padding that existed anyway. The header was 36 bytes and objects
 * start at the next 8-byte boundary, so adding a fourth uint32_t costs nothing
 * and moves no object.
 */
STATIC_ASSERT(sizeof(struct kmalloc_slab) <= 40,
	      "slab header must not grow past the 8-byte-aligned header before it");

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
static u64 stat_size_mismatch;	/* frees whose `size` disagreed with the block */
static u64 stat_bad_free;	/* frees of a pointer no slab owns */

/*
 * Set as the last thing kmalloc_init() does. Until it is set, no cache in
 * kcaches[] describes a usable allocator, and every path that would consume a
 * cache's geometry returns NULL instead.
 */
static bool kmalloc_ready;

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

/*
 * Is `obj` an object start inside the slab `s`?
 *
 * The mask in slab_of() is only meaningful for the cache the object was carved
 * from: applied with the wrong slab size it lands on a page that is merely
 * covered by some other slab, or on payload bytes in the middle of one. Both
 * are rejected here, by asking the candidate header to identify itself. Two
 * independent checks, because the wrong candidate can look right: the range
 * test rejects a pointer past the end of the block or in the header, and the
 * header test rejects a page of object payload that happens to sit where a slab
 * head would be.
 */
static bool slab_owns(const struct kmalloc_cache *c,
		      const struct kmalloc_slab *s, const void *obj)
{
	size_t off = (size_t)((const char *)obj - (const char *)s);
	size_t idx = (size_t)(c - kcaches);

	if (!c->size || !c->slab_bytes || !c->per_slab || !c->obj_off)
		return false;
	if (idx >= NUM_KCACHE)
		return false;
	if (off < c->obj_off || off + c->size > c->slab_bytes)
		return false;			/* outside the block, or header */
	if ((off - c->obj_off) % c->size)
		return false;			/* interior pointer */
	if (s->class_idx != idx || s->total != c->per_slab)
		return false;			/* not this cache's slab */
	if (s->free > s->total)
		return false;			/* header is not one of ours */
	return true;
}

/*
 * The slab that owns `obj`, found without being told which cache it came from.
 *
 * Every cache's mask is tried and the header's own claim decides. Slabs are
 * disjoint buddy blocks, so at most one cache can own a real object; a second
 * candidate would mean object payload that coincidentally reads as a header,
 * and rather than guess, that case is reported as "no owner" so the caller can
 * refuse the free instead of writing a freelist link into the wrong class.
 */
static struct kmalloc_slab *slab_find_owner(void *obj,
					    struct kmalloc_cache **owner)
{
	struct kmalloc_slab *found = NULL;
	struct kmalloc_cache *fc = NULL;

	for (size_t i = 0; i < NUM_KCACHE; i++) {
		struct kmalloc_cache *c = &kcaches[i];
		struct kmalloc_slab *s;

		if (!c->slab_bytes)
			continue;		/* cache not initialised */

		s = slab_of(c, obj);
		if (!slab_owns(c, s, obj))
			continue;

		if (found)
			return NULL;		/* ambiguous: refuse */
		found = s;
		fc = c;
	}

	*owner = fc;
	return found;
}

static int kcache_init_one(struct kmalloc_cache *c, size_t size)
{
	size_t obj_off = ALIGN_UP(sizeof(struct kmalloc_slab), 8);

	for (unsigned o = 0; o <= BUDDY_MAX_ORDER; o++) {
		size_t bytes = (size_t)PAGE_SIZE << o;

		if ((bytes - obj_off) / size >= KMALLOC_MIN_PER_SLAB) {
			uint32_t per_slab = (uint32_t)((bytes - obj_off) / size);

			/*
			 * kfree() decides which tier a pointer came from by one
			 * bit: vmalloc blocks are page-aligned, slab objects are
			 * not. That decision is only sound while no object in this
			 * class lands on a page boundary.
			 *
			 * Every class size divides PAGE_SIZE, so the object
			 * offsets hit a page boundary exactly when the size
			 * divides obj_off and the first such object still fits
			 * inside the block. For the current geometry (obj_off 40,
			 * per_slab 507 for the 8-byte class) the answer is no,
			 * by one object -- so this is checked rather than
			 * assumed, because the margin is one object wide and a
			 * future header size could erase it.
			 */
			if (obj_off % size == 0 &&
			    (PAGE_SIZE - (obj_off % PAGE_SIZE)) / size < per_slab)
				panic("kmalloc: %u-byte objects would be page-aligned",
				      (unsigned)size);

			c->size = size;
			c->order = o;
			c->slab_bytes = bytes;
			c->obj_off = obj_off;
			c->per_slab = per_slab;
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
	uint32_t per_slab;

	/*
	 * An allocator with no pages or no size class cannot make a slab. This
	 * is the check that makes the loop below incapable of running away: it is
	 * reached with a cache that has not been through kmalloc_init(), where
	 * per_slab is 0 and the old code threaded a free list from 0xFFFFFFFF
	 * downwards, indexing a slab whose size class was 0 -- so every
	 * iteration wrote the same four bytes, four billion times.
	 */
	if (!kmalloc_ready || !c->size || !c->per_slab || !c->slab_bytes ||
	    !c->obj_off)
		return NULL;

	/*
	 * And a cache that claims more objects than its own slab can hold is
	 * refused, whatever it claims. per_slab is derived from slab_bytes and
	 * size in kcache_init_one(), so this holds for every cache that exists;
	 * it is here so the bound below is a property of the check and not of the
	 * caller having initialised the cache first.
	 */
	per_slab = c->per_slab;
	if ((u64)per_slab * c->size > (u64)c->slab_bytes - c->obj_off)
		return NULL;

	p = pmm_alloc_pages(c->order, GFP_KERNEL);
	if (!p)
		return NULL;

	phys = page_to_phys(p);
	s = (struct kmalloc_slab *)phys_to_virt(phys);

	s->page = p;
	s->total = per_slab;
	s->free = per_slab;
	s->class_idx = (uint32_t)(c - kcaches);

	/*
	 * Thread the free list through the objects themselves, ascending, so
	 * that allocation hands out ascending addresses -- which is the direction
	 * the hardware prefetcher expects, and the order the magazine refill
	 * below preserves when it pushes a slab's objects onto its tail.
	 *
	 * Every object on the list has to carry a link, including the one the
	 * head points at and the one the tail terminates, so the head is object 0
	 * and object per_slab-1 is the tail rather than the reverse: a head that
	 * is never written is a head whose "next" is whatever the page happened
	 * to contain, and a list built the other way round leaves that same link
	 * uninitialised while writing each remaining object as a pointer to
	 * itself.
	 */
	s->first = 0;
	for (uint32_t i = 0; i + 1 < per_slab; i++)
		*(uint32_t *)slab_obj(c, s, i) = i + 1;
	*(uint32_t *)slab_obj(c, s, per_slab - 1) = SLAB_NONE;

	return s;
}

static void slab_release(struct kmalloc_cache *c, struct kmalloc_slab *s)
{
	list_del_init(&s->node);
	pmm_free_pages(s->page, c->order);
}

/* ------------------------------------------------- cache-level alloc/free --- */

/*
 * Take one object off a slab's free list.
 *
 * `first` is an index into the block, and SLAB_NONE is a legal value for it, so
 * the two ways of running off the end -- an exhausted free count and a
 * terminated list -- are both checked here rather than used as addresses. A
 * slab whose last object has just gone out moves to the full list, which is
 * what keeps an exhausted slab from being rediscovered by the next allocation.
 */
static void *slab_take(struct kmalloc_cache *c, struct kmalloc_slab *s)
{
	void *obj;

	if (s->free == 0 || s->first == SLAB_NONE || s->first >= s->total)
		return NULL;

	obj = slab_obj(c, s, s->first);
	s->first = *(uint32_t *)obj;
	s->free--;

	if (s->free == 0)
		list_move(&s->node, &c->full);

	return obj;
}

static void *cache_alloc(struct kmalloc_cache *c)
{
	struct kmalloc_slab *s;
	void *obj;

	if (!list_empty(&c->partial)) {
		s = list_entry(c->partial.next, struct kmalloc_slab, node);
		obj = slab_take(c, s);
		if (obj)
			return obj;
		/* Unreachable while slab_take() moves an exhausted slab to the
		 * full list. Kept so that a partial list which somehow holds one
		 * grows the cache instead of handing out an index past the end
		 * of a block. */
	}

	s = slab_new(c);
	if (!s)
		return NULL;
	list_add_tail(&s->node, &c->partial);

	obj = slab_take(c, s);
	if (obj)
		return obj;

	/* A slab that cannot give up its first object is not usable. Unreachable
	 * -- slab_new() refuses a geometry with no objects -- but returning the
	 * pages is cheaper than leaving a dead block on the partial list. */
	list_del(&s->node);
	pmm_free_pages(s->page, c->order);
	return NULL;
}

static void cache_free(struct kmalloc_cache *c, void *obj)
{
	struct kmalloc_slab *s = slab_of(c, obj);
	bool was_full = (s->free == 0);

	/*
	 * A partial detector for freeing the same pointer twice. It is not
	 * complete -- an object sitting in a magazine is invisible here, so a
	 * double free that lands in a magazine still duplicates the pointer --
	 * but it costs nothing and it turns the detectable case from freelist
	 * corruption into a counted refusal.
	 */
	if (s->free >= s->total) {
		__atomic_fetch_add(&stat_bad_free, 1, __ATOMIC_RELAXED);
		return;
	}

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

	/*
	 * The magazines are BSS, so a zeroed spinlock is already an unlocked
	 * ticket lock, but they are initialised like the cache locks anyway: a
	 * lock that is usable by accident is a lock whose invariant nobody
	 * wrote down.
	 */
	for (unsigned cpu = 0; cpu < MAX_CPUS; cpu++)
		for (size_t i = 0; i < NUM_KCACHE; i++)
			spinlock_init(&kmag_lock[cpu][i]);

	/*
	 * Last, and only here. Until this assignment the caches are described
	 * but not usable, and kmalloc() reports that by returning NULL.
	 */
	kmalloc_ready = true;
}

/* ----------------------------------------------------------------- kmalloc --- */

void *kmalloc(size_t size)
{
	struct kmalloc_magazine *m;
	struct kmalloc_cache *c;
	unsigned cpu;
	unsigned batch;
	unsigned n = 0;
	int idx;
	void *obj;
	void *objs[KMALLOC_MAG_REFILL];
	u64 flags;

	/*
	 * Before kmalloc_init(). This has to come first, and it has to be a
	 * plain test rather than a check of the cache geometry it is protecting:
	 * the per-CPU magazines are indexed by this_cpu_id(), which is not a
	 * meaningful number until percpu_setup() has run, and asking for one
	 * anyway is how a call made from a console initialiser turns into an
	 * out-of-bounds index instead of a NULL.
	 */
	if (!kmalloc_ready)
		return NULL;

	idx = class_for(size);
	if (idx < 0)
		return vmalloc(size);

	c = &kcaches[idx];
	cpu = this_cpu_id();

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

	/*
	 * Slow path: refill this CPU's magazine a slab's worth at a time.
	 *
	 * The batch is one slab's worth because that is what the cache has to
	 * hand out. Asking for more walks past the end of the current slab into
	 * cache_alloc's growth path, which creates a block and returns a single
	 * object from it -- so a refill that wants eight objects from a
	 * seven-object slab spends a whole second block to park one object in it.
	 */
	batch = c->per_slab < KMALLOC_MAG_REFILL ? c->per_slab : KMALLOC_MAG_REFILL;
	if (batch == 0)
		return NULL;		/* no geometry: nothing to refill from */

	flags = spinlock_irqsave(&c->lock);
	for (unsigned i = 0; i < batch; i++) {
		obj = cache_alloc(c);
		if (!obj)
			break;
		objs[i] = obj;
		n++;
	}
	/*
	 * Pushed in reverse, because mag_pop() takes the tail. The caller then
	 * receives objects in ascending address order, which is the order
	 * slab_new() built the free list in and the order this cache's own
	 * comment claims it preserves.
	 */
	while (n > 0)
		m->objects[m->count++] = objs[--n];
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
	struct kmalloc_cache *c = NULL;
	unsigned cpu;
	int idx;
	u64 flags;

	if (!ptr)
		return;

	/*
	 * A non-NULL free before kmalloc_init() is not a failure this file can
	 * absorb: nothing could have been allocated, so the pointer did not come
	 * from here and there is nowhere to return it to. Saying so beats
	 * returning it to whatever cache happens to match its size.
	 */
	if (!kmalloc_ready)
		panic("kfree: non-NULL free before kmalloc_init()");

	/*
	 * Which tier the pointer came from is decided by the pointer, not by
	 * `size`. vmalloc hands out page-aligned blocks (VMALLOC_AREA is aligned
	 * and every size is rounded up to PAGE_SIZE), while a slab object starts
	 * at obj_off past a page-aligned header and so is never page-aligned.
	 * One bit therefore separates the two, and it is a property of the
	 * allocator rather than of any caller's bookkeeping.
	 */
	if (!((uintptr_t)ptr & (PAGE_SIZE - 1))) {
		if (class_for(size) >= 0)
			__atomic_fetch_add(&stat_size_mismatch, 1,
					  __ATOMIC_RELAXED);
		vfree(ptr);
		return;
	}

	/*
	 * `size` picks the first cache to try. It is the caller's best guess and
	 * it is not trusted: the slab header has to agree before the block is
	 * accepted, so freeing a 4 KiB object with size=1 returns it to the 4 KiB
	 * slab rather than parking it in the 8-byte magazine, where it would be
	 * handed out a second time and the 4 KiB object's freelist link would be
	 * written over live data.
	 */
	idx = class_for(size);
	if (idx >= 0) {
		struct kmalloc_slab *s = slab_of(&kcaches[idx], ptr);

		if (slab_owns(&kcaches[idx], s, ptr))
			c = &kcaches[idx];
	}

	if (!c) {
		struct kmalloc_slab *s = slab_find_owner(ptr, &c);

		if (!s || !c) {
			/*
			 * No slab holds this pointer. Returning it to a cache
			 * anyway is what turns a bad free into a corrupt slab,
			 * so it is counted and dropped instead of guessed at.
			 */
			__atomic_fetch_add(&stat_bad_free, 1, __ATOMIC_RELAXED);
			return;
		}
		__atomic_fetch_add(&stat_size_mismatch, 1, __ATOMIC_RELAXED);
	}

	idx = (int)(c - kcaches);
	cpu = this_cpu_id();

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
	size_t n;
	char *p;

	if (!s)
		return NULL;

	n = strlen(s) + 1;
	p = kmalloc(n);
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
