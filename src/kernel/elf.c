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

/* ---------------------------------------------------------------- load ------ */

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
		u64 seg_vaddr, seg_start, seg_end, file_off, copylen;
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
		 * A page-aligned vaddr with an unaligned file offset is legal:
		 * the first (vaddr - seg_start) bytes of the first page are not
		 * part of the segment, so the file cursor starts earlier and
		 * the same number of bytes are copied to the page-aligned
		 * destination.
		 */
		file_off = ph->p_offset - (seg_vaddr - seg_start);
		copylen = ph->p_filesz;
		if (seg_end - seg_start < copylen)
			copylen = seg_end - seg_start;

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
			r = user_memory_write(mm, (virt_addr_t)seg_start,
					      base + file_off, copylen, prot);
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
	 */
	mm->start_brk = max_vaddr;
	mm->brk = max_vaddr;
	if (mm->mmap_base == 0) {
		mm->mmap_base = 0x0000200000000000ULL;
		mm->mmap_next = mm->mmap_base;
	}

	ELF_LOG(KLOG_INFO, "loaded ELF entry %#lx phdr %#lx count %u [%#lx,%#lx)",
		out->entry, out->phdr_vaddr, out->phdr_count, min_vaddr, max_vaddr);
	return 0;
}
