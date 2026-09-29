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
 *   ZONE_HIGH    above 4 GiB. Reachable through the direct map, and the only
 *                place allocations land on a machine with more than 4 GiB.
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
 * 8 MiB. Anything larger is served as a contiguous run by the vmalloc
 * layer instead, because splitting a 16 MiB block to satisfy a small
 * request costs 12 splits and permanently fragments the highest orders.
 *
 * It lives here rather than in pmm.c because struct pmm_zone sizes its free
 * list array by it, and the struct is part of the public header.
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

/* Total machine memory, in pages. */
extern phys_addr_t pmm_total_pages;
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
 * Reserve a physical range: mark it allocated and unavailable. Used for the
 * kernel image, the bootstrap page tables, and MMIO regions discovered after
 * init. Must be called before the range is handed out, i.e. during init.
 */
void pmm_reserve_range(phys_addr_t base, phys_addr_t length);

/* Total and free frames across all zones. */
phys_addr_t pmm_total(void);
phys_addr_t pmm_free(void);

/* Free frames in one zone, for the NUMA and per-CPU statistics. */
phys_addr_t pmm_zone_free(zone_t zone);
phys_addr_t pmm_zone_total(zone_t zone);

/* Page descriptor for a physical address. O(1). */
struct page *phys_to_page(phys_addr_t addr);

/* Physical address of a page descriptor. */
phys_addr_t page_to_phys(const struct page *page);

/*
 * Find a contiguous physical range of `length` bytes for DMA, without
 * permanently allocating it. The caller pins it with pmm_pin_range() once the
 * device is programmed. Splitting this out is what lets a driver allocate its
 * descriptors, fail to set up the device, and release them cleanly.
 */
void *pmm_alloc_dma_range(size_t length, size_t alignment, phys_addr_t *out_phys);

/* Mark a range as permanently allocated (device MMIO, or mlock'd pages). */
void pmm_pin_range(phys_addr_t base, phys_addr_t length);

/* Print the zone summary. */
void pmm_dump(void);

#endif /* PMM_H */
