/*
 * elf.c — the ELF64 loader.
 *
 * Everything here happens before the image is trusted. A loader that maps
 * segments as it walks the program header table has already handed the kernel
 * to a malformed file by the time it notices the bad entry; so this one
 * validates the whole header, and every program header entry, before it maps a
 * single page. A rejected image leaves the address space exactly as it was.
 *
 * Pages are written through the direct map rather than faulted, because the
 * loader knows the file contents and faulting a page in only to overwrite it
 * with the same bytes would allocate a frame, zero it, take a fault, and then
 * copy — four times the work for one segment. The tail of a segment beyond
 * p_filesz is left unmapped on purpose: the fault path allocates a zeroed
 * anonymous page for it, which is exactly the bss semantics the ELF ABI
 * specifies and costs nothing until the program reads it.
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
#include <process.h>

#define ELF_LOG(level, ...)                                                  \
	do {                                                               \
		if ((level) >= klog_runtime_level)                         \
			klog_emit((level), "elf", __VA_ARGS__);            \
	} while (0)

/* ------------------------------------------------------------- on-disk ------- */

#define EI_NIDENT      16
#define EI_CLASS        4
#define EI_DATA         5
#define EI_VERSION      6

#define ELFCLASS64      2
#define ELFDATA2LSB     1
#define EV_CURRENT      1

#define ET_EXEC         2
#define ET_DYN          3
#define EM_X86_64      62

#define PT_NULL         0
#define PT_LOAD         1
#define PT_DYNAMIC      2
#define PT_INTERP       3
#define PT_NOTE         4
#define PT_PHDR         6
#define PT_TLS          7

#define PF_X            1
#define PF_W            2
#define PF_R            4

struct elf64_ehdr {
	u8 e_ident[EI_NIDENT];
	u16 e_type;
	u16 e_machine;
	u32 e_version;
	u64 e_entry;
	u64 e_phoff;
	u64 e_shoff;
	u32 e_flags;
	u16 e_ehsize;
	u16 e_phentsize;
	u16 e_phnum;
	u16 e_shentsize;
	u16 e_shnum;
	u16 e_shstrndx;
} __packed;

struct elf64_phdr {
	u32 p_type;
	u32 p_flags;
	u64 p_offset;
	u64 p_vaddr;
	u64 p_paddr;
	u64 p_filesz;
	u64 p_memsz;
	u64 p_align;
} __packed;

/* ------------------------------------------------------------ validation ---- */

/*
 * A single bound used everywhere a field from the image is compared against a
 * size in the image. Keeping it in one place is what stops a length check in
 * one function from being stricter than the same check in another.
 */
static bool in_bounds(u64 off, u64 len, u64 total)
{
	return off <= total && len <= total - off;
}

static int elf_validate(const void *image, size_t size, u64 *out_phoff)
{
	const struct elf64_ehdr *eh = image;

	if (size < sizeof(*eh))
		return -ENOEXEC;
	if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
	    eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F')
		return -ENOEXEC;
	if (eh->e_ident[EI_CLASS] != ELFCLASS64)
		return -ENOEXEC;
	if (eh->e_ident[EI_DATA] != ELFDATA2LSB)
		return -ENOEXEC;
	if (eh->e_ident[EI_VERSION] != EV_CURRENT)
		return -ENOEXEC;
	if (eh->e_machine != EM_X86_64)
		return -ENOEXEC;
	if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN)
		return -ENOEXEC;
	if (eh->e_version != EV_CURRENT)
		return -ENOEXEC;

	/*
	 * The entry size must be at least the size of the structure this
	 * loader knows. A larger one is accepted — that is how the format is
	 * meant to evolve — but the stride used to walk the table is the
	 * file's, not this loader's.
	 */
	if (eh->e_phentsize < sizeof(struct elf64_phdr))
		return -ENOEXEC;
	if (eh->e_phnum == 0 || eh->e_phnum > ELF_MAX_PHDRS)
		return -ENOEXEC;
	if (!in_bounds(eh->e_phoff,
		       (u64)eh->e_phnum * eh->e_phentsize, size))
		return -ENOEXEC;

	*out_phoff = eh->e_phoff;
	return 0;
}

static uint32_t phdr_prot(u32 p_flags)
{
	uint32_t prot = 0;

	if (p_flags & PF_R)
		prot |= VM_READ;
	if (p_flags & PF_W)
		prot |= VM_WRITE;
	if (p_flags & PF_X)
		prot |= VM_EXEC;
	/* VM_USER is not a permission the file can grant or withhold: every
	 * mapping this loader creates is a user mapping. */
	return prot | VM_USER;
}

/* ---------------------------------------------------------------- TLS ------- */

/*
 * Thread-local storage, which is the one program header type whose contents
 * are not executed and are not even reachable through an ordinary load: a
 * PT_TLS segment names a block that the *linker* placed and the *compiler*
 * addressed relative to a thread pointer, and the instruction stream never
 * names an address in it. Nothing else in this file would ever touch those
 * bytes, so a loader that skips PT_TLS still produces a process that runs --
 * until the first `__thread` access, which is a load from whatever the thread
 * pointer happens to be. With no way to set that pointer, that is address zero.
 *
 * Two things have to be right, and the second is the one that is easy to get
 * backwards.
 *
 *   1. The block is mapped, including the .tbss tail that no PT_LOAD covers.
 *      For a static image the block usually straddles the end of the last
 *      loadable segment, so a PT_TLS-sized hole in the address space is not a
 *      hole the fault handler can be relied on to fill either, because there
 *      may be no VMA describing it.
 *
 *   2. The thread pointer is not "the end of the block". It is
 *
 *          tp = ALIGN_UP(p_vaddr + p_memsz, p_align)
 *
 *      and the difference is not cosmetic. Local-exec TLS is addressed at
 *      *negative* displacements from FS: the linker assigns each thread-local
 *      symbol an offset counted down from the thread pointer, which is why the
 *      variable that sits lowest in the block has the *largest* negative
 *      displacement. This was measured, not assumed -- see the derivation
 *      below and tests/tls_harness.c, which re-derives it from whatever the
 *      current build produced.
 *
 * Derivation from build/init.elf, which is what libc actually links today:
 *
 *   readelf -lW build/init.elf
 *     TLS  0x008ff0 0x409ff0 0x409ff0 0x000008 0x00000c  R  0x8
 *   readelf -SW build/init.elf
 *     .tdata  0x409ff0  size 8      __libc_tls_sentinel, TLS offset 0
 *     .tbss   0x409ff8  size 4      __errno,             TLS offset 8
 *   objdump -d build/init.elf
 *     401f99: cmp %rax,%fs:0xfffffffffffffff0        -> __libc_tls_sentinel
 *     402457: mov $0xfffffffffffffff8,%rax
 *             movl $0xc,%fs:(%rax)                   -> __errno
 *
 * p_memsz is 0xc, so p_vaddr + p_memsz = 0x409ffc, which is 4 mod 8. Aligning
 * that up to p_align = 8 gives 0x40a000, and that is the only value that puts
 * *both* accesses inside the block:
 *
 *   0x40a000 - 16 = 0x409ff0  = __libc_tls_sentinel
 *   0x40a000 -  8 = 0x409ff8  = __errno
 *
 * Using the unaligned end, 0x409ffc, is what this function did until it was
 * measured. It is off by four and it fails in the worst possible way -- no
 * fault, because every byte it addresses is inside an already-present, writable
 * page:
 *
 *   0x409ffc - 16 = 0x409fec  inside .tdata's page, but not __libc_tls_sentinel
 *   0x409ffc -  8 = 0x409ff4  the *upper four bytes of* __libc_tls_sentinel
 *
 * so libc's first errno write corrupts the sentinel, and the sentinel's own
 * read-back check then fails and refuses to start. A TLS implementation that
 * fails loudly is at least diagnosable; this one failed silently and looked
 * like a linker problem.
 *
 * The rule was then checked against nine synthetic shapes (memsz 1, 4, 5, 8,
 * 0xb, 0x10, 0x20, 0x48 with p_align up to 0x40) and against a second shape
 * with init.elf's exact layout, all of which agree. tests/tls_harness.sh
 * rebuilds that check on demand.
 */

/* Bytes of .tbss zeroed per pass; large enough that the per-call cost of a
 * big block is the loop, not the call. */
#define TLS_ZERO_CHUNK 256

static int elf_load_tls(struct address_space *mm, const u8 *base, u64 offset,
			u64 vaddr, u64 filesz, u64 memsz, u64 align,
			u64 covered_end)
{
	static const u8 zeros[TLS_ZERO_CHUNK];
	u64 start = ALIGN_DOWN(vaddr, PAGE_SIZE);
	u64 end = ALIGN_UP(vaddr + memsz, PAGE_SIZE);
	uint32_t prot = VM_READ | VM_WRITE | VM_USER;
	u64 zero_from, tp;
	int r;

	/*
	 * Only the part no PT_LOAD already covers needs a VMA. The loader adds
	 * VMAs for segment ranges and refuses to overlap one that exists, so
	 * asking for the whole block would fail for every ordinary image: the
	 * block almost always begins inside the last segment's last page.
	 */
	if (end > covered_end) {
		u64 vma_start = start > covered_end ? start : covered_end;

		r = mm_add_vma(mm, (virt_addr_t)vma_start, (virt_addr_t)end,
			       prot, VM_ANON);
		if (r < 0)
			return r;
	}

	/* .tdata: the initialised part, copied from the file. */
	if (filesz) {
		r = user_memory_write(mm, (virt_addr_t)vaddr, base + offset,
				      (size_t)filesz, prot);
		if (r < 0)
			return r;
	}

	/*
	 * .tbss, and only the part of it that is genuinely anonymous.
	 *
	 * This used to zero the whole tail, on the reasoning that a page shared
	 * with the end of a PT_LOAD is present and so no fault would ever fill
	 * it. That reasoning is right about the mechanism and wrong about the
	 * consequence, because the shared bytes are not segment filler: on
	 * build/init.elf .tbss is [0x409ff8,0x409ffc) and .init_array is
	 * [0x409ff8,0x40a000), because a NOBITS section is laid out over
	 * whatever follows it in the data segment. Zeroing the tail there
	 * destroyed the first constructor pointer, so the program's
	 * __attribute__((constructor)) functions silently stopped running.
	 *
	 * Everything below covered_end is live segment content and is left
	 * alone. errno then starts out holding the first four bytes of
	 * __init_array_start rather than zero, which is the documented
	 * TLS-NOBITS-overlay behaviour and is harmless: nothing reads errno
	 * before the first failed call overwrites it.
	 */
	zero_from = vaddr + filesz;
	if (zero_from < covered_end)
		zero_from = covered_end;
	for (u64 at = zero_from; at < vaddr + memsz;) {
		size_t chunk = (size_t)MIN((u64)sizeof(zeros), vaddr + memsz - at);

		r = user_memory_write(mm, (virt_addr_t)at, zeros, chunk, prot);
		if (r < 0)
			return r;
		at += chunk;
	}

	/*
	 * p_align is the segment's declared alignment, which is the largest
	 * alignment of any symbol in the block. It is 1 for a block of bytes
	 * and chars, so ALIGN_UP below has to tolerate it rather than assume a
	 * page or a word.
	 */
	tp = ALIGN_UP(vaddr + memsz, align ? (unsigned long)align : 1UL);
	if (tp < vaddr + memsz)
		tp = vaddr + memsz;   /* only reachable on p_align 0, rejected above */

	mm->tls_ptr = tp;
	mm->tls_size = memsz;

	ELF_LOG(KLOG_INFO,
		"loaded TLS block [%#lx,%#lx) %lu bytes initialised, tp %#lx (p_align %#lx)",
		vaddr, vaddr + memsz, (unsigned long)filesz,
		(unsigned long)mm->tls_ptr, (unsigned long)align);
	return 0;
}


int elf_load(struct address_space *mm, const void *image, size_t size,
	     uint64_t load_bias, struct elf_info *out)
{
	const u8 *base = image;
	const struct elf64_ehdr *eh;
	u64 phoff = 0;
	u64 covered_end = 0;
	u64 min_vaddr = ~0ULL;
	u64 max_vaddr = 0;
	u64 phdr_vaddr = 0;
	u32 phdr_count = 0;
	/* The image's PT_TLS, if it has one. Zeroed here rather than at the
	 * point of use so that "no PT_TLS" and "PT_TLS of size zero" cannot be
	 * confused: an image with no thread-local storage has no thread
	 * pointer, and mm->tls_ptr == 0 says exactly that. */
	u64 tls_offset = 0, tls_vaddr = 0, tls_filesz = 0, tls_memsz = 0;
	u64 tls_align = 1;
	bool tls_seen = false;
	int r;

	if (!mm || !image || !out)
		return -EINVAL;

	r = elf_validate(image, size, &phoff);
	if (r < 0)
		return r;

	eh = image;
	memset(out, 0, sizeof(*out));
	out->is_dyn = eh->e_type == ET_DYN;
	out->entry = load_bias + eh->e_entry;

	/*
	 * First pass: validate every program header. A PT_INTERP means the
	 * image wants a dynamic linker this kernel does not have, and running
	 * its entry point anyway would jump into the middle of an
	 * interpreter-less address space. Rejecting it explicitly is the only
	 * honest answer.
	 */
	for (u32 i = 0; i < eh->e_phnum; i++) {
		const struct elf64_phdr *ph =
			(const struct elf64_phdr *)(base + phoff +
						    (u64)i * eh->e_phentsize);

		if (ph->p_type == PT_INTERP)
			return -ENOEXEC;

		if (ph->p_type == PT_TLS) {
			/*
			 * One block per process. A second PT_TLS would mean two
			 * candidate thread pointers and no rule for choosing
			 * between them, and silently taking the first is how a
			 * program's own thread-local variables end up somewhere
			 * the linker never put them.
			 */
			if (tls_seen)
				return -ENOEXEC;
			if (!in_bounds(ph->p_offset, ph->p_filesz, size))
				return -ENOEXEC;
			if (ph->p_memsz < ph->p_filesz)
				return -ENOEXEC;
			if (load_bias + ph->p_vaddr < load_bias)
				return -ENOEXEC;
			/*
			 * The block, *and* the thread pointer, have to be in the
			 * user half. The thread pointer can sit up to p_align-1
			 * past the last byte of the block (see elf_load_tls), so
			 * it is the bound that matters, not p_vaddr + p_memsz.
			 * The kernel window is the bound available here;
			 * user_memory_write() enforces the stricter user half
			 * below when it copies the bytes, so a block that lands
			 * between the two is rejected by the write rather than
			 * mapped.
			 */
			if (load_bias + ph->p_vaddr + ph->p_memsz <
			    load_bias + ph->p_vaddr)
				return -ENOEXEC;
			if (load_bias + ph->p_vaddr + ph->p_memsz +
			    (ph->p_align ? ph->p_align : 1) - 1 >=
			    VMM_KERNEL_BASE)
				return -ENOEXEC;
			/*
			 * p_align of zero is not a power of two and ALIGN_UP
			 * would divide by it. The specification lets a linker
			 * emit 0 to mean "no alignment claimed"; treating that
			 * as byte alignment is the same answer the block's real
			 * alignment would give for a block of bytes and chars,
			 * and it is the only interpretation that cannot produce
			 * a thread pointer the image was not linked against.
			 */
			if (ph->p_align &&
			    (ph->p_align & (ph->p_align - 1)))
				return -ENOEXEC;
			tls_offset = ph->p_offset;
			tls_vaddr = load_bias + ph->p_vaddr;
			tls_filesz = ph->p_filesz;
			tls_memsz = ph->p_memsz;
			tls_align = ph->p_align ? ph->p_align : 1;
			tls_seen = true;
			continue;
		}

		if (ph->p_type != PT_LOAD)
			continue;
		if (!in_bounds(ph->p_offset, ph->p_filesz, size))
			return -ENOEXEC;
		if (ph->p_memsz < ph->p_filesz)
			return -ENOEXEC;
		if (ph->p_filesz && ph->p_offset + ph->p_vaddr < ph->p_offset)
			return -ENOEXEC;
		if (load_bias + ph->p_vaddr < load_bias)
			return -ENOEXEC;
		if (ph->p_align > PAGE_SIZE && (ph->p_align & (ph->p_align - 1)))
			return -ENOEXEC;
	}

	/* Second pass: map. */
	for (u32 i = 0; i < eh->e_phnum; i++) {
		const struct elf64_phdr *ph =
			(const struct elf64_phdr *)(base + phoff +
						    (u64)i * eh->e_phentsize);
		u64 seg_vaddr, seg_start, seg_end, copylen;
		uint32_t prot;

		if (ph->p_type == PT_PHDR) {
			phdr_vaddr = load_bias + ph->p_vaddr;
			phdr_count = 1;
			continue;
		}
		if (ph->p_type != PT_LOAD || ph->p_memsz == 0)
			continue;

		seg_vaddr = load_bias + ph->p_vaddr;
		seg_start = ALIGN_DOWN(seg_vaddr, PAGE_SIZE);
		seg_end = ALIGN_UP(seg_vaddr + ph->p_memsz, PAGE_SIZE);

		if (min_vaddr == ~0ULL || seg_start < min_vaddr)
			min_vaddr = seg_start;
		if (seg_end > max_vaddr)
			max_vaddr = seg_end;

		/*
		 * The file bytes go to p_vaddr, not to seg_start.
		 *
		 * p_vaddr and p_offset are congruent modulo p_align, so for a
		 * segment whose vaddr is page-aligned -- which is every segment
		 * except the one that carries .tdata -- the two are equal and
		 * this is a distinction without a difference. It stops being
		 * one when they are not: on build/init.elf the last LOAD is
		 * file offset 0x8ff0 at vaddr 0x409ff0, both 0xff0 into their
		 * page, and this function used to rebase the source to 0x8000
		 * and copy p_filesz bytes to 0x409000. That is short by 0xff0
		 * bytes and offset by 0xff0 bytes at the same time, so the
		 * segment's actual contents never arrived: .init_array and
		 * .data were never written at all, and the constructors libc
		 * runs before main silently did nothing.
		 *
		 * The leading (seg_vaddr - seg_start) bytes of the first page
		 * are left alone. They are not part of the segment, and the
		 * page they live in faults to a zero page if anything reads
		 * them, which is what Linux does with the same segment.
		 */
		copylen = ph->p_filesz;
		if (seg_end - seg_vaddr < copylen)
			copylen = seg_end - seg_vaddr;

		prot = phdr_prot(ph->p_flags);

		/*
		 * Segments are supposed to be page aligned with respect to
		 * each other. When they are not, the later segment's leading
		 * pages are already covered by the earlier one; the VMA is
		 * only created for the part that is not, but the contents are
		 * still written, because the later segment's bytes are the
		 * ones that should be visible there.
		 */
		if (seg_end > covered_end) {
			u64 vma_start = seg_start > covered_end ? seg_start
								 : covered_end;

			r = mm_add_vma(mm, (virt_addr_t)vma_start,
				       (virt_addr_t)seg_end, prot, VM_ANON);
			if (r < 0)
				return r;
			covered_end = seg_end;
		}

		if (copylen) {
			r = user_memory_write(mm, (virt_addr_t)seg_vaddr,
					      base + ph->p_offset, copylen, prot);
			if (r < 0)
				return r;
		}
	}

	if (max_vaddr == 0)
		return -ENOEXEC;
	if (out->entry < min_vaddr || out->entry >= max_vaddr)
		return -ENOEXEC;

	/*
	 * AT_PHDR is where a dynamic loader looks for the program headers, and
	 * the ELF ABI requires them to be inside a PT_LOAD. They are found by
	 * locating the segment that covers the header table's file offset
	 * rather than by assuming the first segment starts at file offset 0,
	 * because a linker is free to pad it.
	 */
	if (!phdr_vaddr) {
		for (u32 i = 0; i < eh->e_phnum; i++) {
			const struct elf64_phdr *ph =
				(const struct elf64_phdr *)(base + phoff +
							    (u64)i * eh->e_phentsize);

			if (ph->p_type != PT_LOAD)
				continue;
			if (phoff >= ph->p_offset &&
			    phoff < ph->p_offset + ph->p_filesz) {
				phdr_vaddr = load_bias + ph->p_vaddr +
					     (phoff - ph->p_offset);
				phdr_count = eh->e_phnum;
				break;
			}
		}
	}
	if (!phdr_vaddr)
		return -ENOEXEC;

	/*
	 * The TLS block goes in after every PT_LOAD, because the only thing
	 * this needs to know about them is how far they reach: the block
	 * usually starts inside the last segment's last page, and a VMA that
	 * overlapped it would be refused.
	 *
	 * max_vaddr moves with it, and past the *thread pointer* rather than
	 * past the last byte of the block. The heap starts at the end of the
	 * image, and for a static image the TLS block is the last thing in the
	 * address space; the thread pointer can sit up to p_align-1 bytes past
	 * the block's last byte (see elf_load_tls), and a brk() that started
	 * inside that gap would fault rather than fail the VMA insert cleanly.
	 */
	if (tls_memsz) {
		r = elf_load_tls(mm, base, tls_offset, tls_vaddr, tls_filesz,
				 tls_memsz, tls_align, covered_end);
		if (r < 0)
			return r;

		out->tls_ptr = mm->tls_ptr;
		out->tls_size = mm->tls_size;

		if (mm->tls_ptr > max_vaddr)
			max_vaddr = mm->tls_ptr;
	}

	out->phdr_vaddr = phdr_vaddr;
	out->phdr_count = phdr_count ? phdr_count : eh->e_phnum;
	out->min_vaddr = min_vaddr;
	out->max_vaddr = max_vaddr;

	/*
	 * The VMM keeps the code and data extents for /proc and for a
	 * security policy that treats them differently. They come from the
	 * executable bits of the segments, which is the same rule the
	 * program's own linker script used.
	 */
	for (u32 i = 0; i < eh->e_phnum; i++) {
		const struct elf64_phdr *ph =
			(const struct elf64_phdr *)(base + phoff +
						    (u64)i * eh->e_phentsize);
		u64 s, e;

		if (ph->p_type != PT_LOAD || ph->p_memsz == 0)
			continue;
		s = ALIGN_DOWN(load_bias + ph->p_vaddr, PAGE_SIZE);
		e = ALIGN_UP(load_bias + ph->p_vaddr + ph->p_memsz, PAGE_SIZE);
		if (ph->p_flags & PF_X) {
			if (mm->start_code == 0 || s < mm->start_code)
				mm->start_code = s;
			if (e > mm->end_code)
				mm->end_code = e;
		} else {
			if (mm->start_data == 0 || s < mm->start_data)
				mm->start_data = s;
			if (e > mm->end_data)
				mm->end_data = e;
		}
	}

	/*
	 * The heap starts where the last loadable segment ends. Starting it
	 * lower would let a brk() grow a VMA into the program's own data and
	 * the loader's VMA insert would fail with a confusing EINVAL; starting
	 * it higher wastes address space for no benefit.
	 *
	 * "Last loadable segment" includes the TLS block here, because max_vaddr
	 * was moved past it above and .tbss is the last thing in the address
	 * space for a static image.
	 */
	mm->start_brk = max_vaddr;
	mm->brk = max_vaddr;
	if (mm->mmap_base == 0) {
		mm->mmap_base = 0x0000200000000000ULL;
		mm->mmap_next = mm->mmap_base;
	}

	/* The TLS block is data, and for a static image it is the data with the
	 * highest address, so it belongs in the extents /proc and any policy
	 * that reads them will otherwise be told the program's memory ends
	 * before its thread-locals do. */
	if (mm->tls_ptr > mm->end_data)
		mm->end_data = mm->tls_ptr;

	ELF_LOG(KLOG_INFO, "loaded ELF entry %#lx phdr %#lx count %u [%#lx,%#lx)",
		out->entry, out->phdr_vaddr, out->phdr_count, min_vaddr, max_vaddr);
	return 0;
}
