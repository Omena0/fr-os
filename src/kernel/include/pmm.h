/*
 * pmm.h — physical page allocator (buddy system).
 *
 * Pages are 4 KiB frames. The allocator hands out physically contiguous blocks
 * of 2^order frames, which is what DMA-capable allocations need and what the
 * larger mapping levels want.
 *
 * Zones split the address space by what a caller is allowed to get:
 *
 *   ZONE_DMA     below 16 MiB. Legacy ISA DMA cannot address above 16 MiB
 *                because its bus address lines are wired that way. A driver
 *                asking for GFP_DMA must be served from here.
 *   ZONE_NORMAL  everything else that fits in 32 bits. The common case.
 *   ZONE_HIGH    above 4 GiB. NOT reachable through the direct map: the map
 *                backs only the low 4 GiB (DIRECT_MAP_BYTES, vmm.c), so a
 *                consumer that phys_to_virt()s a ZONE_HIGH frame faults. It is
 *                populated only on a machine with more than 4 GiB, and is the
 *                place allocations land there, so any of the callers that
 *                memsets or walks what it gets back (GFP_ZERO's memset,
 *                pt_alloc_zeroed, slab_new, clone_table) is wrong above 4 GiB.
 *                Reachable via the kernel window, or not at all.
 *
 * Each zone has its own free lists so a DMA allocation can never consume memory
 * a normal allocation would have used.
 */
#ifndef PMM_H
#define PMM_H

#include <types.h>
#include <list.h>
#include <spinlock.h>

/* Allocation flags, following the GFP_ convention. */
#define GFP_KERNEL     0u   /* may sleep, may reclaim */
#define GFP_ATOMIC     1u   /* may not sleep: interrupt or spinlock context */
#define GFP_DMA        2u   /* must come from ZONE_DMA */
#define GFP_NOWAIT     4u   /* fail rather than reclaim */
#define GFP_ZERO       8u   /* zero the pages before returning */
#define GFP_HUGE      16u   /* 2 MiB pages if the machine supports them */

/* Page flags. */
#define PG_FREE        (1u << 0)
#define PG_ALLOCATED   (1u << 1)
#define PG_BUDDY       (1u << 2)   /* head of a buddy block at some order */
#define PG_SLAB        (1u << 3)
#define PG_TAIL        (1u << 4)   /* interior page of a higher-order block */
#define PG_ZEROED      (1u << 5)   /* known to be all zeroes */
#define PG_PINNED      (1u << 6)   /* mlock'd: never reclaim */
#define PG_WRITEBACK   (1u << 7)
#define PG_DIRTY       (1u << 8)

typedef enum {
	ZONE_DMA = 0,
	ZONE_NORMAL = 1,
	ZONE_HIGH = 2,
	ZONE_COUNT = 3,
} zone_t;

/*
 * Page metadata.
 *
 * `union` with the private data because the two are mutually exclusive: a free
 * page at order N needs its buddy to find its coalescing partner, and an
 * allocated page needs its slab or cache index. Both are reached through
 * phys_to_page(), which is an O(1) index into a flat array in the direct map,
 * so the struct is a single 32 bytes.
 */
struct page {
	uint32_t flags;
	uint8_t  order;         /* buddy order, valid when PG_BUDDY is set */
	uint8_t  zone;
	uint16_t numa_node;
	uint32_t refcount;

	/* Free: link in the zone's free list for `order`.
	 * Allocated: slab pointer, or page cache index. */
	union {
		struct list_head list;
		void *slab;
		uint64_t index;
	};
} __packed;

STATIC_ASSERT(sizeof(struct page) <= 32, "struct page must stay within 32 bytes");

/*
 * Largest buddy order the allocator serves: order 11 is 2048 pages, i.e.
 * 8 MiB, and orders 0 through 11 are all served (12 levels). Anything larger is
 * served as a non-contiguous run by the vmalloc layer instead, because
 * splitting a 16 MiB block to satisfy a small request costs 12 splits and
 * permanently fragments the highest orders.
 *
 * This is the only definition. It lives here rather than in pmm.c because
 * struct pmm_zone sizes its free list array by it and the struct is part of the
 * public header, so a second definition in pmm.c was free to drift from this one
 * without anything noticing. (docs/src/memory/buddy-allocator.md still says
 * 11 levels, orders 0-10 and 4 MiB. The code is the authority; that document is
 * stale.)
 */
#define BUDDY_MAX_ORDER 11

/*
 * Per-zone free state. One lock per zone rather than one global lock: the
 * allocator is the busiest lock in the kernel, and serialising DMA allocations
 * against high-memory allocations is a bottleneck that per-zone locking removes
 * entirely.
 */
struct pmm_zone {
	const char *name;
	phys_addr_t base;          /* first physical address in the zone */
	phys_addr_t end;           /* one past the last */
	phys_addr_t free_pages;
	phys_addr_t total_pages;

	/* One free list per order. Index 0 holds single pages. */
	struct list_head free_list[BUDDY_MAX_ORDER + 1];

	/*
	 * Bitmap of non-empty orders. Finding the smallest order >= the request
	 * is a single tzcnt on the masked value rather than a walk over up to 11
	 * empty lists. With CPuid-confirmed BMI1 the compiler emits TZCNT, whose
	 * encoding of zero is defined, so a miss is detected without a branch on
	 * undefined behaviour.
	 */
	uint16_t free_bitmap;

	spinlock_t lock;
};

/*
 * Frames the allocator tracks: every frame below the highest usable address the
 * E820 map reported, including the ones that fall in an E820 hole. Bounds the
 * bitmaps and phys_to_page(); it is *not* a memory total. pmm_total() below is.
 */
extern phys_addr_t pmm_total_pages;

/* Frames present and unreserved after init: the real memory total. */
extern phys_addr_t pmm_usable_pages;

/* Frames currently on a free list, across all zones. */
extern phys_addr_t pmm_free_page_count;

/* Statistics for the memory subsystem's observability interface. */
struct pmm_stats {
	uint64_t allocs;
	uint64_t frees;
	uint64_t splits;
	uint64_t coalesces;
	uint64_t failed;
	uint64_t reclaim_attempts;
};

extern struct pmm_stats pmm_stats;

/*
 * Build the allocator from the E820 map.
 *
 * `map`/`count` describe the ranges the firmware reported. Regions flagged
 * usable become buddy free lists; everything else is marked reserved. The
 * regions the bootloader reserved for itself (kernel image, page tables,
 * bootinfo, stage2) must already be marked non-usable by the caller or by the
 * ranges themselves — see pmm_reserve_range().
 */
void pmm_init(phys_addr_t e820_phys, uint32_t e820_count);

/*
 * Allocate 2^order contiguous frames.
 *
 * Fast path: the free list for the requested order is non-empty and a block
 * pops off it in O(1). Slow path: find a larger block and split it down, which
 * is at most BUDDY_MAX_ORDER iterations.
 *
 * Returns NULL rather than panicking. Allocation failure is a real condition
 * (the overcommit policy and the OOM path both depend on being able to observe
 * it), so the caller decides what to do.
 */
struct page *pmm_alloc_pages(unsigned order, unsigned flags);

static inline struct page *pmm_alloc_page(unsigned flags)
{
	return pmm_alloc_pages(0, flags);
}

/* Free a block previously returned by pmm_alloc_pages() at the same order. */
void pmm_free_pages(struct page *page, unsigned order);

/*
 * Reserve a physical range: mark it allocated and remove it from the buddy
 * allocator's free lists. Used for the kernel image, the bootstrap page
 * tables, the allocator's own metadata arena, and MMIO regions discovered after
 * init. Must be called before the range is handed out, i.e. during init.
 *
 * The range is validated: length 0 and a base+length that wraps are ignored, and
 * the range is clamped to what the allocator tracks. A reservation genuinely
 * takes the frames out of the free lists even when they sit in the interior of
 * a larger block, and the parts of that block outside the range are handed back
 * in a form that can coalesce, so reserving the low megabyte does not
 * permanently shatter every high order in the zone.
 */
void pmm_reserve_range(phys_addr_t base, phys_addr_t length);

/*
 * Claim a physical range and guarantee it: the frames are removed from the free
 * lists and marked PG_PINNED, so neither buddy_free_locked() nor any future
 * reclamation pass can put them back. Callers that took the address from
 * pmm_alloc_dma_range() call this once the device is programmed.
 */
void pmm_pin_range(phys_addr_t base, phys_addr_t length);

/*
 * Free and total frames, across all zones or for one zone.
 *
 * All four are FRAMES, not bytes. Reporting code that wants bytes must scale
 * them: shifting a frame count right by 20 prints 1/256th of the real figure,
 * which is how the boot log came to claim "1 MiB total, 0 MiB free" on a
 * machine with four of them.
 */
phys_addr_t pmm_total(void);
phys_addr_t pmm_free(void);

/* Byte-denominated forms of the two above, for reporting. */
static inline phys_addr_t pmm_total_bytes(void) { return pmm_total() * PAGE_SIZE; }
static inline phys_addr_t pmm_free_bytes(void)  { return pmm_free() * PAGE_SIZE; }

/* Frames in one zone, for the NUMA and per-CPU statistics. */
phys_addr_t pmm_zone_free(zone_t zone);
phys_addr_t pmm_zone_total(zone_t zone);

/* Page descriptor for a physical address. O(1). */
struct page *phys_to_page(phys_addr_t addr);

/* Physical address of a page descriptor. */
phys_addr_t page_to_phys(const struct page *page);

/*
 * Find a contiguous physical range of `length` bytes for DMA, aligned to
 * `alignment` (rounded up to PAGE_SIZE and then to the next power of two, with a
 * warning if it was not one). The range is *reserved*, i.e. already out of the
 * buddy free lists, so it cannot be handed out twice; pmm_pin_range() then marks
 * it PG_PINNED once the device is programmed. Splitting this out is what lets a
 * driver allocate its descriptors, fail to set up the device, and release them
 * cleanly.
 */
void *pmm_alloc_dma_range(size_t length, size_t alignment, phys_addr_t *out_phys);

/* Print the zone summary. */
void pmm_dump(void);

#endif /* PMM_H */
