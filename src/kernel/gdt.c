/*
 * gdt.c — the kernel's GDT and Task State Segment.
 *
 * The layout is normative and documented in
 * docs/src/kernel/privilege-levels.md. Two of its entries look wrong and are
 * not: index 3 is a 32-bit ring-3 descriptor that is never executed, because
 * SYSRET computes CS and SS arithmetically from STAR and the arithmetic is what
 * makes index 5 the descriptor user code actually runs on.
 *
 * The selector constants live in src/include/gdt.h so the bootloader, the
 * kernel, and syscall.c's STAR write cannot drift apart. Only the storage and
 * the three CPU-touching operations are here.
 */
#include <gdt.h>

#include <interrupt.h>
#include <klog.h>
#include <kstring.h>
#include <panic.h>
#include <types.h>

KLOG_SUBSYSTEM("gdt");

/* ------------------------------------------------- descriptor storage ----- */

/* The pseudo-descriptor handed to LGDT. Ten bytes; the limit is 16 bits and the
 * base is a full doubleword, which is why this cannot be a packed struct of
 * two natural-width fields without an explicit byte count. */
struct gdtr {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed));

/*
 * The 64-bit TSS. Architectural offsets, 80 bytes.
 *
 * The layout is fixed by the processor, not by this file — the table in
 * docs/src/kernel/privilege-levels.md is the same one. interrupt_entry.S and
 * idt.c both depend on these offsets, so every field is asserted rather than
 * the total alone: a struct that is the right size with the fields in the
 * wrong places is exactly the failure these checks exist to catch.
 */
struct tss {
	uint32_t reserved0;   /* 0x00 */
	uint64_t rsp0;        /* 0x04 — 64 bits in long mode, not 16 */
	uint64_t rsp1;        /* 0x0C */
	uint64_t rsp2;        /* 0x14 */
	uint64_t reserved1;   /* 0x1C */
	uint32_t ist[7];      /* 0x24 — 32-bit each, fixed by the architecture */
	uint64_t reserved2;   /* 0x40 */
	uint64_t iomap_base;  /* 0x48 */
} __attribute__((packed));

_Static_assert(offsetof(struct tss, rsp0) == 4,
	       "the interrupt stubs and the IDT read RSP0 at TSS+4");
_Static_assert(offsetof(struct tss, rsp1) == 0x0C, "bad rsp1 offset");
_Static_assert(offsetof(struct tss, rsp2) == 0x14, "bad rsp2 offset");
_Static_assert(offsetof(struct tss, ist) == 0x24, "bad IST offset");
_Static_assert(offsetof(struct tss, iomap_base) == 0x48, "bad iomap offset");
_Static_assert(sizeof(struct tss) == 80,
	       "the 64-bit TSS is 80 bytes without an I/O permission bitmap");

/*
 * Per the docs, each CPU has its own GDT and TSS. The boot CPU is the only one
 * that exists today, so there is exactly one of each. They are not static
 * because interrupt_entry.S and idt.c refer to the TSS by name.
 */
static uint64_t gdt[GDT_ENTRIES] __attribute__((aligned(16)));
struct tss kernel_tss __attribute__((aligned(16)));

/* ------------------------------------------------- assembly entry points -- */

/*
 * Both of these are far jumps. LGDT and LTR change the CPU's idea of what the
 * current code segment is, and only a far return reliably reloads CS — a near
 * jump leaves it pointing at a descriptor the CPU may have just invalidated.
 * interrupt_entry.S carries both, along with the segment reloads that have to
 * accompany a CS change.
 */
void gdt_flush(uint64_t gdt_pointer, uint16_t code_selector,
	       uint16_t data_selector);
void tss_flush(uint16_t tss_selector);

/* ------------------------------------------------- private helpers --------- */

static void gdt_set(unsigned index, uint64_t entry)
{
	ASSERT_MSG(index < GDT_ENTRIES, "gdt: descriptor index %u out of range",
		  index);
	gdt[index] = entry;
}

/* ------------------------------------------------- IST stacks ------------- */

/*
 * IST stacks are not allocated here, and this is a real gap rather than an
 * oversight.
 *
 * The IST field is 32 bits. The kernel is linked at 0xFFFFFFFF80000000 and its
 * direct map is at 0xFFFF800000000000, so a page allocated by vmalloc() is not
 * representable in the field — truncating it would point the CPU at a low
 * address that is not mapped, and a double fault that lands there would fault
 * again with nowhere left to go.
 *
 * Getting it right needs a page mapped at a fixed low linear address, which
 * means reaching into the kernel page tables, and the VMM does not expose the
 * kernel PGD yet. Until it does, the IST pointers stay zero, which the CPU reads
 * as "no IST", and idt.c leaves the IST field of those gates at zero to match.
 * A gate claiming an IST index with a null pointer behind it would #PF on
 * entry, which is the one outcome worse than not having an IST at all.
 */
void gdt_set_ist_stack(uint8_t ist, void *stack_top)
{
	ASSERT_MSG(ist >= 1 && ist <= 7, "gdt: IST index %u out of range", ist);
	ASSERT_MSG(((uintptr_t)stack_top & 0xFFFFFFFF00000000ULL) == 0,
		   "gdt: IST stack %p does not fit the 32-bit IST field; it must "
		   "be mapped at a low linear address",
		   stack_top);

	kernel_tss.ist[ist - 1] = (uint32_t)(uintptr_t)stack_top;
}

bool gdt_have_ist(uint8_t ist)
{
	return ist >= 1 && ist <= 7 && kernel_tss.ist[ist - 1] != 0;
}

/* ------------------------------------------------- public interface ------- */

void gdt_reload(uint32_t cpu)
{
	/*
	 * Per-CPU, per the docs. With a single GDT and a single TSS there is
	 * nothing to vary yet; the parameter is part of the interface so that
	 * secondary-CPU startup is a local operation rather than a redesign.
	 */
	(void)cpu;

	gdt_set(GDT_INDEX_NULL,    0);
	gdt_set(GDT_INDEX_KCODE,   GDT_LCODE64);
	gdt_set(GDT_INDEX_KDATA,   GDT_LDATA64);
	/* The STAR base. Never executed; see the file comment. */
	gdt_set(GDT_INDEX_UCODE32, GDT_LUSERCODE32);
	gdt_set(GDT_INDEX_UDATA,   GDT_LUSERDATA);
	gdt_set(GDT_INDEX_UCODE64, GDT_LUSER64);

	/*
	 * Zeroed here rather than at compile time, because the TSS descriptor
	 * needs the TSS's own address, which is not a constant this file can
	 * compute. This also resets any IST pointer that gdt_set_ist_stack()
	 * installed, since the CPU is holding a reference to this struct.
	 */
	memset(&kernel_tss, 0, sizeof(kernel_tss));
	/* No I/O permission bitmap follows the structure, so every port access
	 * from ring 3 is denied. There is no bitmap to make exceptions in. */
	kernel_tss.iomap_base = sizeof(kernel_tss);

	gdt_set(GDT_INDEX_TSS,
		GDT_TSS_DESC64_LOW((uint64_t)&kernel_tss, sizeof(kernel_tss) - 1));
	gdt_set(GDT_INDEX_TSS + 1,
		GDT_TSS_DESC64_HIGH((uint64_t)&kernel_tss));

	struct gdtr gdtr = {
		.limit = sizeof(gdt) - 1,
		.base  = (uint64_t)gdt,
	};

	/* CS first, then the TSS. A ring-3 interrupt before the LTR would load
	 * RSP0 from a TSS the CPU is not using yet and run on a null stack. */
	gdt_flush((uint64_t)&gdtr, KERNEL_CODE_SELECTOR, KERNEL_DATA_SELECTOR);
	tss_flush(TSS_SELECTOR);

	klog(KLOG_INFO, "gdt: cpu %u, %u entries, kcode=%#x kdata=%#x "
	     "ucode32=%#x udata=%#x ucode64=%#x tss=%#x\n",
	     cpu, GDT_ENTRIES, KERNEL_CODE_SELECTOR, KERNEL_DATA_SELECTOR,
	     USER_CODE_SELECTOR, USER_DATA_SELECTOR, USER_CODE64_SELECTOR,
	     TSS_SELECTOR);

	if (!gdt_have_ist(IST_DOUBLE_FAULT) || !gdt_have_ist(IST_NMI))
		klog(KLOG_WARN,
		     "gdt: IST1/IST2 have no stack; double fault and NMI will run "
		     "on the interrupted stack until a low fixed mapping exists");
}

void tss_set_kernel_stack(void *stack_top)
{
	/*
	 * The CPU reads RSP0 as a full 64 bits. Truncating it to 16, which the
	 * protected-mode layout would suggest, would produce a stack address in
	 * the low 64 KiB and fault on the first ring-3 interrupt.
	 */
	kernel_tss.rsp0 = (uint64_t)stack_top;
}
