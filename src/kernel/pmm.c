/*
 * pmm.c — buddy allocator for physical pages.
 *
 * A request for 2^order frames is served from a free list for that order; if
 * none is available, a larger block is split down. Freeing coalesces with the
 * buddy recursively, which is what keeps fragmentation from growing without
 * bound.
 *
 * Only block heads live on the free lists. Every other frame of a free order-N
 * block carries PG_TAIL and is reachable only through its head. Reserving must
 * walk the free lists, not the page array.
 */

#include <pmm.h>
#include <boot.h>
#include <klog.h>
#include <kstring.h>
#include <panic.h>
#include <vmm.h>
#include <io.h>
#include <cpu_features.h>

/*
 * BUDDY_MAX_ORDER is defined once, in pmm.h, and not here. struct pmm_zone
 * sizes free_list[] by it, so a second definition could drift from the first
 * without anything noticing. The allocator serves orders 0..BUDDY_MAX_ORDER
 * inclusive: order 11 is 2048 frames, i.e. 8 MiB, and anything larger is served
 * as a non-contiguous run by the vmalloc layer. (docs/src/memory/
 * buddy-allocator.md still says 11 levels, orders 0-10 and 4 MiB; the code is
 * the authority and that document is stale.)
 */

#define ZONE_DMA_END    0x01000000ull
#define ZONE_NORMAL_END 0x100000000ull

static struct pmm_zone zones[ZONE_COUNT];

/* Page metadata array. */
static struct page *page_array;

phys_addr_t pmm_total_pages;
phys_addr_t pmm_usable_pages;

phys_addr_t pmm_free_page_count;

struct pmm_stats pmm_stats;

KLOG_SUBSYSTEM("pmm");

/*
 * End of the kernel image, from the link script.
 *
 * The kernel is executing out of physical KERNEL_LANDING_ADDR when pmm_init()
 * runs, and the E820 map quite correctly describes that range as usable — it is
 * a statement about RAM, not about what this kernel is standing on. Nothing but
 * an explicit reservation keeps the allocator from handing out the ELF header
 * and then .text. Taking the bound from the link script rather than a
 * hard-coded constant means the reservation follows the image when it grows.
 *
 * The declaration is weak so a link script that stops defining it degrades to
 * "no self-reservation" rather than failing the build.
 */
extern char _ebss[] __attribute__((weak));

/* ------------------------------------------------------------- lookup ------ */

/*
 * The page array covers all physical memory from zero. Frames below the first
 * E820 entry, and any gap between entries, have no struct page allocated here;
 * they are covered by reserved bitmaps instead. phys_to_page() on such an
 * address is a bug in the caller, and the range check turns it into a panic
 * here rather than a wild write later.
 */
static inline bool phys_valid(phys_addr_t addr)
{
	return (addr >> PAGE_SHIFT) < pmm_total_pages;
}

struct page *phys_to_page(phys_addr_t addr)
{
	if (!phys_valid(addr))
		panic("phys_to_page(0x%llx) beyond %llu tracked frames\n",
		       (unsigned long long)addr,
		       (unsigned long long)pmm_total_pages);
	return &page_array[addr >> PAGE_SHIFT];
}

phys_addr_t page_to_phys(const struct page *page)
{
	return (phys_addr_t)(page - page_array) << PAGE_SHIFT;
}

/* ------------------------------------------------------------- zones ------- */

static zone_t zone_of(phys_addr_t addr)
{
	if (addr < ZONE_DMA_END)
		return ZONE_DMA;
	if (addr < ZONE_NORMAL_END)
		return ZONE_NORMAL;
	return ZONE_HIGH;
}

/* First physical address in `z`. */
static phys_addr_t zone_base(zone_t z)
{
	if (z == ZONE_DMA)
		return 0;
	if (z == ZONE_NORMAL)
		return ZONE_DMA_END;
	return ZONE_NORMAL_END;
}

/* First frame index past the end of `z`. */
static phys_addr_t zone_frame_limit(zone_t z)
{
	if (z == ZONE_DMA)
		return ZONE_DMA_END >> PAGE_SHIFT;
	if (z == ZONE_NORMAL)
		return ZONE_NORMAL_END >> PAGE_SHIFT;
	return pmm_total_pages;
}

/*
 * Track which frames exist at all.
 *
 * A bitmap indexed by frame number: one bit per 4 KiB page. The E820 map has
 * gaps, and a gap is memory that does not exist — handing it out would produce
 * a page fault against a device that is not there. One bit per frame costs
 * 1/32 of the frame count, which for a 4 GiB machine is 128 KiB.
 */
static u64 *frame_present;
static phys_addr_t present_words;

/* Frames that were present but are reserved by something. Same encoding. */

/*
 * Word counts, not pointers: these are the bounds the set/test helpers compare
 * a frame index against before dereferencing. A pointer-typed declaration would
 * compare a frame index to an address, which on any machine is a bound far
 * past the end of the bitmap and turns every out-of-range access into a wild
 * read instead of a clean rejection.
 */
static u64 *frame_reserved;

static bool frame_present_p(phys_addr_t addr)
{
	phys_addr_t idx = addr >> PAGE_SHIFT;

	if (idx / 64 >= present_words)
		return false;
	return (frame_present[idx / 64] >> (idx % 64)) & 1;
}

static void frame_set_present(phys_addr_t addr, bool present)
{
	phys_addr_t idx = addr >> PAGE_SHIFT;

	if (idx / 64 >= present_words)
		return;
	if (present)
		frame_present[idx / 64] |= 1ULL << (idx % 64);
	else
		frame_present[idx / 64] &= ~(1ULL << (idx % 64));
}

static bool frame_reserved_p(phys_addr_t addr)
{
	phys_addr_t idx = addr >> PAGE_SHIFT;

	if (idx / 64 >= present_words)
		return true;      /* unknown: treat as reserved */
	return (frame_reserved[idx / 64] >> (idx % 64)) & 1;
}

static void frame_set_reserved(phys_addr_t addr, bool reserved)
{
	phys_addr_t idx = addr >> PAGE_SHIFT;

	if (idx / 64 >= present_words)
		return;
	if (reserved)
		frame_reserved[idx / 64] |= 1ULL << (idx % 64);
	else
		frame_reserved[idx / 64] &= ~(1ULL << (idx % 64));
}

/*
 * End of the run of frames starting at `first` that are both present and
 * unreserved, bounded by `limit`.
 *
 * Called once per run during init, and always with `first` at or after the
 * previous run's end, so the whole free-list build is O(tracked frames) rather
 * than O(regions x frames). A reserved frame in the middle of a region — the
 * kernel image, say — ends the run, and the caller starts the next one, which is
 * what keeps a two-megabyte hole from turning a four-gigabyte region into two
 * million single-page blocks.
 */
static phys_addr_t usable_run_end(phys_addr_t first, phys_addr_t limit)
{
	phys_addr_t i = first;

	while (i < limit) {
		phys_addr_t addr = i << PAGE_SHIFT;

		if (!frame_present_p(addr) || frame_reserved_p(addr))
			break;
		i++;
	}
	return i;
}

/* ------------------------------------------------------- buddy core ------- */

/*
 * Mark the interior frames of a block that is going on a free list.
 *
 * Only the head is reachable through the lists; the rest must be visibly
 * something other than a head, or a scan that looks for free frames finds a
 * page whose "list" is two stale pointers into a neighbouring block. Used at
 * init, when blocks are pushed whole — splitting and coalescing maintain the
 * marking themselves, because every page they touch is a head they are already
 * rewriting.
 *
 * Caller holds the zone lock.
 */
static void block_mark_tail(struct page *page, unsigned order)
{
	phys_addr_t span = 1UL << order;

	for (phys_addr_t i = 1; i < span; i++)
		page[i].flags |= PG_TAIL;
}

/*
 * Insert a block at the head of its order's free list.
 *
 * The caller must hold the zone lock.
 */
static void buddy_free_locked(struct page *page, unsigned order)
{
	struct pmm_zone *z;

	if (page->zone >= ZONE_COUNT)
		panic("pmm: buddy_free on frame %llx with zone %u\n",
		       (unsigned long long)page_to_phys(page), page->zone);
	z = &zones[page->zone];

	/* A block head is never a tail and never still allocated. PG_PINNED and
	 * PG_ZEROED survive: both are facts about the frame rather than about
	 * its ownership. */
	page->flags = (page->flags & (PG_PINNED | PG_ZEROED)) | PG_FREE | PG_BUDDY;
	page->order = (uint8_t)order;
	page->refcount = 0;
	list_add(&page->list, &z->free_list[order]);
	z->free_bitmap |= (uint16_t)(1u << order);
	z->free_pages += 1UL << order;
}

/* Take a block off its free list. Caller holds the zone lock. */
static void buddy_remove_locked(struct pmm_zone *z, struct page *page,
			       unsigned order)
{
	list_del(&page->list);
	z->free_pages -= 1UL << order;
	if (list_empty(&z->free_list[order]))
		z->free_bitmap &= (uint16_t)~(1u << order);
}

/*
 * Remove a block from its free list.
 *
 * Caller holds the zone lock. PG_BUDDY and the order field are left intact:
 * the block is about to be allocated, but a partially-used block still needs
 * them for the later free.
 */
static struct page *buddy_alloc_locked(struct pmm_zone *z, unsigned order)
{
	struct list_head *head = &z->free_list[order];
	struct page *page;

	if (list_empty(head))
		return NULL;

	page = list_entry(head->next, struct page, list);
	buddy_remove_locked(z, page, order);

	/* PG_BUDDY stays set on the allocated block: it still is a block head,
	 * and `order` is still meaningful until the block is freed. zone_free_locked()
	 * tests PG_FREE before PG_BUDDY, so this does not make an allocated block
	 * eligible for coalescing. */
	page->flags = PG_ALLOCATED | PG_BUDDY;
	page->order = (uint8_t)order;
	page->refcount = 1;
	page->slab = NULL;
	return page;
}

/*
 * Find the smallest order >= `order` that has a free block.
 *
 * With the bitmap this is a masked find-first-set. BMI1's TZCNT defines its
 * result for an all-zero input (returning the operand width), so a miss yields
 * a value > BUDDY_MAX_ORDER and needs no undefined behaviour.
 */
static int first_order_at_least(const struct pmm_zone *z, unsigned order)
{
	uint32_t bitmap = z->free_bitmap;
	uint32_t mask;

	if (order > BUDDY_MAX_ORDER)
		return -1;

	mask = bitmap & (uint32_t)(~0u << order);
	if (mask == 0)
		return -1;

	return __builtin_ctz(mask);
}

/*
 * Allocate 2^order frames from a zone.
 *
 * Caller holds the zone lock. Splits the chosen block down to the requested
 * order, returning the lower half and returning the upper halves to the free
 * lists. Splitting is at most BUDDY_MAX_ORDER - order iterations.
 *
 * Accounting note: buddy_alloc_locked() has already subtracted the 2^have frames
 * of the block it removed, and each split has added back the 2^(cur-1) frames of
 * the half it returned, so the net effect of the whole function is exactly
 * -2^order and the callers must not subtract it a second time.
 */
static struct page *zone_alloc_locked(struct pmm_zone *z, unsigned order)
{
	int have = first_order_at_least(z, order);
	struct page *page;

	if (have < 0)
		return NULL;

	page = buddy_alloc_locked(z, (unsigned)have);

	/* Split down. Each split halves the block and frees the upper half,
	 * which is what preserves the buddy invariant that a block at order k
	 * is aligned to 2^k pages. */
	for (unsigned cur = (unsigned)have; cur > order; cur--) {
		struct page *buddy = page + (1UL << (cur - 1));

		buddy->zone = page->zone;
		buddy_free_locked(buddy, cur - 1);
		page->order = (uint8_t)(cur - 1);
		pmm_stats.splits++;
	}

	page->flags = PG_ALLOCATED | PG_BUDDY;
	page->order = (uint8_t)order;
	page->refcount = 1;
	page->slab = NULL;
	return page;
}

/*
 * The buddy of a block: same size, differing in exactly the `order` bit of the
 * frame index.
 */
static inline struct page *buddy_of(struct page *page, unsigned order)
{
	phys_addr_t phys = page_to_phys(page);
	phys_addr_t buddy_phys = phys ^ ((1UL << order) << PAGE_SHIFT);

	return &page_array[buddy_phys >> PAGE_SHIFT];
}

/*
 * Free a block, coalescing upward as far as the buddy is also free.
 *
 * Coalescing requires the buddy to be free, at the same order, and marked as a
 * buddy head. If any of those fail the merge would produce a block whose
 * interior pages still carry PG_FREE and a stale list link, so the checks are
 * mandatory rather than defensive.
 */
static void zone_free_locked(struct page *page, unsigned order)
{
	struct pmm_zone *z;

	if (page->zone >= ZONE_COUNT)
		panic("pmm: free of frame %llx with zone %u\n",
		       (unsigned long long)page_to_phys(page), page->zone);
	z = &zones[page->zone];

	for (;;) {
		struct page *buddy = buddy_of(page, order);

		/* Stop at the top order, and stop if the buddy belongs to a
		 * different zone (which happens at a zone boundary and means the
		 * address range is not really 2^(order+1)-aligned). */
		if (order >= BUDDY_MAX_ORDER)
			break;
		if (!phys_valid(page_to_phys(buddy)))
			break;
		if (buddy->zone != page->zone)
			break;
		if (!(buddy->flags & PG_FREE) || !(buddy->flags & PG_BUDDY))
			break;
		if (buddy->order != order)
			break;

		/* Remove the buddy from its free list: it is about to become
		 * part of a larger block. */
		buddy_remove_locked(z, buddy, order);

		/* Merge: the pair becomes one block of order+1 whose address is
		 * the lower of the two. The absorbed head is now an interior
		 * frame, so it must stop advertising itself as a free head —
		 * its list link is already stale. */
		if (buddy < page) {
			struct page *swap = page;

			page = buddy;
			buddy = swap;
		}
		buddy->flags = PG_TAIL;
		buddy->order = (uint8_t)(order + 1);
		page->order = (uint8_t)(order + 1);
		order++;
		pmm_stats.coalesces++;
	}

	buddy_free_locked(page, order);
}

/*
 * Exclude [lo, hi) from a zone's free lists.
 *
 * Only block heads are on the free lists, so the range cannot be carved out by
 * walking the page array: an interior frame has no head above it to find, and
 * `if (!(p->flags & PG_FREE)) continue;` skips it — which is why reserving the
 * low megabyte used to do nothing at all and why a reserve through the middle of
 * a large block used to leave that block allocatable whole. The lists have to be
 * walked instead.
 *
 * Two properties matter:
 *
 *   - The frames inside the range must not come back. They are re-introduced
 *     only when they lie outside it, so a coalesce cannot rebuild a block
 *     spanning the range: zone_free_locked() merges two blocks only when both
 *     are free at the same order, and a frame inside the range never is.
 *   - The survivors are handed back through zone_free_locked(), not through a
 *     bare buddy_free_locked(). Reserving a megabyte therefore costs one block,
 *     not 2048 order-0 blocks, and the high orders stay available — which
 *     matters on a machine where the only order-9 block is the one being carved.
 *
 * The walk is done in two passes because the second pass frees pages, and
 * freeing coalesces, and coalescing inserts into the very lists the first pass
 * is iterating.
 *
 * Caller holds the zone lock. Returns how many frames inside the range were on
 * a free list, i.e. how far the free count has to come down.
 */
static phys_addr_t exclude_free_blocks_locked(struct pmm_zone *z,
					     phys_addr_t lo, phys_addr_t hi)
{
	struct list_head blocks = LIST_HEAD_INIT(blocks);
	struct list_head survivors = LIST_HEAD_INIT(survivors);
	struct list_head *pos, *tmp;
	phys_addr_t claimed = 0;

	for (unsigned o = 0; o <= BUDDY_MAX_ORDER; o++) {
		/* Hand-rolled safe iteration: `tmp` is read before the body so
		 * the current element can be unlinked inside it. list_for_each_safe()
		 * would do the same, but its comma expression trips -Wunused-value. */
		pos = z->free_list[o].next;
		while (pos != &z->free_list[o]) {
			struct page *page;

			tmp = pos->next;
			page = list_entry(pos, struct page, list);

			{
				phys_addr_t base = page_to_phys(page);
				phys_addr_t end =
					base + ((1UL << o) << PAGE_SHIFT);

				if (end > lo && base < hi) {
					buddy_remove_locked(z, page, o);
					list_add(&page->list, &blocks);
				}
			}
			pos = tmp;
		}
	}

	pos = blocks.next;
	while (pos != &blocks) {
		struct page *page;
		phys_addr_t base, end;

		tmp = pos->next;
		page = list_entry(pos, struct page, list);
		base = page_to_phys(page);
		end = base + ((1UL << page->order) << PAGE_SHIFT);

		for (phys_addr_t q = base; q < end; q += PAGE_SIZE) {
			struct page *sq = &page_array[q >> PAGE_SHIFT];

			if (q >= lo && q < hi) {
				sq->flags = PG_ALLOCATED;
				sq->order = 0;
				sq->refcount = 1;
				sq->slab = NULL;
				claimed++;
				continue;
			}
			sq->zone = (uint8_t)zone_of(q);
			list_add(&sq->list, &survivors);
		}
		pos = tmp;
	}

	while (!list_empty(&survivors)) {
		struct page *p = list_entry(survivors.next, struct page, list);

		list_del_init(&p->list);
		zone_free_locked(p, 0);
	}

	return claimed;
}

/* ------------------------------------------------------------ public ------ */

struct page *pmm_alloc_pages(unsigned order, unsigned flags)
{
	unsigned start;

	if (order > BUDDY_MAX_ORDER)
		return NULL;

	start = (flags & GFP_DMA) ? ZONE_DMA : ZONE_NORMAL;

	/*
	 * Fall forward through the zones, but only ever to an index that exists.
	 * The old loop bounded the attempt count instead of the index, so a
	 * request starting at ZONE_NORMAL reached zones[3] and one starting at
	 * ZONE_HIGH reached zones[4]. Those slots are adjacent .bss, and
	 * zones[3].free_list[0].next aliases page_array — a non-NULL pointer —
	 * so the `if (!zone->free_list[0].next) continue;` guard could not
	 * catch it. The allocator then took a spinlock on whatever fields
	 * happened to be there and walked a free list built out of page_array.
	 */
	for (unsigned attempt = 0; attempt + start < ZONE_COUNT; attempt++) {
		struct pmm_zone *zone = &zones[start + attempt];
		struct page *page;
		u64 irq;

		/* free_bitmap, not free_list[0].next: list_init() points an
		 * empty list's next at its own head, so that test was never
		 * true and never meant anything. */
		if (!zone->free_bitmap)
			continue;

		irq = spinlock_irqsave(&zone->lock);
		page = zone_alloc_locked(zone, order);
		spinlock_unlock_irqrestore(&zone->lock, irq);

		if (page) {
			__atomic_fetch_add(&pmm_stats.allocs, 1, __ATOMIC_RELAXED);
			pmm_free_page_count -= (1UL << order);

			if (flags & GFP_ZERO) {
				/* Zero through the direct map. The page is
				 * allocated, so this cannot race with another
				 * CPU's view of it. */
				phys_addr_t phys = page_to_phys(page);
				phys_addr_t start_phys = ALIGN_UP(phys, PAGE_SIZE);
				phys_addr_t end_phys = ALIGN_DOWN(phys +
					(1UL << order) * PAGE_SIZE, PAGE_SIZE);

				for (phys_addr_t p = start_phys; p < end_phys;
				     p += PAGE_SIZE) {
					memset((void *)phys_to_virt(p), 0,
					       PAGE_SIZE);
					page_array[p >> PAGE_SHIFT].flags |=
						PG_ZEROED;
				}
			}
			return page;
		}
	}

	__atomic_fetch_add(&pmm_stats.failed, 1, __ATOMIC_RELAXED);
	return NULL;
}

void pmm_free_pages(struct page *page, unsigned order)
{
	struct pmm_zone *z;
	u64 irq;

	if (!page)
		return;

	/*
	 * Only a block head that this allocator handed out may be freed. An
	 * interior frame (PG_TAIL) has no valid position in the buddy scheme:
	 * returning one would thread its stale list link into a free list and the
	 * next allocation would hand out a frame that is still inside somebody
	 * else's block. A frame that is already PG_FREE is a double free. Both
	 * refusals are logged: the alternative is an allocator that corrupts
	 * itself silently, and a leak with a log line is strictly better than a
	 * corruption without one.
	 */
	if (!(page->flags & PG_ALLOCATED) ||
	    (page->flags & (PG_TAIL | PG_FREE))) {
		klog(KLOG_WARN,
		     "pmm: free of frame %llx refused: flags 0x%x order %u\n",
		     (unsigned long long)page_to_phys(page), page->flags,
		     page->order);
		return;
	}

	if (order > BUDDY_MAX_ORDER)
		panic("pmm: free of order %u\n", order);

	z = &zones[page->zone];

	/* An explicit free returns the frame to the pool, so a pin on it is
	 * over: nothing is left to protect. */
	page->flags &= ~(uint32_t)PG_PINNED;

	irq = spinlock_irqsave(&z->lock);
	zone_free_locked(page, order);
	spinlock_unlock_irqrestore(&z->lock, irq);

	__atomic_fetch_add(&pmm_stats.frees, 1, __ATOMIC_RELAXED);
	pmm_free_page_count += (1UL << order);
}

/*
 * Take a physical range out of circulation.
 *
 * `pin` distinguishes the two callers: pmm_reserve_range() marks hardware
 * reservations (kernel image, bootstrap page tables, the metadata arena, MMIO),
 * while pmm_pin_range() additionally sets PG_PINNED, which is the promise
 * pmm_pin_range() is documented to make and which used not to exist at all —
 * PG_PINNED was cleared again by the very next buddy_free_locked() on the way
 * back into a free list, because nothing removed a pinned frame from that list
 * in the first place.
 *
 * Both record the range in the reserved bitmap. That bitmap is what
 * pmm_alloc_dma_range() and the init-time free-list build consult, and it is
 * deliberately *not* what keeps the range out of the buddy allocator after init
 * — only the free-list exclusion below does that.
 */
static void claim_range(phys_addr_t base, phys_addr_t length, bool pin)
{
	phys_addr_t start, end;
	phys_addr_t claimed = 0;

	/*
	 * Validate the arithmetic before rounding. base + length was previously
	 * unchecked, so a wrapping range produced end < start, every loop below
	 * became a no-op, and the caller was told the reservation had happened.
	 */
	if (length == 0)
		return;
	if (base + length < base)      /* wrapped */
		return;

	start = ALIGN_DOWN(base, PAGE_SIZE);
	if (start + length < base)     /* the ALIGN_DOWN itself wrapped */
		return;

	end = ALIGN_UP(base + length, PAGE_SIZE);
	if (end < base + length)       /* the ALIGN_UP itself wrapped */
		return;

	for (phys_addr_t addr = start; addr < end; addr += PAGE_SIZE) {
		if (!phys_valid(addr))
			continue;       /* not tracked: no bitmap bit to set */
		frame_set_reserved(addr, true);
		page_array[addr >> PAGE_SHIFT].zone = (uint8_t)zone_of(addr);
	}

	/* Take the range out of the free lists. Before pmm_init() builds them
	 * this walks empty lists and costs nothing, which is what lets the
	 * kernel image and the metadata arena reserve themselves from inside
	 * pmm_init(). */
	for (int z = 0; z < ZONE_COUNT; z++) {
		phys_addr_t zbase = zone_base(z);
		phys_addr_t zlo = zone_frame_limit(z) << PAGE_SHIFT;
		u64 irq;

		if (start >= zlo)
			continue;       /* entirely above this zone */
		if (end <= zbase)
			continue;       /* entirely below this zone */

		irq = spinlock_irqsave(&zones[z].lock);
		claimed += exclude_free_blocks_locked(&zones[z],
						      MAX(start, zbase),
						      MIN(end, zlo));
		spinlock_unlock_irqrestore(&zones[z].lock, irq);
	}

	pmm_free_page_count -= claimed;

	/* Finally the flags. Frames inside the range are either frames we have
	 * just taken off a free list — allocated now, and claimed — or frames
	 * somebody already owns, whose order/refcount/slab we must not touch.
	 * PG_TAIL cannot survive here: every frame that carried it either had
	 * its block removed above or belonged to a block that is still on a
	 * free list, and such a block cannot contain a frame in the range. */
	for (phys_addr_t addr = start; addr < end; addr += PAGE_SIZE) {
		struct page *p;

		if (!phys_valid(addr))
			continue;
		p = &page_array[addr >> PAGE_SHIFT];
		if (p->flags & PG_ALLOCATED) {
			if (pin)
				p->flags |= PG_PINNED;
			continue;
		}
		p->flags = PG_ALLOCATED | (pin ? PG_PINNED : 0u);
		p->order = 0;
		p->refcount = 1;
		p->slab = NULL;
	}
}

void pmm_reserve_range(phys_addr_t base, phys_addr_t length)
{
	claim_range(base, length, false);
}

void pmm_pin_range(phys_addr_t base, phys_addr_t length)
{
	claim_range(base, length, true);
}

phys_addr_t pmm_total(void)
{
	return pmm_usable_pages;
}

phys_addr_t pmm_free(void)
{
	return pmm_free_page_count;
}

phys_addr_t pmm_zone_free(zone_t zone)
{
	if (zone >= ZONE_COUNT)
		return 0;
	return zones[zone].free_pages;
}

phys_addr_t pmm_zone_total(zone_t zone)
{
	if (zone >= ZONE_COUNT)
		return 0;
	return zones[zone].total_pages;
}

/*
 * Allocate a physically contiguous range for DMA.
 *
 * DMA regions cannot be satisfied by the buddy allocator in general, because a
 * device wants a specific range rather than any free block: the caller usually
 * needs alignment, and the range must survive until the device is programmed.
 * This walks the present bitmaps for a run of `length` consecutive frames that is
 * present, unreserved and aligned, marks it reserved through pmm_reserve_range()
 * — which now really does remove it from the free lists — and hands back both
 * the direct-map alias and the physical address.
 */
void *pmm_alloc_dma_range(size_t length, size_t alignment, phys_addr_t *out_phys)
{
	phys_addr_t total = pmm_total_pages;
	phys_addr_t frames, step, start;

	if (length == 0)
		return NULL;

	length = ALIGN_UP(length, PAGE_SIZE);
	if (length == 0)              /* rounded up past the address space */
		return NULL;
	frames = length >> PAGE_SHIFT;

	if (alignment < PAGE_SIZE)
		alignment = PAGE_SIZE;
	if (alignment & (alignment - 1)) {
		/* Not a power of two: the scan steps in units of alignment, so
		 * rounding up is the only way to honour it at all. Rounding down
		 * would return a range that is not aligned as asked for. */
		size_t rounded = PAGE_SIZE;

		while (rounded < alignment)
			rounded <<= 1;
		klog(KLOG_WARN,
		     "pmm: DMA alignment %zu is not a power of two, using %zu\n",
		     alignment, rounded);
		alignment = rounded;
	}
	step = alignment >> PAGE_SHIFT;

	for (start = 0; start + frames <= total; ) {
		phys_addr_t idx;

		/* Skip forward to the next aligned frame. */
		if (start % step) {
			start = ALIGN_UP(start, step);
			continue;
		}

		/*
		 * Every frame of the run has to be checked, including the one at
		 * `start` itself. The previous implementation cleared bit
		 * (idx % 64) unconditionally before testing — masking off the
		 * current position was written as a clear rather than as the
		 * "ignore the bits below us" mask it was meant to be, so a run
		 * that began at `start` could never match — and then jumped to
		 * the next word after consuming a bit, so a run straddling a word
		 * boundary was never accumulated. It also returned `start` on
		 * `run + free_run >= frames` without ever having verified the
		 * frames below the first set bit.
		 */
		idx = start;
		while (idx < start + frames) {
			phys_addr_t addr = idx << PAGE_SHIFT;

			if (!frame_present_p(addr) || frame_reserved_p(addr))
				break;
			idx++;
		}

		if (idx == start + frames) {
			phys_addr_t phys = start << PAGE_SHIFT;

			pmm_reserve_range(phys, length);
			if (out_phys)
				*out_phys = phys;
			return (void *)phys_to_virt(phys);
		}

		/* The run broke. Resume from the frame that failed rather than
		 * from the next aligned boundary, or a device asking for two
		 * pages would rescan the entire gap for every alignment slot in
		 * it. */
		start = idx + 1;
	}

	klog(KLOG_WARN, "pmm: no %zu byte contiguous DMA range\n", length);
	return NULL;
}

/* ------------------------------------------------------------- init -------- */

/*
 * The kernel's own image.
 *
 * pmm_init() runs after the paging switch, so the kernel is running from
 * physical KERNEL_LANDING_ADDR + offset while the E820 map cheerfully reports
 * that range as usable. Reserving it from inside the allocator, against the
 * link-time end of the image, means the first pmm_alloc_page() cannot return
 * the ELF header. The bootstrap page tables at BOOT_PT_ADDR are stage2's and
 * live in stage2's header, so the caller reserves those; see the note in
 * pmm_init().
 */
static void reserve_kernel_image(void)
{
	phys_addr_t virt, phys_end;

	if (!_ebss)
		return;         /* no link-time symbol: nothing to derive a bound */

	virt = (phys_addr_t)(uintptr_t)_ebss;
	if (virt < KERNEL_VIRT_BASE || virt <= KERNEL_LANDING_ADDR)
		return;

	phys_end = kernel_virt_to_phys(virt);
	klog(KLOG_INFO, "pmm: reserving kernel image %llx-%llx\n",
	     (unsigned long long)KERNEL_LANDING_ADDR,
	     (unsigned long long)phys_end);
	pmm_reserve_range(KERNEL_LANDING_ADDR, phys_end - KERNEL_LANDING_ADDR);
}

/*
 * Largest usable run that can hold `size`, reported as [lo, hi).
 *
 * "Usable" means E820_USABLE with a non-zero, non-wrapping length, so a
 * malformed entry cannot contribute a run that is not there. Returns false if
 * no run is big enough, which is the caller's cue to shrink what it is trying
 * to place rather than to place it somewhere that does not exist.
 */
static bool meta_find_run(const struct e820_entry *map, uint32_t count,
			  phys_addr_t size, phys_addr_t *out_lo, phys_addr_t *out_hi)
{
	phys_addr_t best_len = 0;

	*out_lo = 0;
	*out_hi = 0;

	for (uint32_t i = 0; i < count; i++) {
		phys_addr_t base = map[i].base;
		phys_addr_t length = map[i].length;
		phys_addr_t lo, hi;

		if (map[i].type != E820_USABLE || length == 0)
			continue;
		if (base + length < base)
			continue;
		lo = ALIGN_UP(base, PAGE_SIZE);
		hi = ALIGN_DOWN(base + length, PAGE_SIZE);
		if (hi <= lo || hi - lo < size)
			continue;
		if (hi - lo > best_len) {
			best_len = hi - lo;
			*out_lo = lo;
			*out_hi = hi;
		}
	}
	return best_len != 0;
}

/*
 * Initialise from the E820 map.
 *
 * The order of operations matters:
 *   1. Find the highest address reported as usable; that bounds the frame count
 *      and therefore the size of every metadata array.
 *   2. Place the metadata arrays in the direct map, which at this point is
 *      whatever stage2's identity map provided. They have to be somewhere
 *      before the buddy allocator can allocate anything.
 *   3. Mark every frame as reserved by default.
 *   4. Walk the E820 map, marking usable frames present and unreserved.
 *   5. Reserve the metadata arena and the kernel image. Both have to be in the
 *      bitmaps *before* step 6, which is what makes step 6 skip them in bulk
 *      instead of inserting them and hoping a later reserve finds them.
 *   6. Hand the usable, unreserved frames to the buddy allocator.
 *
 * The caller is responsible for reserving the rest of the bootloader's footprint
 * — the low megabyte, and stage2's bootstrap page tables at BOOT_PT_ADDR.
 */
void pmm_init(phys_addr_t e820_phys, uint32_t e820_count)
{
	/*
	 * Through the direct map, not as a bare address. pmm_init() runs after
	 * vmm_switch_to_kernel_pgd(), which replaced the bootloader's tables, and
	 * the new PML4 deliberately has no low identity window: physical memory is
	 * reachable at PHYS_DIRECT_MAP + phys and nowhere else. Casting the
	 * physical address straight to a pointer faults on the first read.
	 */
	const struct e820_entry *map =
		(const struct e820_entry *)phys_to_virt(e820_phys);
	phys_addr_t highest = 0;
	phys_addr_t bitmap_bytes, page_array_bytes;
	phys_addr_t meta_bytes;
	phys_addr_t run_lo = 0, run_hi = 0;
	phys_addr_t pa_lo = 0, pa_hi = 0;
	phys_addr_t pr_lo = 0, pr_hi = 0;
	phys_addr_t rs_lo = 0, rs_hi = 0;
	bool one_run;
	phys_addr_t usable = 0, added = 0;
	u64 *present_map, *reserved_map;
	struct page *page_map;
	phys_addr_t carve;

	/* 1. Bound the address space. Zero-length and wrapping entries are
	 * dropped here rather than being allowed to contribute a bogus bound:
	 * base + length is unsigned and a wrapping entry reads as a small
	 * number, and nothing downstream would notice. */
	for (uint32_t i = 0; i < e820_count; i++) {
		phys_addr_t base = map[i].base;
		phys_addr_t length = map[i].length;

		if (map[i].type != E820_USABLE)
			continue;
		if (length == 0)
			continue;
		if (base + length < base)
			continue;
		if (base + length > highest)
			highest = base + length;
	}

	if (highest == 0)
		panic("pmm: no usable memory reported by E820\n");

	pmm_total_pages = highest >> PAGE_SHIFT;

	/*
	 * 2. Metadata. Every array is placed inside a run the firmware reported
	 * as usable, at the top of that run, because the bottom of memory is
	 * where the bootloader structures live and the top is the least
	 * contended. The frames the arrays occupy are reserved in step 5.
	 *
	 * vmalloc_raw() is deliberately not used for any of this. It hands back
	 * non-contiguous VMALLOC_AREA addresses whose backing frames pmm cannot
	 * recover, and whose PTEs at audit time carried the *virtual* address as
	 * the frame — 0xFFFFC00000000000 masked down to 0x000FC00000000000,
	 * about 4 PiB, with no RAM behind it. Metadata written there is written
	 * nowhere, and the allocator then reads its own bitmaps back out of
	 * whatever RAM happens to occupy that address. So instead of falling
	 * back to vmalloc, the tracked frame bound is *shrunk* until the page
	 * array fits in a run that really exists. Frames above the bound are
	 * wasted, which is a recoverable and visible loss; an allocator whose own
	 * state is fictional is neither.
	 *
	 * Only two bitmaps are kept. A third, one per zone, used to be allocated
	 * and written for every frame and read by nothing: the zones come from
	 * the address, and the allocator's bookkeeping is the free lists. On a
	 * 4 GiB machine it cost 384 KiB and a -Wunused-function warning.
	 */
	bitmap_bytes = ALIGN_UP((pmm_total_pages / 64 + 1) * sizeof(u64),
				PAGE_SIZE);
	page_array_bytes = ALIGN_UP(pmm_total_pages * sizeof(struct page),
				    PAGE_SIZE);
	meta_bytes = bitmap_bytes * 2 + page_array_bytes;

	one_run = meta_find_run(map, e820_count, meta_bytes, &run_lo, &run_hi);

	while (!one_run) {
		/*
		 * Nothing holds all of it. Shrink the bound rather than place the
		 * page array in memory that may not be there. Each pass strictly
		 * lowers pmm_total_pages, so this terminates.
		 */
		if (!meta_find_run(map, e820_count, page_array_bytes,
				   &run_lo, &run_hi))
			break;              /* no run can hold the page array */
		if ((run_hi >> PAGE_SHIFT) >= pmm_total_pages)
			break;              /* already bounded by that run: no progress */

		pmm_total_pages = run_hi >> PAGE_SHIFT;
		bitmap_bytes = ALIGN_UP((pmm_total_pages / 64 + 1) * sizeof(u64),
					PAGE_SIZE);
		page_array_bytes = ALIGN_UP(pmm_total_pages * sizeof(struct page),
					    PAGE_SIZE);
		meta_bytes = bitmap_bytes * 2 + page_array_bytes;
		one_run = meta_find_run(map, e820_count, meta_bytes,
					&run_lo, &run_hi);
	}

	if (one_run) {
		carve = run_hi;

		carve -= page_array_bytes;
		pa_lo = carve;
		page_map = (struct page *)(uintptr_t)phys_to_virt(pa_lo);
		pa_hi = carve + page_array_bytes;
		memset(page_map, 0, page_array_bytes);

		carve -= bitmap_bytes;
		rs_lo = carve;
		reserved_map = (u64 *)(uintptr_t)phys_to_virt(rs_lo);
		rs_hi = carve + bitmap_bytes;
		memset(reserved_map, 0, bitmap_bytes);

		carve -= bitmap_bytes;
		pr_lo = carve;
		present_map = (u64 *)(uintptr_t)phys_to_virt(pr_lo);
		pr_hi = carve + bitmap_bytes;
		memset(present_map, 0, bitmap_bytes);
	} else {
		/*
		 * The page array has to stand on its own, so put it in the
		 * largest run that holds it and the bitmaps in another. Both are
		 * still real frames from the E820 map.
		 */
		if (!meta_find_run(map, e820_count, page_array_bytes,
				   &run_lo, &run_hi))
			panic("pmm: no usable E820 run holds the %llu byte page array\n",
			       (unsigned long long)page_array_bytes);

		pa_hi = run_hi;
		pa_lo = run_hi - page_array_bytes;
		page_map = (struct page *)(uintptr_t)phys_to_virt(pa_lo);
		memset(page_map, 0, page_array_bytes);

		/* Prefer the tail of the same run for the bitmaps; fall back to
		 * whatever other run is largest. */
		if (pa_lo - bitmap_bytes * 2 >= run_lo)
			carve = pa_lo;
		else if (!meta_find_run(map, e820_count, bitmap_bytes * 2,
					&run_lo, &run_hi))
			panic("pmm: no usable E820 run holds the %llu byte bitmaps\n",
			       (unsigned long long)(bitmap_bytes * 2));
		else
			carve = run_hi;

		carve -= bitmap_bytes;
		rs_lo = carve;
		reserved_map = (u64 *)(uintptr_t)phys_to_virt(rs_lo);
		rs_hi = carve + bitmap_bytes;
		memset(reserved_map, 0, bitmap_bytes);

		carve -= bitmap_bytes;
		pr_lo = carve;
		present_map = (u64 *)(uintptr_t)phys_to_virt(pr_lo);
		pr_hi = carve + bitmap_bytes;
		memset(present_map, 0, bitmap_bytes);

		klog(KLOG_WARN,
		     "pmm: metadata split across runs: page array %llx-%llx, bitmaps %llx-%llx\n",
		     (unsigned long long)pa_lo, (unsigned long long)pa_hi,
		     (unsigned long long)pr_lo, (unsigned long long)rs_hi);
	}


	frame_present = present_map;
	frame_reserved = reserved_map;
	page_array = page_map;
	present_words = (pmm_total_pages / 64) + 1;

	/* 3. Default: everything is reserved. */
	memset(frame_present, 0, (size_t)present_words * sizeof(u64));
	memset(frame_reserved, 0xFF, (size_t)present_words * sizeof(u64));

	for (uint32_t i = 0; i < e820_count; i++) {
		phys_addr_t base = map[i].base;
		phys_addr_t length = map[i].length;
		phys_addr_t lo, hi, addr;

		if (map[i].type != E820_USABLE || length == 0)
			continue;
		if (base + length < base)
			continue;
		lo = ALIGN_UP(base, PAGE_SIZE);
		hi = ALIGN_DOWN(base + length, PAGE_SIZE);

		for (addr = lo; addr < hi; addr += PAGE_SIZE) {
			phys_addr_t idx = addr >> PAGE_SHIFT;

			if (idx >= pmm_total_pages)
				break;
			frame_set_present(addr, true);
			frame_set_reserved(addr, false);
			/* Every frame's zone is set here, once, over the whole
			 * address range. buddy_free_locked() selects its list
			 * from page->zone and zone_free_locked() locks
			 * page->zone, so a frame whose zone was left at the
			 * memset value of 0 puts every block on the machine into
			 * ZONE_DMA while the accounting is credited to the right
			 * zone — which is exactly what pmm_dump reported:
			 * 1048479 free pages under "dma", zero under "normal"
			 * and "high". */
			page_array[idx].zone = (uint8_t)zone_of(addr);
		}
	}

	/*
	 * Zone state. This has to happen before step 5, not after it: step 5
	 * goes through pmm_reserve_range(), which walks each zone's free lists,
	 * and an uninitialised free_list[] is a zeroed struct list_head whose
	 * next is NULL. list_empty() then answers false — NULL is not the head —
	 * so the walk believes the list is populated and dereferences NULL.
	 */
	memset(zones, 0, sizeof(zones));
	static const char *const zone_names[ZONE_COUNT] = {
		"dma", "normal", "high"
	};

	for (int z = 0; z < ZONE_COUNT; z++) {
		struct pmm_zone *zone = &zones[z];

		zone->name = zone_names[z];
		zone->base = zone_base(z);
		zone->end = MIN(zone_frame_limit(z) << PAGE_SHIFT,
				pmm_total_pages << PAGE_SHIFT);
		zone->free_pages = 0;
		zone->total_pages = 0;
		zone->free_bitmap = 0;
		spinlock_init(&zone->lock);
		for (unsigned o = 0; o <= BUDDY_MAX_ORDER; o++)
			list_init(&zone->free_list[o]);
	}

	/*
	 * 5. Reserve the metadata, then the kernel image.
	 *
	 * This is the step whose absence was audit #29: the arena was computed
	 * and written through but never reserved, and because blocks are pushed
	 * at the list head the allocator's own page array sat at the head of a
	 * free list. The first caller was handed the allocator's bookkeeping and
	 * memset over it.
	 *
	 * All three ranges are reserved, not just the page array: in the split
	 * layout the bitmaps live somewhere else entirely.
	 */
	if (pa_hi > pa_lo)
		pmm_reserve_range(pa_lo, pa_hi - pa_lo);
	if (pr_hi > pr_lo)
		pmm_reserve_range(pr_lo, pr_hi - pr_lo);
	if (rs_hi > rs_lo)
		pmm_reserve_range(rs_lo, rs_hi - rs_lo);
	reserve_kernel_image();

	/*
	 * And confirm the placement was real. If a metadata frame is not in
	 * RAM the firmware reported, the allocator is about to read its own
	 * state back out of memory it does not own, and every number it prints
	 * will be meaningless. Say so here, where the addresses are known,
	 * rather than three screens later in unexplained numbers.
	 */
	{
		const phys_addr_t probe[3][2] = {
			{ pa_lo, pa_hi }, { pr_lo, pr_hi }, { rs_lo, rs_hi },
		};
		const char *const what[3] = {
			"page array", "present bitmap", "reserved bitmap",
		};

		for (int i = 0; i < 3; i++) {
			for (phys_addr_t a = probe[i][0]; a < probe[i][1];
			     a += PAGE_SIZE) {
				if (!frame_present_p(a)) {
					panic("pmm: %s frame %llx is not in any usable E820 range\n",
					      what[i], (unsigned long long)a);
					break;
				}
				if (!frame_reserved_p(a))
					panic("pmm: %s frame %llx was not reserved\n",
					      what[i], (unsigned long long)a);
			}
		}
	}

	/* 5. Reserve the metadata, then the kernel image. */

	/*
	 * Regions are handed over in ascending address order and blocks within a
	 * region are cut at the largest order that is aligned at the current
	 * frame and fits inside both the run of usable frames and the zone, which
	 * is what makes the buddy invariant hold: a block at order k is 2^k
	 * aligned, so its buddy is a valid block at the same order.
	 *
	 * The run is measured first, in bulk, so a reservation in the middle of a
	 * region costs one alignment step on each side of it rather than turning
	 * everything after it into single pages. Without that, reserving the
	 * kernel image would leave the whole remaining 3 GiB as order-0 blocks
	 * and no order-9 allocation could ever succeed.
	 */
	for (uint32_t i = 0; i < e820_count; i++) {
		phys_addr_t base = map[i].base;
		phys_addr_t length = map[i].length;
		phys_addr_t lo, hi;

		if (map[i].type != E820_USABLE || length == 0)
			continue;
		if (base + length < base)
			continue;
		lo = ALIGN_UP(base, PAGE_SIZE) >> PAGE_SHIFT;
		hi = ALIGN_DOWN(base + length, PAGE_SIZE) >> PAGE_SHIFT;

		while (lo < hi) {
			phys_addr_t run = usable_run_end(lo, hi);

			if (run == lo) {
				lo++;
				continue;
			}

			while (lo < run) {
				zone_t z = zone_of(lo << PAGE_SHIFT);
				phys_addr_t zone_hi =
					MIN(run, zone_frame_limit(z));
				struct page *p;
				unsigned order;

				/* Largest order aligned at this frame that fits
				 * inside both the run and the zone. One block
				 * at a time: taking the whole run at order 0
				 * when the first frame is misaligned would
				 * emit thousands of single pages where a
				 * handful of doubling blocks would do, and
				 * order 9+ would stop being satisfiable. */
				order = BUDDY_MAX_ORDER;
				while (order > 0 &&
				       (((lo & ((1UL << order) - 1)) != 0) ||
					(lo + (1UL << order) > zone_hi)))
					order--;

				p = &page_array[lo];
				buddy_free_locked(p, order);
				block_mark_tail(p, order);
				added += 1UL << order;
				lo += 1UL << order;
			}
			lo = run;
		}
	}

	/*
	 * Zone totals and the usable count. total_pages is the number of frames
	 * the zone *manages* — present and unreserved — not the number that
	 * happened to be free when init finished. Assigning the free count left
	 * pmm_zone_total() returning a free count and pmm_dump printing "X/X
	 * pages free" for every zone, with base and end left at 0 so nothing
	 * could say which addresses a zone covered.
	 */
	for (int z = 0; z < ZONE_COUNT; z++) {
		struct pmm_zone *zone = &zones[z];
		phys_addr_t lo = zone->base >> PAGE_SHIFT;
		phys_addr_t hi = zone->end >> PAGE_SHIFT;
		phys_addr_t count = 0;

		for (phys_addr_t idx = lo; idx < hi && idx < pmm_total_pages;
		     idx++) {
			phys_addr_t addr = idx << PAGE_SHIFT;

			if (frame_present_p(addr) && !frame_reserved_p(addr))
				count++;
		}
		zone->total_pages = count;
		usable += count;
	}

	pmm_usable_pages = usable;
	pmm_free_page_count = added;

	klog(KLOG_INFO, "pmm: %llu usable frames (%llu MiB) of %llu tracked (%llu MiB)\n",
	     (unsigned long long)usable,
	     (unsigned long long)(usable * PAGE_SIZE >> 20),
	     (unsigned long long)pmm_total_pages,
	     (unsigned long long)(pmm_total_pages * PAGE_SIZE >> 20));
	for (int z = 0; z < ZONE_COUNT; z++)
		klog(KLOG_INFO, "pmm: zone %-6s %llx-%llx %llu/%llu pages free\n",
		     zones[z].name,
		     (unsigned long long)zones[z].base,
		     (unsigned long long)zones[z].end,
		     (unsigned long long)zones[z].free_pages,
		     (unsigned long long)zones[z].total_pages);

	klog(KLOG_INFO,
	     "pmm: metadata bitmaps %zu KiB, page array %zu KiB, reserved %llx-%llx %llx-%llx %llx-%llx\n",
	     (size_t)(present_words * 2 * sizeof(u64)) >> 10,
	     (size_t)(pmm_total_pages * sizeof(struct page)) >> 10,
	     (unsigned long long)pr_lo, (unsigned long long)pr_hi,
	     (unsigned long long)rs_lo, (unsigned long long)rs_hi,
	     (unsigned long long)pa_lo, (unsigned long long)pa_hi);
}

void pmm_dump(void)
{
	klog(KLOG_INFO, "pmm: %llu/%llu frames free (%llu MiB of %llu MiB)\n",
	     (unsigned long long)pmm_free_page_count,
	     (unsigned long long)pmm_usable_pages,
	     (unsigned long long)(pmm_free_page_count * PAGE_SIZE >> 20),
	     (unsigned long long)(pmm_usable_pages * PAGE_SIZE >> 20));
	for (int z = 0; z < ZONE_COUNT; z++)
		klog(KLOG_INFO, "pmm:   %-6s %llx-%llx %llu/%llu pages free\n",
		     zones[z].name,
		     (unsigned long long)zones[z].base,
		     (unsigned long long)zones[z].end,
		     (unsigned long long)zones[z].free_pages,
		     (unsigned long long)zones[z].total_pages);
	klog(KLOG_INFO, "pmm: %llu allocs, %llu frees, %llu splits, %llu coalesces, %llu failures\n",
	     (unsigned long long)pmm_stats.allocs,
	     (unsigned long long)pmm_stats.frees,
	     (unsigned long long)pmm_stats.splits,
	     (unsigned long long)pmm_stats.coalesces,
	     (unsigned long long)pmm_stats.failed);
}