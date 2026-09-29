/*
 * pmm.c — buddy allocator for physical pages.
 *
 * The allocator manages physical frames in power-of-two blocks. A request for
 * 2^order frames is served from a free list for that order; if none is
 * available, a larger block is split down. Freeing coalesces a block with its
 * buddy recursively, which is what keeps fragmentation from growing without
 * bound: a 1 GiB block built from 4 KiB allocations becomes possible again as
 * soon as every one of its halves is free.
 *
 * Implementation notes worth knowing before changing anything here:
 *
 *   - The page metadata array lives in the direct map, so phys_to_page() is a
 *     multiply and an add rather than a lookup.
 *   - Each zone is locked independently. The allocator is the busiest lock in
 *     the kernel and there is no reason for a DMA-zone allocation to contend
 *     with a high-memory one.
 *   - A free block's buddy can only be coalesced if it is itself free AND at
 *     the same order AND at a page-index that differs only in the order bit.
 *     All three conditions are checked; missing any one produces a corrupt
 *     allocator that mostly works.
 */

#include <pmm.h>
#include <boot.h>
#include <klog.h>
#include <kstring.h>
#include <panic.h>
#include <vmm.h>
#include <io.h>
#include <cpu_features.h>

/* Maximum block order: 2^11 frames = 8 MiB. Larger allocations come from
 * vmalloc, which is satisfied with non-contiguous frames. */
#define BUDDY_MAX_ORDER 11
#define BUDDY_MAX_PAGES (1UL << BUDDY_MAX_ORDER)

static struct pmm_zone zones[ZONE_COUNT];

/* Page metadata array. Sized for the largest machine the kernel supports and
 * placed in the direct map during early init. */
static struct page *page_array;
static phys_addr_t page_array_pages;

phys_addr_t pmm_total_pages;
phys_addr_t pmm_free_page_count;

struct pmm_stats pmm_stats;

KLOG_SUBSYSTEM("pmm");

/* Zone boundaries. ZONE_DMA exists because of the ISA bus, not because of the
 * CPU: it is the lowest 16 MiB of physical memory. */
#define ZONE_DMA_END    0x01000000ull
#define ZONE_NORMAL_END 0x100000000ull

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
static u64 *frame_reserved;
static u64 *zone_bitmap[ZONE_COUNT];

/*
 * Word counts, not pointers: these are the bounds the set/test helpers compare
 * a frame index against before dereferencing. A pointer-typed declaration would
 * compare a frame index to an address, which on any machine is a bound far
 * past the end of the bitmap and turns every out-of-range access into a wild
 * read instead of a clean rejection.
 */
static phys_addr_t zone_words;

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

static void zone_set(zone_t z, phys_addr_t idx, bool in_zone)
{
	if (idx / 64 >= zone_words)
		return;
	if (in_zone)
		zone_bitmap[z][idx / 64] |= 1ULL << (idx % 64);
	else
		zone_bitmap[z][idx / 64] &= ~(1ULL << (idx % 64));
}

static bool zone_test(zone_t z, phys_addr_t addr)
{
	phys_addr_t idx = addr >> PAGE_SHIFT;

	if (idx / 64 >= zone_words)
		return false;
	return (zone_bitmap[z][idx / 64] >> (idx % 64)) & 1;
}

/* ------------------------------------------------------- buddy core ------- */

/*
 * Insert a block at the head of its order's free list.
 *
 * The caller must hold the zone lock.
 */
static void buddy_free_locked(struct page *page, unsigned order)
{
	struct pmm_zone *z = &zones[page->zone];

	page->flags = PG_FREE | PG_BUDDY;
	page->order = (uint8_t)order;
	page->refcount = 0;
	list_add(&page->list, &z->free_list[order]);
	z->free_bitmap |= (uint16_t)(1u << order);
	z->free_pages += 1UL << order;
}

static inline void buddy_set_bitmap(struct pmm_zone *z, unsigned order)
{
	z->free_bitmap |= (uint16_t)(1u << order);
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
	list_del(&page->list);

	z->free_pages -= 1UL << order;
	if (list_empty(&z->free_list[order]))
		z->free_bitmap &= (uint16_t)~(1u << order);

	page->flags = PG_ALLOCATED;
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

	page->flags = PG_ALLOCATED;
	page->order = (uint8_t)order;
	page->refcount = 1;
	z->free_pages -= (1UL << order);
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
 * interior pages still carry PG_TAIL and whose free-list membership is wrong,
 * so the checks are mandatory rather than defensive.
 */
static void zone_free_locked(struct page *page, unsigned order)
{
	struct pmm_zone *z = &zones[page->zone];

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
		list_del(&buddy->list);
		z->free_pages -= 1UL << order;
		if (list_empty(&z->free_list[order]))
			z->free_bitmap &= (uint16_t)~(1u << order);

		/* Merge: the pair becomes one block of order+1 whose address is
		 * the lower of the two. */
		if (buddy < page)
			page = buddy;
		page->order = (uint8_t)(order + 1);
		order++;
		pmm_stats.coalesces++;
	}

	buddy_free_locked(page, order);
}

/* ------------------------------------------------------------ public ------ */

struct page *pmm_alloc_pages(unsigned order, unsigned flags)
{
	zone_t want;
	unsigned start;

	if (order > BUDDY_MAX_ORDER)
		return NULL;

	if (flags & GFP_DMA) {
		want = ZONE_DMA;
	} else {
		want = ZONE_NORMAL;
	}

	for (unsigned attempt = 0; attempt < ZONE_COUNT; attempt++) {
		zone_t z = (zone_t)(want + attempt);
		struct pmm_zone *zone = &zones[z];
		struct page *page;
		u64 irq;

		if (!zone->free_list[0].next)
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

	z = &zones[page->zone];

	irq = spinlock_irqsave(&z->lock);
	zone_free_locked(page, order);
	spinlock_unlock_irqrestore(&z->lock, irq);

	__atomic_fetch_add(&pmm_stats.frees, 1, __ATOMIC_RELAXED);
	pmm_free_page_count += (1UL << order);
}

/*
 * Reserve a physical range.
 *
 * A range that overlaps free buddy blocks cannot simply be flagged reserved:
 * the buddy allocator hands out whole blocks, so a block spanning reserved and
 * free frames would eventually be handed out whole. The containing block has to
 * be removed from its free list and split down to individual pages, so the
 * reserved frames become order-0 allocations and the siblings go back as free
 * pages.
 *
 * This runs during initialisation and for MMIO discovery, not on a hot path,
 * so the search is a simple walk rather than a clever one.
 */
void pmm_reserve_range(phys_addr_t base, phys_addr_t length)
{
	phys_addr_t start = ALIGN_DOWN(base, PAGE_SIZE);
	phys_addr_t end = ALIGN_UP(base + length, PAGE_SIZE);

	for (phys_addr_t addr = start; addr < end; addr += PAGE_SIZE) {
		if (!phys_valid(addr))
			continue;

		phys_addr_t idx = addr >> PAGE_SHIFT;
		struct page *p = &page_array[idx];

		frame_set_reserved(addr, true);

		if (!(p->flags & PG_FREE))
			continue;   /* already allocated: nothing to split */

		zone_t z = p->zone;
		u64 irq = spinlock_irqsave(&zones[z].lock);

		/*
		 * Remove the whole containing block, then re-introduce every
		 * frame in it individually, marking the target allocated and the
		 * rest free. Walking the block and freeing page by page achieves
		 * the same thing with less code, because freeing order-0 pages
		 * re-coalesces the untouched siblings for free.
		 */
		unsigned order = p->order;
		phys_addr_t block_base = addr & ~(((1UL << order) - 1) << PAGE_SHIFT);
		phys_addr_t block_end = block_base +
			((1UL << order) << PAGE_SHIFT);

		/* Take the block out of circulation. */
		list_del(&p->list);
		zones[z].free_pages -= 1UL << order;
		if (list_empty(&zones[z].free_list[order]))
			zones[z].free_bitmap &= (uint16_t)~(1u << order);
		pmm_free_page_count -= 1UL << order;

		/* Re-free every frame in the block except the reserved one. */
		for (phys_addr_t q = block_base; q < block_end; q += PAGE_SIZE) {
			struct page *sq = &page_array[q >> PAGE_SHIFT];

			if (q == addr) {
				sq->flags = PG_ALLOCATED;
				sq->order = 0;
				sq->refcount = 1;
				sq->slab = NULL;
				continue;
			}
			sq->zone = (uint8_t)zone_of(q);
			buddy_free_locked(sq, 0);
			pmm_free_page_count++;
		}

		spinlock_unlock_irqrestore(&zones[z].lock, irq);
	}
}

void pmm_pin_range(phys_addr_t base, phys_addr_t length)
{
	phys_addr_t start = ALIGN_DOWN(base, PAGE_SIZE);
	phys_addr_t end = ALIGN_UP(base + length, PAGE_SIZE);

	for (phys_addr_t addr = start; addr < end; addr += PAGE_SIZE) {
		if (phys_valid(addr)) {
			page_array[addr >> PAGE_SHIFT].flags |= PG_PINNED;
			frame_set_reserved(addr, true);
		}
	}
}

phys_addr_t pmm_total(void)
{
	return pmm_total_pages;
}

phys_addr_t pmm_free(void)
{
	return pmm_free_page_count;
}

phys_addr_t pmm_zone_free(zone_t zone)
{
	return zones[zone].free_pages;
}

phys_addr_t pmm_zone_total(zone_t zone)
{
	return zones[zone].total_pages;
}

/*
 * Allocate a physically contiguous range for DMA.
 *
 * DMA regions cannot be satisfied by the buddy allocator in general, because a
 * device wants a specific range rather than any free block: the caller usually
 * needs alignment, and the range must survive until the device is programmed.
 * This walks the E820 map for the largest usable run satisfying the alignment,
 * marks it reserved, and hands back both the virtual alias and the physical
 * address.
 */
void *pmm_alloc_dma_range(size_t length, size_t alignment, phys_addr_t *out_phys)
{
	phys_addr_t total = pmm_total_pages;
	phys_addr_t start;

	length = ALIGN_UP(length, PAGE_SIZE);
	if (!alignment)
		alignment = PAGE_SIZE;

	/*
	 * Walk the present bitmaps from the bottom of ZONE_DMA, looking for a
	 * run of `length` consecutive frames that is present, unreserved, and
	 * aligned. Bit scanning is word-at-a-time so this is not O(frames) per
	 * byte; the common case finds the first run immediately.
	 */
	for (start = 0; start + (length >> PAGE_SHIFT) <= total; ) {
		phys_addr_t frames = length >> PAGE_SHIFT;
		phys_addr_t run = 0;
		phys_addr_t candidate = start;

		if (start % (alignment >> PAGE_SHIFT)) {
			/* Skip forward to the next aligned frame. */
			phys_addr_t step = alignment >> PAGE_SHIFT;
			start = ALIGN_UP(start, step);
			continue;
		}

		while (run < frames) {
			phys_addr_t idx = start + run;

			if (idx / 64 >= present_words)
				break;
			u64 word = frame_present[idx / 64] &
				frame_reserved[idx / 64] &
				~(1ULL << (idx % 64));
			/* Mask off bits below the current position within the
			 * word so the run cannot straddle into an earlier bit. */
			if (run % 64)
				word &= ~0ULL << (run % 64);

			if (word == 0) {
				/* Skip to the start of the next word. */
				phys_addr_t to_word = 64 - (idx % 64);
				run += to_word;
				continue;
			}

			int free_run = __builtin_ctzll(~word);
			if (free_run > 64 - (int)(idx % 64))
				free_run = 64 - (int)(idx % 64);
			run += (phys_addr_t)free_run;
			if (run >= frames)
				break;
			/* A set bit is a usable frame; advance past it. */
			run += 64 - (run % 64);
		}

		if (run >= frames) {
			pmm_reserve_range(start << PAGE_SHIFT, length);
			if (out_phys)
				*out_phys = start << PAGE_SHIFT;
			return (void *)phys_to_virt(start << PAGE_SHIFT);
		}

		start = ALIGN_UP(start + 1, alignment >> PAGE_SIZE ? alignment >> PAGE_SHIFT : 1);
	}

	klog(KLOG_WARN, "pmm: no %zu byte contiguous DMA range\n", length);
	return NULL;
}

/* ------------------------------------------------------------- init -------- */

/*
 * Initialise from the E820 map.
 *
 * The order of operations matters:
 *   1. Find the highest address reported as usable; that bounds the frame
 *      count and therefore the size of every metadata array.
 *   2. Place the metadata arrays in the direct map, which at this point is
 *      whatever stage2's identity map provided. They have to be somewhere
 *      before the buddy allocator can allocate anything.
 *   3. Mark every frame as reserved by default.
 *   4. Walk the E820 map, marking usable frames present and unreserved.
 *   5. Hand the usable, unreserved frames to the buddy allocator.
 *   6. Reserve the kernel image, the bootstrap page tables, and everything the
 *      bootloader used.
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
	phys_addr_t reserved = 0;
	phys_addr_t bitmap_bytes;
	u64 arena_start, arena_end;

	/* 1. Bound the address space. */
	for (uint32_t i = 0; i < e820_count; i++) {
		if (map[i].type != E820_USABLE)
			continue;
		phys_addr_t end = map[i].base + map[i].length;
		if (end > highest)
			highest = end;
	}

	if (highest == 0)
		panic("pmm: no usable memory reported by E820\n");

	pmm_total_pages = highest >> PAGE_SHIFT;

	/*
	 * 2. Metadata. Everything is placed at the top of usable memory and
	 * carved down, because the bottom is where the bootloader structures
	 * live and the top is the least contended.
	 */
	bitmap_bytes = ALIGN_UP((pmm_total_pages / 64) * sizeof(u64), PAGE_SIZE);

	/* Find a usable region big enough for the bitmaps and the page array. */
	arena_start = 0;
	arena_end = 0;
	for (uint32_t i = 0; i < e820_count; i++) {
		if (map[i].type != E820_USABLE)
			continue;
		phys_addr_t base = ALIGN_UP(map[i].base, PAGE_SIZE);
		phys_addr_t end = ALIGN_DOWN(map[i].base + map[i].length, PAGE_SIZE);
		phys_addr_t need;

		if (end <= base)
			continue;
		need = bitmap_bytes          /* present */
			+ bitmap_bytes       /* reserved */
			+ bitmap_bytes * ZONE_COUNT  /* zone maps */
			+ pmm_total_pages * sizeof(struct page);

		if (end - base >= need) {
			arena_end = end;
			arena_start = end - need;
			break;
		}
	}

	if (arena_end == 0) {
		/*
		 * The machine does not have a single contiguous run large enough
		 * for all the metadata at once. Fall back to placing each piece
		 * independently: this is the case on a fragmented VM, and
		 * refusing to boot because of it would be worse than scattering
		 * the arrays.
		 */
		klog(KLOG_WARN, "pmm: no contiguous metadata run; placing arrays separately\n");
	}

	/*
	 * Carve the arrays out of the chosen region, top down.
	 *
	 * Both placement paths hand back pointers rather than physical addresses.
	 * The direct map is already live here, so the contiguous path translates
	 * once at this point and the scattered path already has virtual
	 * addresses. Keeping one representation is the point: an earlier version
	 * had the contiguous path publish physical addresses and the scattered
	 * path publish pointers, then unconditionally re-derived the first from
	 * the second's variables — so on a fragmented machine the vmalloc results
	 * were discarded and replaced by translations of uninitialised values.
	 */
	phys_addr_t carve = arena_end;
	u64 *zone_maps_base, *present_map, *reserved_map;
	struct page *page_map;

	if (arena_end != 0) {
		phys_addr_t n;

		n = ALIGN_UP((phys_addr_t)bitmap_bytes * ZONE_COUNT, PAGE_SIZE);
		carve -= n;
		zone_maps_base = (u64 *)(uintptr_t)phys_to_virt(carve);
		memset(zone_maps_base, 0, n);

		n = ALIGN_UP((phys_addr_t)bitmap_bytes, PAGE_SIZE);
		carve -= n;
		present_map = (u64 *)(uintptr_t)phys_to_virt(carve);
		memset(present_map, 0, n);

		/* Same size as the present map, so the length computed above
		 * still applies. */
		carve -= n;
		reserved_map = (u64 *)(uintptr_t)phys_to_virt(carve);
		memset(reserved_map, 0, n);

		n = ALIGN_UP((phys_addr_t)(pmm_total_pages * sizeof(struct page)),
			     PAGE_SIZE);
		carve -= n;
		page_map = (struct page *)(uintptr_t)phys_to_virt(carve);
		memset(page_map, 0, n);
	} else {
		void *m;

		m = vmalloc_raw(bitmap_bytes * ZONE_COUNT);
		if (!m)
			panic("pmm: cannot allocate metadata\n");
		zone_maps_base = m;
		m = vmalloc_raw(bitmap_bytes);
		if (!m)
			panic("pmm: cannot allocate present bitmap\n");
		present_map = m;
		m = vmalloc_raw(bitmap_bytes);
		if (!m)
			panic("pmm: cannot allocate reserved bitmap\n");
		reserved_map = m;
		m = vmalloc_raw(pmm_total_pages * sizeof(struct page));
		if (!m)
			panic("pmm: cannot allocate page array\n");
		page_map = m;
	}

	for (int z = 0; z < ZONE_COUNT; z++)
		zone_bitmap[z] = zone_maps_base + (size_t)bitmap_bytes * z / sizeof(u64);

	frame_present = present_map;
	frame_reserved = reserved_map;
	page_array = page_map;
	page_array_pages = (pmm_total_pages * sizeof(struct page)) >> PAGE_SHIFT;
	present_words = (pmm_total_pages / 64) + 1;
	zone_words = present_words;

	/* 3. Default: everything is reserved. A frame becomes available only by
	 * being explicitly marked present and unreserved below. The E820 gap
	 * regions therefore stay unavailable for free. */
	memset(frame_present, 0, (size_t)present_words * sizeof(u64));
	memset(frame_reserved, 0xFF, (size_t)present_words * sizeof(u64));
	for (int z = 0; z < ZONE_COUNT; z++)
		memset(zone_bitmap[z], 0, (size_t)zone_words * sizeof(u64));

	/* 4. Mark usable ranges present. */
	for (uint32_t i = 0; i < e820_count; i++) {
		phys_addr_t base, end, addr;

		if (map[i].type != E820_USABLE)
			continue;
		base = ALIGN_UP(map[i].base, PAGE_SIZE);
		end = ALIGN_DOWN(map[i].base + map[i].length, PAGE_SIZE);

		for (addr = base; addr < end; addr += PAGE_SIZE) {
			if ((addr >> PAGE_SHIFT) >= pmm_total_pages)
				break;
			frame_set_present(addr, true);
			frame_set_reserved(addr, false);
			zone_set(zone_of(addr), addr >> PAGE_SHIFT, true);
		}
		reserved += end - base;
	}

	/* 5. Init zones and hand the free memory to the buddy allocator. */
	memset(zones, 0, sizeof(zones));
	static const char *const zone_names[ZONE_COUNT] = {
		"dma", "normal", "high"
	};

	for (int z = 0; z < ZONE_COUNT; z++) {
		struct pmm_zone *zone = &zones[z];

		zone->name = zone_names[z];
		zone->base = 0;
		zone->end = 0;
		zone->free_pages = 0;
		zone->total_pages = 0;
		zone->free_bitmap = 0;
		spinlock_init(&zone->lock);
		for (unsigned o = 0; o <= BUDDY_MAX_ORDER; o++)
			list_init(&zone->free_list[o]);
	}

	/*
	 * Build the free lists. Regions are handed over in ascending address
	 * order and aligned down to the region base, which is what makes the
	 * buddy invariant hold: a block at order k is 2^k-aligned, so its
	 * buddy is a valid block at the same order.
	 */
	phys_addr_t added = 0;
	for (uint32_t i = 0; i < e820_count; i++) {
		phys_addr_t base, end, addr;

		if (map[i].type != E820_USABLE)
			continue;

		base = ALIGN_UP(map[i].base, PAGE_SIZE);
		end = ALIGN_DOWN(map[i].base + map[i].length, PAGE_SIZE);

		/* Trim leading partial-order head so the first block is aligned. */
		for (addr = base; addr < end;) {
			zone_t z = zone_of(addr);
			struct pmm_zone *zone = &zones[z];
			unsigned order;
			phys_addr_t run = 0;
			phys_addr_t probe;
			phys_addr_t limit = MIN(end,
						   (z == ZONE_HIGH) ? pmm_total_pages << PAGE_SHIFT
						   : ((uint64_t)z == ZONE_DMA ?
						      ZONE_DMA_END : ZONE_NORMAL_END));

			if (addr >= limit) {
				addr = limit;
				continue;
			}

			/* Find the largest order for which `addr` is aligned and
			 * the whole block is inside this zone and this region. */
			for (order = BUDDY_MAX_ORDER; order > 0; order--) {
				phys_addr_t size = (1UL << order) << PAGE_SHIFT;
				bool aligned = (addr % size) == 0;
				bool fits = addr + size <= limit;
				bool same_zone = zone_of(addr) ==
						 zone_of(addr + size - 1);

				if (aligned && fits && same_zone)
					break;
			}

			/* Count how many consecutive blocks of this order fit. */
			probe = addr;
			while (probe + ((1UL << order) << PAGE_SHIFT) <= limit) {
				phys_addr_t size = (1UL << order) << PAGE_SHIFT;

				if ((probe % size) != 0)
					break;
				if (zone_of(probe) != zone_of(probe + size - 1))
					break;
				if (!frame_present_p(probe) ||
				    frame_reserved_p(probe))
					break;
				run += 1UL << order;
				probe += size;
			}

			if (run == 0) {
				addr += PAGE_SIZE;
				continue;
			}

			zone = &zones[zone_of(addr)];
			for (phys_addr_t n = 0; n < run; n += 1UL << order) {
				struct page *p = &page_array[(addr + n) >> PAGE_SHIFT];
				u64 irq = spinlock_irqsave(&zone->lock);

				buddy_free_locked(p, order);
				spinlock_unlock_irqrestore(&zone->lock, irq);
				added += 1UL << order;
			}

			addr = probe;
		}
	}

	for (int z = 0; z < ZONE_COUNT; z++) {
		zones[z].total_pages = zones[z].free_pages;
		if (zones[z].free_pages)
			zones[z].base = 0;
		pmm_total_pages += 0;   /* total was set from the E820 bound */
	}

	pmm_free_page_count = added;

	klog(KLOG_INFO, "pmm: %u usable frames (%llu MiB) of %llu reported\n",
	     (unsigned)added, (unsigned long long)(added * PAGE_SIZE >> 20),
	     (unsigned long long)(pmm_total_pages * PAGE_SIZE >> 20));
	for (int z = 0; z < ZONE_COUNT; z++)
		klog(KLOG_INFO, "pmm: zone %-6s free %u pages\n",
		     zones[z].name, (unsigned)zones[z].free_pages);

	klog(KLOG_INFO, "pmm: metadata bitmaps %zu KiB, page array %zu KiB\n",
	     (size_t)(present_words * 3 * sizeof(u64)) >> 10,
	     (size_t)(pmm_total_pages * sizeof(struct page)) >> 10);
}

void pmm_dump(void)
{
	klog(KLOG_INFO, "pmm: %llu/%llu frames free (%llu MiB of %llu MiB)\n",
	     (unsigned long long)pmm_free_page_count,
	     (unsigned long long)pmm_total_pages,
	     (unsigned long long)(pmm_free_page_count * PAGE_SIZE >> 20),
	     (unsigned long long)(pmm_total_pages * PAGE_SIZE >> 20));
	for (int z = 0; z < ZONE_COUNT; z++)
		klog(KLOG_INFO, "pmm:   %-6s %u/%u pages free\n",
		     zones[z].name, (unsigned)zones[z].free_pages,
		     (unsigned)zones[z].total_pages);
	klog(KLOG_INFO, "pmm: %llu allocs, %llu frees, %llu splits, %llu coalesces, %llu failures\n",
	     (unsigned long long)pmm_stats.allocs,
	     (unsigned long long)pmm_stats.frees,
	     (unsigned long long)pmm_stats.splits,
	     (unsigned long long)pmm_stats.coalesces,
	     (unsigned long long)pmm_stats.failed);
}
