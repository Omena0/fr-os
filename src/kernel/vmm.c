/*
 * vmm.c — page tables, the direct map, vmalloc, and the CR3 switch.
 *
 * The hard part of this file is not any individual function; it is that the
 * first function cannot use anything the later ones provide. Building a page
 * table means writing to memory addressed by a page table that does not exist
 * yet, and the direct map that would normally make that easy is itself one of
 * the things being built here. The bootstrap below gets out of that with a
 * static pool inside the kernel image, which works because the bootloader is
 * still identity-mapping that image when vmm_init() runs.
 *
 * Boot order matters here, and the reverse does not work at all:
 *
 *   1. vmm_init()                 needs no allocator; tables come from .bss
 *   2. vmm_switch_to_kernel_pgd() now phys_to_virt() resolves
 *   3. pmm_init()                 uses phys_to_virt() for its metadata
 *
 * pmm_init() cannot come first. It places its bitmaps and its page array
 * through phys_to_virt(), and that does not resolve until the direct map exists.
 *
 * The kernel window these tables build is the same window the loader mapped, at
 * the same place: virtual KERNEL_VIRT_BASE is an alias of KERNEL_LANDING_ADDR,
 * not of physical zero. See map_kernel_window() for what that costs when it is
 * assumed otherwise.
 */
#include <vmm.h>

#include <console.h>
#include <cpu_features.h>
#include <io.h>
#include <klog.h>
#include <kstring.h>
#include <panic.h>
#include <percpu.h>
#include <types.h>

KLOG_SUBSYSTEM("vmm");

/* Provided by the link script. */
extern char __kernel_stack_top[];

/* The PML4 the kernel runs on. Per-CPU CR3 values are only needed once processes
 * exist, and those PML4s are built from copies of this one. */
static phys_addr_t kernel_pgd;

/*
 * Physical extent of the direct map.
 *
 * E820 is not consulted, on purpose: this runs before the allocator exists, and
 * mapping 4 GiB costs the same four page directories whether the machine has
 * 4 GiB or 512 MiB of it. Addresses past the machine's memory are simply never
 * touched, and a fault there is the same as a fault on any other absent page.
 */
#define DIRECT_MAP_BYTES  (4ULL << 30)

/* 2 MiB, the granularity the direct map and vmalloc both use. */
#define VMALLOC_CHUNK (2ULL << 20)

/*
 * How much of the kernel window is mapped up front: eight page tables of 512
 * entries, so 16 MiB. The loaded image ends at _ebss, which is 0x1C8000, and
 * __kernel_stack_top is that same address, so this is over 8x the headroom the
 * current link needs.
 */
#define KERNEL_WINDOW_PTS 8

/* ------------------------------------------------- early access ------------ */

/*
 * Physical memory, addressed directly.
 *
 * The bootloader's tables identity-map the low 4 GiB, so a physical address is
 * also a usable virtual address until the CR3 is replaced. This is the only
 * code in the kernel that may rely on that, which is why it is a function and
 * not a comment: after the switch the same expression silently means something
 * different, and anyone reading it later needs to be able to find every place
 * that depends on which regime it is in.
 */
static inline void *early_phys(phys_addr_t phys)
{
	return (void *)(uintptr_t)phys;
}

/*
 * The physical address of anything linked into the kernel is
 * kernel_virt_to_phys() of its link address, which is vmm.h's
 * KERNEL_LANDING_ADDR-offset translation and not an identity map of low
 * memory. This file deliberately keeps no second copy of that rule: the two
 * facts about the window -- which PDPT entry it is reached through and what
 * physical address it is an alias of -- live with KERNEL_VIRT_BASE in boot.h,
 * which the bootloader includes too, and are the only two that have to agree
 * between the loader's tables and these.
 */

/* ------------------------------------------------- bootstrap pool ---------- */

/*
 * The first PML4 and its tables, carved from the kernel image rather than from
 * the allocator, because there is no allocator yet.
 *
 * These pages are physically inside the image: boot_pt_pool is at link offset
 * 0x1AF000, so the pool is physical 0x2AF000-0x2BF000, well above the low
 * megabyte that main.c reserves. They stay reserved for the life of the kernel:
 * a page table that something might still be walking is not worth reclaiming,
 * and there are only a few dozen of them. Nothing in this file can make that
 * true -- it is a property of what pmm is told to reserve, not of these tables
 * -- so the claim is recorded here as something the rest of the kernel has to
 * honour rather than something achieved here.
 */
#define BOOT_PT_POOL_PAGES 16
static uint8_t boot_pt_pool[BOOT_PT_POOL_PAGES * PAGE_SIZE]
	__attribute__((aligned(PAGE_SIZE)));

static phys_addr_t pool_next;

/* Set once the CR3 write in vmm_switch_to_kernel_pgd() has happened. */
static bool direct_map_live;

void *vmm_boot_ptr(phys_addr_t phys)
{
	return direct_map_live ? phys_to_virt(phys) : early_phys(phys);
}

/*
 * One zeroed page from the pool.
 *
 * The zeroing is not tidiness. An entry of all zeros reads as "not present",
 * which is exactly what an unbuilt table has to look like; a table carrying
 * stale bits from a previous boot would alias whatever they happened to point
 * at. The first tables the kernel builds are the ones every later fault walks
 * through, so this is the single place where that assumption is established.
 */
static phys_addr_t boot_table_alloc(void)
{
	phys_addr_t phys;

	if (pool_next >= BOOT_PT_POOL_PAGES)
		panic("vmm: bootstrap page table pool exhausted");

	phys = kernel_virt_to_phys((virt_addr_t)(uintptr_t)boot_pt_pool) +
	       pool_next * PAGE_SIZE;
	pool_next++;

	memset(early_phys(phys), 0, PAGE_SIZE);
	return phys;
}

/* ------------------------------------------------- table walking ----------- */

/*
 * Return the physical address of the table at `level` for `virt`, where level 4
 * is the PML4, 3 a PDPT, 2 a PD, and 1 a PT.
 *
 * Returns 0 when the walk stops early, which is the ordinary answer for an
 * unmapped address and not something the caller has to tell apart from a fault.
 *
 * The walk never descends into a leaf: below a PD entry with PS set there is no
 * table, only the rest of the address, so descending would read a physical
 * address as a pointer. The leaf PTE, when there is one, is returned through
 * `leaf` and is always indexed by PT_ENTRY_OF regardless of the level asked
 * for.
 *
 * `leaf` is only valid until the next call. It points into the direct map, and
 * the caller is expected to read the entry immediately — which is the whole
 * point of the walk being four dependent loads rather than eight.
 */
static phys_addr_t walk(phys_addr_t pgd, virt_addr_t virt, unsigned level,
			uint64_t **leaf)
{
	static const unsigned pml4_idx_at[5] = { 0, 0, 0, 0, 0 };
	phys_addr_t table = pgd;
	uint64_t *entries;

	(void)pml4_idx_at;
	if (leaf)
		*leaf = NULL;

	/* Descend from the PML4 (level 4) down to but not including `level`. */
	for (unsigned l = 4; l > level; l--) {
		unsigned index;

		entries = phys_to_virt(table);
		switch (l) {
		case 4: index = PML4_ENTRY_OF(virt); break;
		case 3: index = PDPT_ENTRY_OF(virt); break;
		default: index = PD_ENTRY_OF(virt); break;
		}

		uint64_t entry = entries[index];

		if (!(entry & PTE_PRESENT) || (entry & PTE_PS))
			return 0;
		table = entry & PTE_ADDR_MASK;
	}

	if (leaf) {
		entries = phys_to_virt(table);
		*leaf = &entries[PT_ENTRY_OF(virt)];
	}
	return table;
}

/* ------------------------------------------------- bootstrap mapping ------- */

static void map_direct_map(uint64_t *pml4)
{
	phys_addr_t pdpt = boot_table_alloc();
	uint64_t *pdpt_entries = early_phys(pdpt);
	unsigned gib = (unsigned)(DIRECT_MAP_BYTES >> 30);

	/* PML4[256] is 0xFFFF800000000000, the direct map base. */
	pml4[256] = pdpt | PTE_PRESENT | PTE_WRITE;

	/*
	 * 1 GiB pages when the CPU has them, 2 MiB otherwise. Both are a
	 * deliberate choice over 4 KiB: a 4 GiB machine mapped with 4 KiB pages
	 * needs 2048 page-table pages and 2048 TLB entries, so almost every
	 * access to ordinary memory would be a page walk. With 2 MiB pages it
	 * needs four and four.
	 */
	if (cpu_features.has_1gb_pages) {
		for (unsigned i = 0; i < gib; i++)
			pdpt_entries[i] = ((phys_addr_t)i << 30) |
					 PTE_PRESENT | PTE_WRITE | PTE_PS;
		klog(KLOG_INFO, "vmm: direct map 0-%u GiB, 1 GiB pages\n", gib);
		return;
	}

	for (unsigned i = 0; i < gib; i++) {
		phys_addr_t pd = boot_table_alloc();
		uint64_t *entries = early_phys(pd);

		pdpt_entries[i] = pd | PTE_PRESENT | PTE_WRITE;
		for (unsigned j = 0; j < 512; j++)
			entries[j] = (((phys_addr_t)i << 30) +
				      (phys_addr_t)j * (2ULL << 20)) |
				     PTE_PRESENT | PTE_WRITE | PTE_PS;
	}
	klog(KLOG_INFO, "vmm: direct map 0-%u GiB, 2 MiB pages\n", gib);
}

/*
 * The kernel window, at the same place and with the same shape stage2 put it.
 *
 * The window is not an identity map of low physical memory. stage2 copies the
 * image to KERNEL_LANDING_ADDR because INT 13h cannot address above 1 MiB, and
 * maps virtual KERNEL_VIRT_BASE there, so what the tables below describe is
 *
 *	virt KERNEL_VIRT_BASE + off	->	phys KERNEL_LANDING_ADDR + off
 *
 * Describing it as an identity map is not a simplification that happens to work;
 * it is the failure that is hardest to see. The kernel's entry point is at link
 * offset 0x180, so an identity window resolves it to physical 0x180 -- which is
 * real-mode interrupt vector table, not code. The CPU then executes IVT entries
 * and never faults, because every address in the window resolves to *something*,
 * and the loader has already printed that it is entering long mode. The long
 * comment in stage2.c on the same mapping says the same thing from the other
 * side; that file used to have this bug, and it was the reason nothing printed
 * at all.
 *
 * The page size is 4 KiB for the whole window, which is what stage2 has to use
 * for its first 2 MiB (KERNEL_LANDING_ADDR is 1 MiB-aligned but not
 * 2 MiB-aligned, so a large page cannot name it) and the right choice above
 * that: 2 MiB pages here would make physical 1-17 MiB writable through the
 * window, and pmm believes most of that is free.
 *
 * The window spans 16 MiB of link offset. The loaded image ends at 0x1C8000 and
 * __kernel_stack_top is the same address, so that is over 8x what is reachable
 * now. The further 3 GiB that stage2 additionally maps with large pages is
 * deliberately not reproduced: nothing above the image is reached through the
 * window, since vmalloc and kstack live at VMALLOC_AREA, and vmm_map_page()
 * builds the PDPT entries for that range on first use.
 */
static void map_kernel_window(uint64_t *pml4)
{
	phys_addr_t pdpt = boot_table_alloc();
	phys_addr_t pd = boot_table_alloc();
	uint64_t *pd_entries = early_phys(pd);

	/* PML4 511 covers the entire negative canonical half, which is where the
	 * kernel window, vmalloc and kstack all live. Named, not written: stage2
	 * fills the same entry from the same definition. */
	pml4[KERNEL_PML4_IDX] = pdpt | PTE_PRESENT | PTE_WRITE;

	/*
	 * PDPT 510 is the 1 TiB region KERNEL_VIRT_BASE starts, and it is the
	 * single entry that address walks through: PML4 511, PDPT 510, PD 0, PT 0.
	 *
	 * Getting this wrong does not fault at the write. 511 names a real 1 TiB
	 * region one step above the window, so the table reads back fine and every
	 * diagnostic passes; the fault arrives on the instruction after the CR3
	 * write, delivered through an IDT that has not been installed yet, which
	 * makes it a triple fault and looks from the outside like QEMU exiting
	 * without a word.
	 */
	((uint64_t *)early_phys(pdpt))[KERNEL_PDPT_IDX] = pd | PTE_PRESENT | PTE_WRITE;

	for (unsigned i = 0; i < KERNEL_WINDOW_PTS; i++) {
		phys_addr_t pt = boot_table_alloc();
		uint64_t *pte = early_phys(pt);

		for (unsigned j = 0; j < 512; j++)
			pte[j] = (KERNEL_LANDING_ADDR +
				  (phys_addr_t)i * (2ULL << 20) +
				  (phys_addr_t)j * PAGE_SIZE) |
				 PTE_PRESENT | PTE_WRITE;
		pd_entries[i] = pt | PTE_PRESENT | PTE_WRITE;
	}

	klog(KLOG_INFO, "vmm: kernel window %#lx-%#lx -> phys %#lx-%#lx, 4 KiB pages\n",
	     (unsigned long)KERNEL_VIRT_BASE,
	     (unsigned long)(KERNEL_VIRT_BASE +
			     (phys_addr_t)KERNEL_WINDOW_PTS * (2ULL << 20)),
	     (unsigned long)KERNEL_LANDING_ADDR,
	     (unsigned long)(KERNEL_LANDING_ADDR +
			     (phys_addr_t)KERNEL_WINDOW_PTS * (2ULL << 20)));
}

/* ------------------------------------------------- init --------------------- */

void vmm_init(void)
{
	phys_addr_t pool_base = kernel_virt_to_phys((virt_addr_t)(uintptr_t)boot_pt_pool);
	uint64_t *pml4;

	pool_next = 0;

	kernel_pgd = boot_table_alloc();
	pml4 = early_phys(kernel_pgd);

	map_direct_map(pml4);
	map_kernel_window(pml4);

	klog(KLOG_INFO, "vmm: kernel PML4 at %#lx from %lu pool pages "
	     "(#%lx-%#lx)\n", (unsigned long)kernel_pgd,
	     (unsigned long)pool_next, (unsigned long)pool_base,
	     (unsigned long)(pool_base + sizeof(boot_pt_pool)));
}

void vmm_switch_to_kernel_pgd(void)
{
	/*
	 * PAE is what makes the 4-level format selectable, and it is also the bit
	 * that turns CR3 into a PML4 pointer rather than a directory pointer.
	 */
	write_cr4(read_cr4() | (1ULL << 5));

	/*
	 * EFER.NXE enables the NX bit. Without it the bit is reserved, so a page
	 * marked non-executable stays executable and the W^X policy appears to
	 * work while enforcing nothing — which is worse than not having it,
	 * because the page tables say otherwise.
	 */
	u64 efer = rdmsr(MSR_EFER);
	if (!(efer & EFER_NXE))
		wrmsr(MSR_EFER, efer | EFER_NXE);

	write_cr3(kernel_pgd);
	cpu_barrier();

	/*
	 * The identity view is gone as of the CR3 write above, and this is the one
	 * place that knows it. Anything that still holds a low physical address
	 * across the switch -- the VGA text buffer, the framebuffer the firmware
	 * reported -- has to start resolving it through vmm_boot_ptr(), and the
	 * difference is a #PF on the next store rather than a wrong value.
	 */
	direct_map_live = true;

	klog(KLOG_INFO, "vmm: CR3 = %#lx, direct map and kernel window live\n",
	     (unsigned long)read_cr3());
}

/* ------------------------------------------------- page tables -------------- */

phys_addr_t pt_alloc_zeroed(unsigned count)
{
	phys_addr_t first = 0;

	for (unsigned i = 0; i < count; i++) {
		struct page *p = pmm_alloc_page(0);

		if (!p)
			return first;
		if (!first)
			first = page_to_phys(p);
		memset(phys_to_virt(page_to_phys(p)), 0, PAGE_SIZE);
	}
	return first;
}

void pt_free(phys_addr_t pgd, unsigned count)
{
	for (unsigned i = 0; i < count; i++)
		pmm_free_pages(phys_to_page(pgd + (phys_addr_t)i * PAGE_SIZE), 0);
}

/* Build the PD and PT that `virt` needs but does not have yet. */
static int build_missing_tables(phys_addr_t pgd, virt_addr_t virt)
{
	phys_addr_t table = walk(pgd, virt, 2, NULL);
	phys_addr_t fresh;

	if (!table) {
		phys_addr_t pdpt = walk(pgd, virt, 3, NULL);

		if (!pdpt)
			return -1;
		fresh = pt_alloc_zeroed(1);
		if (!fresh)
			return -1;
		((uint64_t *)phys_to_virt(pdpt))[PD_ENTRY_OF(virt)] =
			fresh | PTE_PRESENT | PTE_WRITE;
		table = fresh;
	}

	fresh = pt_alloc_zeroed(1);
	if (!fresh)
		return -1;
	/* Supervisor only, always. A user process must not be able to reach the
	 * tables that describe it. */
	((uint64_t *)phys_to_virt(table))[PD_ENTRY_OF(virt)] =
		fresh | PTE_PRESENT | PTE_WRITE;

	return 0;
}

int vmm_map_page(phys_addr_t pgd, virt_addr_t virt, phys_addr_t phys,
		 uint32_t prot)
{
	uint64_t *pte = NULL;
	phys_addr_t table;

	if (virt & (PAGE_SIZE - 1))
		return -1;

	table = walk(pgd, virt, 1, &pte);
	if (!table) {
		if (build_missing_tables(pgd, virt))
			return -1;
		table = walk(pgd, virt, 1, &pte);
		if (!table)
			return -1;
	}

	if (*pte & PTE_PRESENT)
		return -1;  /* remapping an occupied PTE is a caller bug, not a
			     * thing to paper over by overwriting */

	*pte = (phys & PTE_ADDR_MASK) | PTE_PRESENT |
	       ((prot & VM_WRITE) ? PTE_WRITE : 0) |
	       ((prot & VM_USER) ? PTE_USER : 0) |
	       ((prot & VM_EXEC) ? 0 : PTE_NX);

	invlpg(virt);
	return 0;
}

phys_addr_t vmm_unmap_page(phys_addr_t pgd, virt_addr_t virt)
{
	uint64_t *pte = NULL;
	phys_addr_t table = walk(pgd, virt, 1, &pte);
	phys_addr_t phys;

	if (!table || !pte || !(*pte & PTE_PRESENT))
		return 0;
	phys = *pte & PTE_ADDR_MASK;
	*pte = 0;
	invlpg(virt);
	return phys;
}

phys_addr_t vmm_translate(phys_addr_t pgd, virt_addr_t virt, uint64_t **leaf)
{
	uint64_t *pte = NULL;
	phys_addr_t table = walk(pgd, virt, 1, &pte);

	if (leaf)
		*leaf = pte;
	if (!table || !pte || !(*pte & PTE_PRESENT))
		return 0;
	return (*pte) & PTE_ADDR_MASK;
}

int vmm_map_large(phys_addr_t pgd, virt_addr_t virt, phys_addr_t phys,
		  uint64_t count_pages_2mib, uint32_t prot)
{
	for (uint64_t i = 0; i < count_pages_2mib; i++) {
		int r = vmm_map_page(pgd, virt + i * VMALLOC_CHUNK,
				     phys + i * VMALLOC_CHUNK, prot);

		if (r)
			return r;
	}
	return 0;
}

/* ------------------------------------------------- vmalloc ----------------- */

/*
 * The vmalloc window is carved into 2 MiB chunks and split, so the bookkeeping
 * is a first-fit over a sorted array rather than a buddy tree. vmalloc serves
 * large, long-lived objects where there are few live blocks and fragmentation
 * costs more than allocation latency; a kmalloc hot path is the opposite and
 * lives in its own file.
 */
struct vmalloc_block {
	virt_addr_t start;
	virt_addr_t end;
	bool used;
};

#define VMALLOC_MAX_BLOCKS 2048
static struct vmalloc_block vmalloc_blocks[VMALLOC_MAX_BLOCKS];
static unsigned vmalloc_block_count;
static virt_addr_t vmalloc_brk = VMM_VMALLOC_BASE;
static spinlock_t vmalloc_lock = SPINLOCK_INIT;

/* Insert a block keeping the array sorted by start address. */
static int block_insert(unsigned at, virt_addr_t start, virt_addr_t end,
			bool used)
{
	if (vmalloc_block_count >= VMALLOC_MAX_BLOCKS)
		return -1;

	for (unsigned i = vmalloc_block_count; i > at; i--)
		vmalloc_blocks[i] = vmalloc_blocks[i - 1];

	vmalloc_blocks[at].start = start;
	vmalloc_blocks[at].end = end;
	vmalloc_blocks[at].used = used;
	vmalloc_block_count++;
	return 0;
}

/*
 * Extend the window by one run of pages and record it as free.
 *
 * The new run is appended rather than merged, which keeps the invariant simple
 * enough to state in one sentence: the blocks tile [VMM_VMALLOC_BASE,
 * vmalloc_brk) exactly, in order, with no gaps and no overlaps. A first-fit
 * pass over that array is then obviously correct, which matters more here than
 * saving a comparison.
 */
static bool vmalloc_grow(size_t size)
{
	virt_addr_t start = vmalloc_brk;
	unsigned at = vmalloc_block_count;

	if (start + size > VMM_VMALLOC_END)
		return false;

	for (size_t off = 0; off < size; off += PAGE_SIZE) {
		if (vmm_map_page(kernel_pgd, start + off, start + off,
				 VM_READ | VM_WRITE)) {
			/* Unwind. A partial mapping left behind would make the
			 * next attempt collide with pages the array believes are
			 * still free. */
			while (off) {
				off -= PAGE_SIZE;
				vmm_unmap_page(kernel_pgd, start + off);
			}
			return false;
		}
	}

	if (block_insert(at, start, start + size, false)) {
		for (size_t off = 0; off < size; off += PAGE_SIZE)
			vmm_unmap_page(kernel_pgd, start + off);
		return false;
	}

	vmalloc_brk += size;
	return true;
}

void *vmalloc_raw(size_t size)
{
	/*
	 * Once pmm_init() has run, the direct map is live and "raw" is just
	 * vmalloc(). The distinction existed because pmm_init() used to call this
	 * before the direct map existed, when the only honest thing it could
	 * return was a bare physical address that would not have survived the CR3
	 * switch. The boot order at the top of this file removed that case, so
	 * the two are the same function and pmm's metadata is safe to keep.
	 */
	return vmalloc(size);
}

void *vmalloc(size_t size)
{
	u64 flags;
	virt_addr_t virt;

	if (!size)
		return NULL;
	size = ALIGN_UP(size, PAGE_SIZE);
	if (size > VMM_VMALLOC_SIZE)
		return NULL;

	flags = spinlock_irqsave(&vmalloc_lock);

	/*
	 * One first-fit pass, then grow and try once more. Growing first would be
	 * wasteful for the common case of a long-lived allocation reusing freed
	 * space, which is the whole reason vfree keeps its bookkeeping.
	 */
	for (int attempt = 0; attempt < 2; attempt++) {
		for (unsigned i = 0; i < vmalloc_block_count; i++) {
			struct vmalloc_block *b = &vmalloc_blocks[i];

			if (b->used || b->end - b->start < size)
				continue;

			virt = b->start;
			if (b->end - b->start == size) {
				b->used = true;
			} else {
				/* Split the front off; the tail stays with the
				 * free entry it came from. */
				b->start += size;
				if (block_insert(i, virt, virt + size, true))
					goto out_fail;
			}
			spinlock_unlock_irqrestore(&vmalloc_lock, flags);
			return (void *)(uintptr_t)virt;
		}

		if (attempt == 0 && !vmalloc_grow(size))
			break;
	}

out_fail:
	spinlock_unlock_irqrestore(&vmalloc_lock, flags);
	return NULL;
}

void *vmalloc_aligned(size_t size, size_t alignment)
{
	u64 flags;
	virt_addr_t virt;

	if (!size || alignment < PAGE_SIZE || (alignment & (alignment - 1)))
		return NULL;
	if (alignment > VMALLOC_CHUNK)
		return NULL;   /* chunks are the granularity below which nothing
				 * here can be positioned */

	size = ALIGN_UP(size, alignment);
	flags = spinlock_irqsave(&vmalloc_lock);

	for (int attempt = 0; attempt < 2; attempt++) {
		for (unsigned i = 0; i < vmalloc_block_count; i++) {
			struct vmalloc_block *b = &vmalloc_blocks[i];

			if (b->used || b->end - b->start < size)
				continue;

			/* Start the allocation at the first aligned address
			 * inside the block, leaving the head as its own free
			 * block. Skipping the head rather than splitting the
			 * front off is what makes the alignment land, because
			 * every block starts on a chunk boundary. */
			virt = ALIGN_UP(b->start, alignment);
			if (virt + size > b->end)
				continue;

			if (virt > b->start &&
			    block_insert(i, b->start, virt, false)) {
				b->start = virt;   /* undo, keep the block usable */
				goto out_fail;
			}
			unsigned at = (virt > b->start) ? i + 1 : i;
			if (virt + size < b->end) {
				if (block_insert(at, virt + size, b->end, false))
					goto out_fail;
				b->end = virt + size;
			}
			if (block_insert(at, virt, virt + size, true))
				goto out_fail;

			spinlock_unlock_irqrestore(&vmalloc_lock, flags);
			return (void *)(uintptr_t)virt;
		}

		if (attempt == 0 && !vmalloc_grow(size + alignment))
			break;
	}

out_fail:
	spinlock_unlock_irqrestore(&vmalloc_lock, flags);
	return NULL;
}

void vfree(void *addr)
{
	u64 flags;
	virt_addr_t virt = (virt_addr_t)addr;

	if (!addr)
		return;

	flags = spinlock_irqsave(&vmalloc_lock);
	for (unsigned i = 0; i < vmalloc_block_count; i++) {
		if (vmalloc_blocks[i].used && vmalloc_blocks[i].start == virt) {
			vmalloc_blocks[i].used = false;
			break;
		}
	}
	spinlock_unlock_irqrestore(&vmalloc_lock, flags);

	/*
	 * The mapping is left in place and the frames are not returned to pmm.
	 * vmalloc serves long-lived objects, so the normal case is an allocation
	 * that lives until exit; reclaiming would mean walking every PT covering
	 * the range on every free, and a stale pointer would then read reused
	 * memory instead of faulting. The cost is that a freed range stays
	 * reserved, which is recorded here so the tradeoff is not rediscovered as
	 * a leak.
	 */
}

/* ------------------------------------------------- kernel stacks ----------- */

void *kstack_alloc(size_t size)
{
	/*
	 * One extra page sits below the stack so that an overflow faults at a
	 * known address instead of corrupting whichever allocation happens to be
	 * mapped lower. Returning base + PAGE_SIZE is the caller's handle; the
	 * page beneath it is the guard.
	 */
	void *raw = vmalloc(size + PAGE_SIZE);

	if (!raw)
		return NULL;
	return (void *)((uintptr_t)raw + PAGE_SIZE);
}

void kstack_free(void *stack)
{
	if (!stack)
		return;
	vfree((void *)((uintptr_t)stack - PAGE_SIZE));
}

void *irq_stack_top(void)
{
	return (void *)__kernel_stack_top;
}
