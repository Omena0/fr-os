/*
 * vmm.h — virtual memory manager.
 *
 * The kernel runs in the higher half at 0xFFFFFFFF80000000 and keeps a direct
 * map of all physical memory at 0xFFFF800000000000. Everything below is built
 * on two primitives: given a physical address, produce a virtual one (always
 * possible, through the direct map), and given a virtual address in a user
 * address space, find its frame.
 *
 * The direct map uses 2 MiB pages, and 1 GiB pages where the firmware supports
 * them. This is a deliberate performance choice rather than an incidental one:
 * a 4 GiB machine mapped with 4 KiB pages needs 2048 page-table pages and 2048
 * TLB entries; with 2 MiB pages it needs 2 page-table pages and 2 TLB entries.
 * The page-table savings are real memory; the TLB savings are the difference
 * between a page walk on most memory accesses and almost none.
 *
 * User address spaces are 4-level (or 5-level where supported) with their own
 * PML4. The kernel's own PML4 entries are shared with a user PML4 so that a
 * process running in ring 3 can still reach kernel code for a syscall entry:
 * on a syscall the CPU switches to the kernel PML4, and having the entries
 * already present avoids a per-syscall page-table swap cost beyond the CR3 load
 * itself.
 */
#ifndef VMM_H
#define VMM_H

#include <boot.h>
#include <types.h>
#include <spinlock.h>
#include <pmm.h>

/* Page table entry bits. */
#define PTE_PRESENT     (1ULL << 0)
#define PTE_WRITE       (1ULL << 1)
#define PTE_USER        (1ULL << 2)
#define PTE_PWT         (1ULL << 3)   /* write-through */
#define PTE_PCD         (1ULL << 4)   /* cache disabled */
#define PTE_ACCESSED    (1ULL << 5)
#define PTE_DIRTY       (1ULL << 6)
#define PTE_PS          (1ULL << 7)   /* large page (2 MiB in a PD) */
#define PTE_GLOBAL      (1ULL << 8)
#define PTE_NX          (1ULL << 63)
#define PTE_ADDR_MASK   0x000FFFFFFFFFF000ULL
#define PTE_PROTNONE    (1ULL << 63)

/* Address-space protection flags, matching the userspace PROT_* values. */
#define VM_READ    0x1
#define VM_WRITE   0x2
#define VM_EXEC    0x4
#define VM_USER    0x8

/* Mapping flags. */
#define VM_FIXED   0x0100   /* must land at exactly `addr` */
#define VM_ANON    0x0200   /* zero-filled, no backing file */
#define VM_ZERO    0x0400   /* zero the existing contents */
#define VM_HUGE    0x0800   /* use 2 MiB pages where possible */

/* Flags for the recursive-map self-reference. */
#define VMM_RECURSE_PML4_INDEX  511
#define VMM_RECURSE_OFFSET      0xFFFFFF8000000000ULL

/*
 * Recursive mapping -- defined here, but not built.
 *
 * Mapping a page table entry into itself at a known offset makes the whole page
 * table tree reachable as ordinary memory, so any level can be read or written
 * with a single load instead of a walk of up to four dependent ones. A walk of
 * dependent loads is the single largest cost in a page-fault handler, so this
 * is a real optimisation.
 *
 * It is not built yet, and the reason is the ordering. The full form needs 512
 * PDs and 512*512 PTs -- 16 MiB of page tables, or 2 MiB if only the top three
 * levels are mapped -- and it would have to be built at the one moment this
 * code cannot afford a large fixed cost: vmm_init() runs before the physical
 * allocator exists, so the first tables come from a static pool inside the
 * kernel image.
 *
 * vmm.c therefore uses an ordinary four-level walk. The helpers below describe
 * where the tables land and are correct for any page table that *is*
 * recursively mapped, but nothing is recursively mapped today. They are kept
 * so that when a full map becomes affordable, the walk in vmm.c is the only
 * thing that has to change.
 *
 * Do not assume a recursive map exists anywhere in code that runs before
 * vmm_init() completes. It does not, at any level.
 */
#define RECURSIVE_PML4_ENTRY  (VMM_RECURSE_PML4_INDEX * 512 + 511)
/* PML4_ENTRY_OF, PDPT_ENTRY_OF, PD_ENTRY_OF and PT_ENTRY_OF live in boot.h,
 * next to KERNEL_VIRT_BASE: the loader fills the higher half with them and so
 * does this file, and two copies of an index formula is one copy too many. */

static inline void *rec_pml4(void)  { return (void *)(VMM_RECURSE_OFFSET + 0x0000ull); }
static inline void *rec_pdpte(void) { return (void *)(VMM_RECURSE_OFFSET + 0x1000ull); }
static inline void *rec_pd(void)   { return (void *)(VMM_RECURSE_OFFSET + 0x2000ull); }
static inline void *rec_pt(void)   { return (void *)(VMM_RECURSE_OFFSET + 0x3000ull); }

/* One virtual address per index: with the recursive map, entry `i` of a table
 * at level L is simply VMM_RECURSE_OFFSET + L*0x1000 + i*8. */
static inline uint64_t *recursive_entry(unsigned level, unsigned index)
{
	return (uint64_t *)(VMM_RECURSE_OFFSET + (uint64_t)level * 0x1000 +
			    (uint64_t)index * 8);
}

/* Kernel virtual memory layout, matching src/kernel/link.ld and boot.h. */
#define VMM_KERNEL_BASE    PHYS_DIRECT_MAP
#define VMM_VMALLOC_BASE   VMALLOC_AREA
#define VMM_VMALLOC_END    0xFFFFC00080000000ULL
#define VMM_VMALLOC_SIZE   (VMM_VMALLOC_END - VMM_VMALLOC_BASE)
#define VMM_KSTACK_BASE    0xFFFFC00080000000ULL
#define VMM_KSTACK_END     0xFFFFC00084000000ULL
#define VMM_KSTACK_SIZE    (VMM_KSTACK_END - VMM_KSTACK_BASE)

/*
 * A virtual memory area. VMAs describe ranges with uniform properties; a
 * process address space is a set of them.
 *
 * The list is kept sorted by ascending start address, and mm_find_vma() walks it
 * in order and stops at the first VMA whose end is above the address. That is
 * O(n) in the number of VMAs, not the O(log n) an earlier draft of this comment
 * claimed -- a linked list has no random access, so a binary search is not
 * available without a second index to keep in step. In practice a process has
 * tens of VMAs, and the walk is pointer-chasing over memory the caller is
 * already holding under mm->lock, so it measures far better than the count
 * suggests. If this ever shows up in a profile, the fix is an ordered array or
 * an rbtree in struct address_space, and a change to mm_find_vma alone.
 */
struct vma {
	virt_addr_t start;          /* page aligned, inclusive */
	virt_addr_t end;            /* page aligned, exclusive */
	uint32_t prot;              /* VM_READ | VM_WRITE | VM_EXEC | VM_USER */
	uint32_t flags;             /* VM_ANON, VM_FIXED, ... */
	phys_addr_t phys_base;      /* for file-backed or pre-mapped regions */
	void *file;                 /* backing file, or NULL */
	uint64_t file_offset;          /* offset into the backing file */
	struct list_head list;
} __packed;

/*
 * A process address space.
 *
 * `pgd` is the physical address of the PML4. It is stored as a physical address
 * rather than a pointer because the recursive map is keyed off the *current*
 * CR3: dereferencing this PML4 requires a context switch to it.
 */
struct address_space {
	phys_addr_t pgd;
	spinlock_t lock;
	struct list_head vma_list;
	uint64_t vma_count;

	virt_addr_t start_code, end_code;
	virt_addr_t start_data, end_data;
	virt_addr_t start_brk, brk;
	virt_addr_t stack_top;

	/* Randomised mmap region base, per ASLR policy. */
	virt_addr_t mmap_base;
	virt_addr_t mmap_next;

	/* Refcount on the address space: threads sharing it (CLONE_VM) hold
	 * references so the last one to exit frees the page tables. */
	volatile s32 refcount;
};

/*
 * Initialise the kernel's own address space: build a PML4 with the recursive
 * entry, map the direct map, map the kernel image, and switch CR3 to it.
 *
 * Must run before anything else allocates or touches a large array, because the
 * moment CR3 is replaced the stage2 bootstrap tables are gone.
 */
void vmm_init(void);

/* Switch the current CPU to the kernel's master PML4. Called on every CPU
 * during bring-up. */
void vmm_switch_to_kernel_pgd(void);

/*
 * Physical to virtual through the direct map. This is a pure computation, not a
 * lookup, which is what makes it usable from contexts where taking a lock is not
 * allowed.
 */
static inline void *phys_to_virt(phys_addr_t phys)
{
	return (void *)(PHYS_DIRECT_MAP + phys);
}

static inline phys_addr_t virt_to_phys_direct(virt_addr_t virt)
{
	return virt - PHYS_DIRECT_MAP;
}

/*
 * A pointer to a fixed low physical address, in whichever map is live.
 *
 * Before vmm_switch_to_kernel_pgd() the bootloader's identity view of the low
 * 4 GiB is still installed, so a physical address is already a usable virtual
 * address. After the CR3 write that view is gone and the direct map is the
 * only route to the same bytes. Nothing in the kernel holds a low physical
 * address across the switch for long -- the VGA text buffer and the framebuffer
 * the firmware reported are the two that do -- and using the wrong map is a
 * #PF on the next store to it, and only on the next store.
 */
void *vmm_boot_ptr(phys_addr_t phys);

/*
 * Convert a kernel *link* address to its physical address.
 *
 * This is not virt_to_phys_direct(), and using the wrong one is a silent
 * failure rather than a fault. The direct map starts at 0xFFFF800000000000 and
 * the kernel window at 0xFFFFFFFF80000000; the two are 8 exabytes apart, so
 * subtracting the wrong base yields a plausible-looking physical address that is
 * nowhere near the machine. Only the physical range the kernel window covers is
 * aliased by it, so this is valid for the kernel image and nothing else.
 *
 * The offset is KERNEL_LANDING_ADDR, not zero. The window is not an identity
 * map of low memory: stage2 cannot address above 1 MiB with INT 13h, so the
 * image is copied to KERNEL_LANDING_ADDR before paging exists, and virtual
 * KERNEL_VIRT_BASE is mapped there. Subtracting KERNEL_VIRT_BASE on its own is
 * exactly the assumption that the window and the image share a base, which they
 * do virtually and not physically, and it puts every result 1 MiB below the page
 * that holds it: the bootstrap page tables land 1 MiB low, and pmm_init() is
 * handed the address of an E820 copy that is not where this kernel's copy is.
 */
static inline phys_addr_t kernel_virt_to_phys(virt_addr_t virt)
{
	return (virt - KERNEL_VIRT_BASE) + KERNEL_LANDING_ADDR;
}

/* ------------------------------------------------------------- vmalloc ----- */

/*
 * Kernel virtual allocations of arbitrary size, backed by non-contiguous
 * frames. Suitable above roughly a page, where contiguity stops being
 * reasonable to require. Returns the direct-map alias of the mapping.
 */
void *vmalloc(size_t size);
void *vmalloc_aligned(size_t size, size_t alignment);
void vfree(void *addr);

/*
 * Reserve a virtual range without backing it, or hand out raw pages during
 * early boot before vmalloc's own metadata exists. Used by pmm_init() to place
 * its bitmaps when no contiguous run is available.
 */
void *vmalloc_raw(size_t size);

/* ---------------------------------------------------------- page tables --- */

/* Allocate `count` contiguous page-table pages and zero them. */
phys_addr_t pt_alloc_zeroed(unsigned count);
void pt_free(phys_addr_t pgd, unsigned count);

static inline uint64_t *pt_virt(phys_addr_t pgd)
{
	return (uint64_t *)(uintptr_t)(PHYS_DIRECT_MAP + pgd);
}

/* Map one 4 KiB page in `pgd`'s address space. */
int vmm_map_page(phys_addr_t pgd, virt_addr_t virt, phys_addr_t phys,
		 uint32_t prot);

/* Unmap one page. Returns the physical frame that was mapped, or 0. */
phys_addr_t vmm_unmap_page(phys_addr_t pgd, virt_addr_t virt);

/*
 * Translate a virtual address through a PML4. Returns the physical address of
 * the frame and, through `leaf`, a pointer to the leaf PTE so the caller can
 * inspect or modify the flags.
 */
phys_addr_t vmm_translate(phys_addr_t pgd, virt_addr_t virt, uint64_t **leaf);

/* Populate all leaf entries in a large-page-aligned range. */
int vmm_map_large(phys_addr_t pgd, virt_addr_t virt, phys_addr_t phys,
		  uint64_t count_pages_2mib, uint32_t prot);

/* -------------------------------------------------------- address spaces -- */

/* Create an empty address space with the kernel's entries copied in. */
struct address_space *mm_create(void);

/* Drop a reference; the last one frees every VMA and the page tables. */
void mm_put(struct address_space *mm);

/* Add a reference. */
void mm_get(struct address_space *mm);

/*
 * Add a VMA. Returns 0 on success or a negative errno. Rejects ranges that
 * overlap an existing VMA unless VM_FIXED was requested for that exact range.
 */
int mm_add_vma(struct address_space *mm, virt_addr_t start, virt_addr_t end,
	       uint32_t prot, uint32_t flags);

int mm_remove_vma(struct address_space *mm, virt_addr_t start, virt_addr_t end);

/* Find the VMA containing `addr`. Caller must hold mm->lock. */
struct vma *mm_find_vma(struct address_space *mm, virt_addr_t addr);

/* A contiguous, unmapped, page-aligned range of `len` bytes. */
virt_addr_t mm_find_free(struct address_space *mm, size_t len, size_t align);

/* --------------------------------------------------------------- kmalloc --- */

/*
 * General kernel allocator. Objects up to 4 KiB come from the SLAB caches,
 * larger ones from vmalloc. The cutoff is the largest power-of-two class in
 * docs/src/memory/slab-allocator.md. A hypothetical 8 KiB class would need a
 * 32 KiB slab to hold more than one object, so every such allocation would cost
 * four times its own size; vmalloc is the better path for objects that large.
 */
/* Bring up the SLAB caches. Must run after pmm_init(), since the first slab
 * request allocates buddy pages, and before the first kmalloc(). */
void kmalloc_init(void);

void *kmalloc(size_t size);
void *kmalloc_zeroed(size_t size);
void *kcalloc(size_t n, size_t size);
void *krealloc(void *ptr, size_t old_size, size_t new_size);
void kfree(void *ptr, size_t size);
char *kstrdup(const char *s);

/* Statistics, for the observability interface. */
void kmalloc_stats(u64 *allocs, u64 *frees, u64 *bytes_in_use);

/* ----------------------------------------------------------- page faults --- */

/*
 * Handle a page fault in a named address space.
 *
 * Called from the #PF handler with the faulting CR2 address, the error code, and
 * the address space to resolve against. Resolves the faulting VMA, allocates or
 * maps a frame, and returns 0 so the caller can retry the instruction.
 *
 * `mm` is a parameter rather than an implicit "the current one" because the two
 * callers are not the same case. The #PF handler passes the faulted task's
 * space, which does happen to be current, but the ELF loader does not: execve
 * builds a brand-new address space and copies the image into it while the CPU is
 * still running on the old PML4. An implicit current-space lookup there would
 * resolve the VMA in one address space and install the PTE in the other, which
 * succeeds and maps nothing.
 *
 * Returns 0 if handled, -EFAULT if the address is backed by nothing (the caller
 * delivers SIGSEGV), -EACCES if the VMA exists but refuses the access, and
 * -ENOMEM if a frame could not be allocated.
 */
long vmm_handle_page_fault(struct address_space *mm, virt_addr_t addr,
			   uint64_t error_code);

/* The address space of the task running on this CPU, or NULL before the first
 * task exists. The #PF handler uses it; nothing else should assume the current
 * address space is the interesting one. */
struct address_space *current_mm(void);

/* -------------------------------------------------------- kernel stacks ---- */

/*
 * Kernel stacks live in a dedicated vmalloc range rather than on the general
 * vmalloc heap, so that a stack overflow hits a guard page instead of
 * corrupting a neighbouring allocation. The guard is an unmapped hole below
 * each stack, and the mapping below the guard is PROT_NONE, so a runaway
 * recursion faults deterministically instead of quietly eating the heap.
 */
void *kstack_alloc(size_t size);
void kstack_free(void *stack);

/* The base of the current CPU's interrupt stack. */
void *irq_stack_top(void);

#endif /* VMM_H */
