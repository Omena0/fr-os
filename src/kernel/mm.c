/*
 * mm.c — address spaces and the virtual memory areas inside them.
 *
 * A process address space is a PML4 plus a sorted list of VMAs. The VMAs
 * describe what the address space *means*; the page tables describe what is
 * currently *there*. The two are deliberately allowed to disagree: a VMA is a
 * promise that a range is the process's to use, and nothing in it is backed
 * until a fault asks for a specific page. That is what makes an 8 MiB user
 * stack with a 4 KiB footprint cost one page table, and it is why this file
 * owns the fault path rather than delegating it.
 *
 * Everything here is called from two very different places — a syscall making
 * a normal kernel call, and the #PF handler running with interrupts already
 * off — so no function may sleep, and every one that touches the list takes
 * the address space lock with spinlock_irqsave() rather than a bare acquire.
 *
 * The one structural deviation from the design in vmm.h: the VMA list is
 * searched linearly. vmm.h's comment describes a sorted array searched in
 * O(log n), but struct address_space carries a list_head and a count, not an
 * array, and a linked list has no random access for a binary search to use.
 * The search below early-exits on the first candidate rather than pretending
 * to be logarithmic. A process in this kernel has on the order of ten VMAs, so
 * the difference is not measurable; a process with ten thousand would want the
 * rbtree that the header's comment already describes but the struct does not
 * carry. See the note in the report.
 */
#include <stdbool.h>
#include <io.h>
#include <types.h>
#include <list.h>
#include <spinlock.h>
#include <klog.h>
#include <panic.h>
#include <kstring.h>
#include <percpu.h>
#include <pmm.h>
#include <task.h>
#include <vmm.h>
#include <interrupt.h>
#include <uapi/errno.h>

KLOG_SUBSYSTEM("mm");

/* ------------------------------------------------------ user layout -------- */

/*
 * The user half of the address space.
 *
 * USER_ADDRESS_MAX is the same boundary process.c gates user pointers on, and
 * the two must agree: a range that this file will happily place a VMA in but
 * copy_to_user() refuses to touch would be a range where a process can mmap
 * memory it can never read. The kernel half above 0xFFFF800000000000 is
 * reachable only through the copied PML4 entries and is never described by a
 * VMA, so a fault there finds nothing and becomes a signal or a panic rather
 * than a mapping.
 */
#define MM_USER_LIMIT   0x0000800000000000ULL

/* The user stack, matching process.c's USER_STACK_BASE/TOP. process.c adds the
 * VMA itself, so these are only the defaults written into a fresh address
 * space for a caller that does not. */
#define MM_STACK_TOP    0x00007FFFFFFFF000ULL
#define MM_STACK_SIZE   (8ULL << 20)
#define MM_STACK_BASE   (MM_STACK_TOP - MM_STACK_SIZE)

/*
 * The program break starts well above a typical ET_EXEC load address and well
 * below the mmap region, so a growing heap never walks into a mapping the
 * loader put there. elf.c moves both start_brk and brk up to the end of the
 * image once it knows where that is; this is only the floor for an address
 * space that has not been through a loader.
 */
#define MM_BRK_BASE     0x0000100000000000ULL

/*
 * mmap grows upward from here, which is also the seed elf.c uses when it finds
 * mmap_base still zero. It is far enough below the stack that a process cannot
 * realistically run out of mmap space before the stack, and the ceiling for
 * both is the same, so a runaway mmap fails rather than colliding with the
 * stack.
 */
#define MM_MMAP_BASE    0x0000200000000000ULL
#define MM_MMAP_LIMIT   MM_STACK_BASE

/* ------------------------------------------------------------- vmas -------- */

/*
 * One VMA, zeroed and unlinked.
 *
 * vmalloc rather than kmalloc: a VMA is 64 bytes and lives for the life of the
 * process, so the allocation rate is a handful per mapping operation rather
 * than a hot path, and vmalloc's slab-free first-fit is the cheaper thing to
 * carry into every fault. The block comes back zeroed anyway, but vfree() only
 * releases the block for reuse and the next allocation may be the slab's
 * poisoned-memory path on some other allocator, so the memset is what actually
 * establishes the invariant.
 */
static struct vma *vma_alloc(void)
{
	struct vma *v = vmalloc(sizeof(*v));

	if (!v)
		return NULL;

	memset(v, 0, sizeof(*v));
	list_init(&v->list);
	return v;
}

static void vma_free(struct vma *v)
{
	if (v)
		vfree(v);
}

static void vma_copy(struct vma *dst, const struct vma *src)
{
	dst->start = src->start;
	dst->end = src->end;
	dst->prot = src->prot;
	dst->flags = src->flags;
	dst->phys_base = src->phys_base;
	dst->file = src->file;
	dst->file_offset = src->file_offset;
}

/*
 * Locate the insertion point for a VMA starting at `start`, and note whether it
 * would overlap an existing one.
 *
 * Insertion and the overlap test are the same walk because a sorted list makes
 * them the same question: the VMA that would follow the new one is the first
 * candidate for an overlap, and everything before it is strictly below. The
 * caller uses the two results to insert and to reject, so the list is walked
 * once rather than twice and, more importantly, the decision to insert and the
 * decision to reject can never be made from two different views of the list.
 *
 * `overlap` reports a VMA that *intersects* [start, end), in either direction:
 * one that starts inside the range and one that starts before it and reaches
 * into it. A VMA is half-open, so one that begins exactly at `end` is disjoint
 * and is not reported -- which is what lets an adjacent mapping, a split half
 * of a VMA and the tail of a punch all be re-inserted without a false positive.
 * `before` is the node the caller must insert *after*; see the note on
 * list_add() in mm_add_vma().
 */
static void vma_insertion_point(struct address_space *mm, virt_addr_t start,
				virt_addr_t end, struct list_head **before,
				bool *overlap)
{
	struct list_head *pos;

	*before = NULL;
	*overlap = false;

	list_for_each(pos, &mm->vma_list) {
		struct vma *v = list_entry(pos, struct vma, list);

		/* Half-open: [v->start, v->end) starts at v->end at the earliest,
		 * so a VMA beginning at `end` shares no page with the range and
		 * neither it nor anything after it can overlap. Testing `>` here
		 * instead of `>=` reported those as overlaps, which turned every
		 * re-insert of a freed range into -EEXIST. */
		if (v->start >= end)
			break;
		/* Both halves matter. `v->start >= start` alone misses a VMA that
		 * begins below the range and extends up into it -- the case where a
		 * new mapping is dropped into the middle of an existing one, which
		 * then overlaps it and lands out of order. */
		if (v->end > start)
			*overlap = true;
		*before = pos;
	}

	if (*before == NULL)
		*before = &mm->vma_list;
}

struct vma *mm_find_vma(struct address_space *mm, virt_addr_t addr)
{
	struct list_head *pos;

	if (!mm)
		return NULL;

	/*
	 * The list is sorted by start and the VMAs do not overlap, so the first
	 * entry that ends past `addr` is the only one that can contain it: every
	 * later entry starts at or after that one's start, which is already past
	 * `addr`. Returning on the first candidate rather than comparing the
	 * whole list is what makes the stack, the heap and the most recent mmap
	 * all cost a fraction of the walk.
	 *
	 * The caller holds mm->lock. Taking it here instead would deadlock the
	 * fault path, which already holds the lock across the VMA lookup and
	 * the mapping that has to happen before the VMA can be released.
	 */
	list_for_each(pos, &mm->vma_list) {
		struct vma *v = list_entry(pos, struct vma, list);

		if (v->end <= addr)
			continue;
		if (v->start > addr)
			return NULL;
		return v;
	}

	return NULL;
}

/* ------------------------------------------------- address spaces ----------- */

/*
 * The kernel's own PML4, as a physical address.
 *
 * vmm.c keeps its PML4 in a file-static and does not export it, which is the
 * right encapsulation for everything that uses the recursive map: those callers
 * never name the PML4 at all. mm_create() does have to name it, and it can
 * read it out of CR3 instead. That is exactly as authoritative as the static —
 * CR3 is where the running kernel's PML4 physically is — and it stays correct
 * for the case that matters most: mm_create() is called from a syscall, where
 * the CPU is running on the *process's* PML4. That PML4 was itself cloned
 * from the kernel's, so copying its high-half entries reproduces the kernel
 * window and the direct map, which is precisely what the new address space
 * needs in order to be able to serve a syscall at all.
 *
 * The low 12 bits are PCID, not address, and are cleared here so the value can
 * be used with phys_to_virt() unconditionally.
 */
static phys_addr_t kernel_pgd_from_cr3(void)
{
	return read_cr3() & ~0xFFFULL;
}

struct address_space *mm_create(void)
{
	phys_addr_t kernel_pgd = kernel_pgd_from_cr3();
	phys_addr_t pgd;
	struct address_space *mm;
	uint64_t *src, *dst;

	mm = vmalloc(sizeof(*mm));
	if (!mm)
		return NULL;

	pgd = pt_alloc_zeroed(1);
	if (!pgd) {
		vfree(mm);
		return NULL;
	}

	/*
	 * Two entries and nothing else. 256 is the direct map, which every piece
	 * of kernel code reaches through phys_to_virt(); 511 is the kernel's
	 * negative canonical half, which is where the image, vmalloc and the
	 * kernel stacks live. Everything between them is the user half, and a new
	 * address space must start with none of it mapped — an address space that
	 * inherited the parent's user mappings would make every VMA a description
	 * of something that may or may not still be there.
	 *
	 * The entries are shared, not copied: both address spaces point at the
	 * same PDPT pages. That is the whole reason the split is cheap, and it is
	 * also why the teardown below has to leave indices 256 and 511 alone.
	 */
	src = (uint64_t *)phys_to_virt(kernel_pgd);
	dst = (uint64_t *)phys_to_virt(pgd);
	dst[256] = src[256];
	dst[511] = src[511];

	memset(mm, 0, sizeof(*mm));
	mm->pgd = pgd;
	spinlock_init(&mm->lock);
	list_init(&mm->vma_list);
	mm->vma_count = 0;

	mm->start_brk = MM_BRK_BASE;
	mm->brk = MM_BRK_BASE;
	mm->stack_top = MM_STACK_TOP;
	mm->mmap_base = MM_MMAP_BASE;
	mm->mmap_next = MM_MMAP_BASE;
	mm->refcount = 1;

	klog(KLOG_DEBUG, "mm: address space pgd %#lx (kernel pgd %#lx)\n",
	     (unsigned long)pgd, (unsigned long)kernel_pgd);
	return mm;
}

void mm_get(struct address_space *mm)
{
	if (!mm)
		return;
	/* Relaxed: the new reference protects nothing the previous holder has
	 * not already published, and the only ordering that matters is the
	 * release side, which is in mm_put(). */
	__atomic_add_fetch(&mm->refcount, 1, __ATOMIC_RELAXED);
}

/* ------------------------------------------------- page table teardown ------ */

/*
 * Free one page table and everything below it.
 *
 * `level` is the architectural level: 3 a PDPT, 2 a PD, 1 a PT. Leaf frames
 * go back to pmm on the way out, because leaving them would turn every process
 * exit into a slow leak of exactly the memory the process was using.
 *
 * A PS entry below level 1 is a large leaf — 2 MiB, or 1 GiB at the PDPT level
 * — and the frames behind it cannot be handed back individually without
 * splitting the mapping first. Nothing in the user path creates one: vmm.c's
 * vmm_map_page() always builds 4 KiB leaves, and the only 2 MiB and 1 GiB
 * mappings in the system are the direct map, which is never reached from here
 * because the caller skips the two PML4 entries shared with the kernel. The
 * case is therefore a leak on a path that does not exist yet, and a leak is
 * the right answer to a path that does not exist yet.
 */
static void free_page_table(phys_addr_t table, unsigned level)
{
	uint64_t *entries = (uint64_t *)phys_to_virt(table);

	for (unsigned i = 0; i < 512; i++) {
		uint64_t e = entries[i];

		if (!(e & PTE_PRESENT))
			continue;
		if (level == 1) {
			pmm_free_pages(phys_to_page(e & PTE_ADDR_MASK), 0);
		} else if (e & PTE_PS) {
			continue;
		} else {
			free_page_table(e & PTE_ADDR_MASK, level - 1);
		}
	}

	pmm_free_pages(phys_to_page(table), 0);
}

/*
 * Tear down every page table this address space owns.
 *
 * Indices 256 and 511 are skipped, and not out of caution: those PDPT pages
 * were *shared* with the kernel's PML4 by mm_create(), so freeing them would
 * free the direct map out from under every other address space on the machine
 * and under the kernel itself. The other 510 entries were reached only through
 * this PML4, so anything below them is provably this address space's own and
 * is freed.
 *
 * The cost of being conservative here is that a forked child, whose PML4 was
 * deep-copied by process.c including the direct map, leaks that copy. The
 * distinction is not visible from the PML4's contents — the kernel's and the
 * child's entries for 256 and 511 are indistinguishable — so it would take a
 * flag in struct address_space to record it, and the header does not have one.
 */
static void mm_free_page_tables(phys_addr_t pgd)
{
	uint64_t *entries = (uint64_t *)phys_to_virt(pgd);

	for (unsigned i = 0; i < 512; i++) {
		uint64_t e = entries[i];

		if (i == 256 || i == 511)
			continue;
		if (!(e & PTE_PRESENT) || (e & PTE_PS))
			continue;

		free_page_table(e & PTE_ADDR_MASK, 3);
		entries[i] = 0;
	}

	pmm_free_pages(phys_to_page(pgd), 0);
}

void mm_put(struct address_space *mm)
{
	phys_addr_t pgd;
	u64 flags;

	if (!mm)
		return;

	/* Acq_rel pairs with mm_get()'s relaxed increment: the releasing CPU's
	 * writes have to be visible to whichever CPU observes the count reach
	 * zero, or that CPU frees a VMA another CPU is still walking. */
	if (__atomic_sub_fetch(&mm->refcount, 1, __ATOMIC_ACQ_REL) != 0)
		return;

	flags = spinlock_irqsave(&mm->lock);

	/* The page tables go before the VMAs, because freeing a table already
	 * returns every frame the VMAs' pages occupied. Walking the VMAs
	 * afterwards to unmap them would find nothing. */
	pgd = mm->pgd;
	mm->pgd = 0;
	mm_free_page_tables(pgd);

	struct list_head *pos, *tmp;

	list_for_each_safe(pos, tmp, &mm->vma_list) {
		struct vma *v = list_entry(pos, struct vma, list);

		list_del(&v->list);
		vma_free(v);
	}
	mm->vma_count = 0;

	spinlock_unlock_irqrestore(&mm->lock, flags);

	vfree(mm);
}

/* --------------------------------------------------------- vm manip --------- */

/*
 * Drop [start, end) from the VMA list, splitting a straddling VMA in two.
 *
 * The caller holds mm->lock. Returns -ENOMEM having changed nothing if the
 * split cannot be allocated, so every failure path leaves the list exactly as
 * it was.
 */
static int vma_punch(struct address_space *mm, virt_addr_t start, virt_addr_t end)
{
	struct vma *head = NULL;
	struct vma *tail = NULL;
	struct list_head *pos, *tmp;
	int rc = 0;

	/*
	 * Two VMAs can need attention, and they can be the same one: the VMA
	 * containing `start` keeps everything below it, and the VMA containing
	 * `end` keeps everything above it. On a sorted, non-overlapping list at
	 * most one contains each, so one pass finds both.
	 *
	 * The tail is allocated here, before anything is modified, which is what
	 * makes the removal itself infallible: a caller that gets -ENOMEM here
	 * gets an untouched address space rather than one missing a VMA it was
	 * told still exists.
	 *
	 * Note what the tail's existence is keyed on: `v->end > end`, not
	 * "`head` straddles". A VMA that contains `start` but ends before `end`
	 * contributes no tail -- everything from its end to `end` is being
	 * removed anyway -- and the VMA containing `end` is a separate entry
	 * that the second pass trims in place.
	 */
	list_for_each(pos, &mm->vma_list) {
		struct vma *v = list_entry(pos, struct vma, list);

		if (v->end <= start)
			continue;
		if (v->start >= end)
			break;
		if (v->start < start) {
			head = v;
			if (v->end > end) {
				tail = vma_alloc();
				if (!tail) {
					rc = -ENOMEM;
					goto out;
				}
				vma_copy(tail, v);
				tail->start = end;
			}
			continue;
		}
		/* Not the head, so there is nothing to pre-allocate: whatever this
		 * is, the second pass either deletes it whole or moves its start. */
		break;
	}

	list_for_each_safe(pos, tmp, &mm->vma_list) {
		struct vma *v = list_entry(pos, struct vma, list);

		if (v->end <= start)
			continue;
		if (v->start >= end)
			break;

		if (v->start >= start && v->end <= end) {
			list_del(&v->list);
			mm->vma_count--;
			vma_free(v);
			continue;
		}

		/* The head. Its end moves up to `start`, and the piece that
		 * survives past `end` -- which is the tail when this is the same
		 * VMA -- is re-inserted after it, so the list stays sorted even in
		 * the middle.
		 *
		 * `continue`, not `break`: the VMA containing `end` is a later
		 * entry whenever the gap between the two VMAs is smaller than the
		 * range, which is the common case for an mmap region. Breaking here
		 * left that entry whole and moved this one's *start* to `end`
		 * instead, producing an interval with end <= start and unmapping
		 * nothing at all. */
		if (v == head) {
			v->end = start;
			if (tail) {
				list_add(&tail->list, &v->list);
				mm->vma_count++;
				tail = NULL;
			}
			continue;
		}

		/* The tail, when the removal stops inside a VMA. */
		v->start = end;
		break;
	}

	/*
	 * A range that was never mapped is not an error: POSIX has munmap()
	 * succeed on an address range it holds no mapping for, and syscall.c
	 * returns this value directly to userspace.
	 */
out:
	if (tail)
		vma_free(tail);
	return rc;
}

int mm_add_vma(struct address_space *mm, virt_addr_t start, virt_addr_t end,
	       uint32_t prot, uint32_t flags)
{
	struct vma *v;
	struct list_head *before;
	bool overlap;
	int rc = 0;

	if (!mm)
		return -EINVAL;
	/* An empty range is not a VMA. Accepting one would put an entry into a
	 * sorted list that no lookup can ever match and that every gap scan has
	 * to step over. */
	if (end <= start)
		return -EINVAL;
	/* Unaligned bounds are a caller bug rather than something to round: the
	 * caller's idea of where its mapping ends would silently differ from the
	 * VMA's, and the next fault on the tail page would find no VMA for an
	 * address the caller believes it owns. */
	if (!IS_ALIGNED(start, PAGE_SIZE) || !IS_ALIGNED(end, PAGE_SIZE))
		return -EINVAL;
	/* Same reasoning for the user half: a VMA above the limit describes
	 * kernel addresses, which are mapped by the shared PML4 entries and are
	 * not this file's to hand out. */
	if (start >= MM_USER_LIMIT || end > MM_USER_LIMIT)
		return -EINVAL;

	/* Allocated before the lock so the critical section stays a list walk.
	 * vfree() on the error path takes vmalloc's own lock, which is a leaf, so
	 * doing it outside is strictly better and not merely tidier. */
	v = vma_alloc();
	if (!v)
		return -ENOMEM;

	u64 irqflags = spinlock_irqsave(&mm->lock);

	vma_insertion_point(mm, start, end, &before, &overlap);

	if (overlap && !(flags & VM_FIXED)) {
		rc = -EEXIST;
		goto out;
	}

	if (overlap) {
		/*
		 * VM_FIXED: the new mapping replaces whatever is there, exactly as
		 * MAP_FIXED replaces rather than merges. Punching the range first
		 * is what keeps the list a set of non-overlapping intervals —
		 * unlinking only the last overlapping VMA would leave the new entry
		 * overlapping the ones before it, and every gap scan and every
		 * lookup would then be walking a list it cannot reason about.
		 *
		 * The overlap may need a VMA to be split, which is why this can
		 * fail at all; on failure nothing has been changed.
		 */
		rc = vma_punch(mm, start, end);
		if (rc < 0)
			goto out;
		vma_insertion_point(mm, start, end, &before, &overlap);
	}

	v->start = start;
	v->end = end;
	v->prot = prot;
	v->flags = flags;
	/*
	 * list_add(), not list_add_tail(). vma_insertion_point() returns the
	 * node the new VMA belongs *after* -- the last one that starts below
	 * `end` -- and list_add() is "insert immediately after pos".
	 * list_add_tail() splices in at pos->prev, so it put every VMA but the
	 * first one position too early, and the first insertion into a
	 * non-empty list landed at the head: the list came out reversed, and
	 * mm_find_vma() then returned NULL for an address the process owns,
	 * because it early-returns on the first entry that starts above addr.
	 * On the empty list both forms agree, which is why one VMA looked fine.
	 */
	list_add(&v->list, before);
	mm->vma_count++;

out:
	spinlock_unlock_irqrestore(&mm->lock, irqflags);
	if (rc < 0)
		vma_free(v);
	return rc;
}

int mm_remove_vma(struct address_space *mm, virt_addr_t start, virt_addr_t end)
{
	u64 flags;
	int rc;

	if (!mm)
		return -EINVAL;
	if (end <= start)
		return -EINVAL;
	if (!IS_ALIGNED(start, PAGE_SIZE) || !IS_ALIGNED(end, PAGE_SIZE))
		return -EINVAL;
	/* The same user-half bound mm_add_vma() applies. syscall.c's
	 * mmap(MAP_FIXED) punches the range and *then* adds, discarding this
	 * function's return value, so a request above the limit used to destroy
	 * the process's own mappings there and fail the add afterwards. Symmetry
	 * with the insert is what makes the pair refuse instead. */
	if (start >= MM_USER_LIMIT || end > MM_USER_LIMIT)
		return -EINVAL;

	flags = spinlock_irqsave(&mm->lock);
	rc = vma_punch(mm, start, end);
	spinlock_unlock_irqrestore(&mm->lock, flags);
	return rc;
}

virt_addr_t mm_find_free(struct address_space *mm, size_t len, size_t align)
{
	struct list_head *pos;
	virt_addr_t cand, limit;

	if (!mm || !len)
		return 0;

	if (align < PAGE_SIZE)
		align = PAGE_SIZE;
	/* An alignment that is not a power of two cannot be satisfied by the
	 * mask arithmetic below, and asking for one is a caller bug rather than
	 * something to round to the nearest power of two. */
	if (align & (align - 1))
		return 0;

	len = ALIGN_UP(len, PAGE_SIZE);
	if (len > MM_MMAP_LIMIT)
		return 0;

	u64 flags = spinlock_irqsave(&mm->lock);

	/* Start above everything handed out so far rather than at mmap_base:
	 * re-scanning the low holes on every call is what turns a fragmented
	 * address space into an O(n^2) mmap loop. */
	virt_addr_t start = mm->mmap_next > mm->mmap_base ? mm->mmap_next
							  : mm->mmap_base;

	cand = ALIGN_UP(start, align);
	limit = MM_MMAP_LIMIT - len;

	list_for_each(pos, &mm->vma_list) {
		struct vma *v = list_entry(pos, struct vma, list);

		if (v->end <= cand)
			continue;
		if (v->start >= cand + len)
			break;          /* the whole run fits below this VMA */
		cand = ALIGN_UP(v->end, align);
	}

	if (cand <= limit) {
		virt_addr_t got = cand;

		mm->mmap_next = cand + len;
		spinlock_unlock_irqrestore(&mm->lock, flags);
		return got;
	}

	spinlock_unlock_irqrestore(&mm->lock, flags);
	klog(KLOG_DEBUG, "mm: no free range of %#zx bytes below %#lx\n",
	     (size_t)len, (unsigned long)MM_MMAP_LIMIT);
	return 0;
}

/* ------------------------------------------------------- page faults -------- */

/*
 * The address space the CPU is actually running in.
 *
 * A #PF carries the faulting address and nothing else: no pointer to the mm it
 * belongs to. The running task is the only thing that connects the two, and it
 * is the right answer because CR3 belongs to the running task too — the kernel
 * executes on the process's own PML4 in this design, so a CR3 that disagrees
 * with t->mm means the fault is not this task's business at all.
 */
struct address_space *current_mm(void)
{
	struct task *t = current_task();

	if (!t || !t->mm)
		return NULL;
	if ((read_cr3() & ~0xFFFULL) != t->mm->pgd)
		return NULL;
	return t->mm;
}

/*
 * Does this VMA permit the access the CPU reported?
 *
 * The CPU will fault again on the same instruction if the page is mapped with
 * more permission than the VMA grants, so a mapping that ignores this is a way
 * to turn a PROT_READ page into a writable one the first time it faults in.
 * Granting the VMA's protection rather than the fault's is what makes the
 * check below and the mapping above agree.
 */
static bool vma_allows(const struct vma *vma, uint64_t error_code)
{
	if ((error_code & PF_WRITE) && !(vma->prot & VM_WRITE))
		return false;
	if ((error_code & PF_FETCH) && !(vma->prot & VM_EXEC))
		return false;
	if ((error_code & PF_USER) && !(vma->prot & VM_USER))
		return false;
	return true;
}

long vmm_handle_page_fault(struct address_space *mm, virt_addr_t addr,
			   uint64_t error_code)
{
	struct vma *vma;
	struct page *page;
	virt_addr_t virt;
	u64 flags;
	long rc = -EFAULT;

	/*
	 * PF_PRESENT clear means the mapping exists and the access was refused.
	 * That is never a demand fault: it is a permission or protection-key
	 * violation, a reserved-bit violation, or a genuine bug in the kernel.
	 * Filling the page in here would turn "this process may not write that"
	 * into "this process may not write that, once", which is the difference
	 * between a permission check and a suggestion.
	 */
	if (!(error_code & PF_PRESENT))
		return -EACCES;
	if (error_code & PF_RESERVED)
		return -EFAULT;

	virt = addr & PAGE_MASK;

	/* The kernel half is described by the shared PML4 entries, never by a
	 * VMA. Rejecting it here rather than after a list walk keeps a kernel
	 * fault from resolving against a user VMA that happens to be nearby. */
	if (virt >= MM_USER_LIMIT)
		return -EFAULT;

	if (!mm)
		return -EFAULT;

	/* Held across the whole fault, not just the lookup: the page allocated
	 * below has to be mapped while the address space cannot be torn down, and
	 * the VMA cannot be released while its page is being installed. */
	flags = spinlock_irqsave(&mm->lock);

	vma = mm_find_vma(mm, virt);
	if (!vma) {
		klog(KLOG_DEBUG, "mm: fault %#lx, no VMA (err %#lx)\n",
		     (unsigned long)virt, (unsigned long)error_code);
		goto out;
	}

	/*
	 * File-backed mappings are not implementable yet. There is no page
	 * cache, no read path from a struct file, and no offset convention
	 * between a VMA's file_offset and anything on disk, so mapping a
	 * zero-filled page here would hand the process a file it never read:
	 * correct enough to pass a test that only checks the address is mapped,
	 * wrong in every way that matters. The caller turns this into SIGSEGV,
	 * which is the correct outcome for a mapping the kernel cannot honour.
	 */
	if (vma->file) {
		klog(KLOG_DEBUG, "mm: fault %#lx, file-backed VMA unsupported\n",
		     (unsigned long)virt);
		goto out;
	}

	if (!vma_allows(vma, error_code)) {
		klog(KLOG_DEBUG,
		     "mm: fault %#lx denied by VMA prot %#x (err %#lx)\n",
		     (unsigned long)virt, vma->prot, (unsigned long)error_code);
		rc = -EACCES;
		goto out;
	}

	/* Another CPU faulting the same address, or a caller that pre-faulted
	 * through user_memory_write(), left the page present already. Handing
	 * back the frame here rather than mapping it is what stops the retry
	 * from turning into an allocation leak. */
	{
		/* Presence, not "non-zero physical address" -- see vmm_lookup_page(). */
		phys_addr_t have = 0;

		if (vmm_lookup_page(mm->pgd, virt, &have, NULL)) {
			rc = 0;
			goto out;
		}
	}

	/* Zeroed because an anonymous page must read as zero, and pmm's PG_ZEROED
	 * is a hint the caller cannot see: a page recycled from a previous
	 * process, or from a page freed with contents still in it, is not zero.
	 * The 4 KiB of stores per fault is cheaper than a process observing
	 * another process's memory. */
	page = pmm_alloc_page(0);
	if (!page) {
		rc = -ENOMEM;
		goto out;
	}
	memset(phys_to_virt(page_to_phys(page)), 0, PAGE_SIZE);

	if (vmm_map_page(mm->pgd, virt, page_to_phys(page), vma->prot | VM_USER)) {
		pmm_free_pages(page, 0);
		rc = -ENOMEM;
		goto out;
	}

	rc = 0;

out:
	spinlock_unlock_irqrestore(&mm->lock, flags);
	return rc;
}
