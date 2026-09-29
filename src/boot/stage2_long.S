/*
 * stage2_long.S — enable PAE and long mode, then jump to the kernel.
 *
 * Runs in 32-bit protected mode. The sequence is fixed by the architecture:
 *
 *   1. CR4.PAE selects the 32-bit PML4/PDPT/PD/PT paging format.
 *   2. EFER.LME enables long mode, which only takes effect once paging is on.
 *   3. CR3 is loaded with the physical address of the PML4 built by stage2.c.
 *   4. CR0.PG | CR0.PE enables paging. From here the CPU is running in 64-bit
 *      compatibility mode, executing 32-bit instructions out of a 64-bit
 *      paging structure.
 *   5. A far jump with a 64-bit code descriptor (L=1) switches the CPU into
 *      long mode proper: 64-bit default operand size, and the segment base and
 *      limit stop mattering entirely.
 *
 * Ordering matters. LME before PG, and PG before the far jump: setting PG while
 * LME is clear would activate 32-bit paging against 64-bit page tables and
 * fault immediately.
 */
	.code32
	.section .text

/* MSR numbers. */
#define MSR_EFER        0xC0000080

/* EFER bits. */
#define EFER_LME        (1 << 8)
#define EFER_LMA        (1 << 10)
#define EFER_NXE        (1 << 11)

/* CR0 bits. */
#define CR0_PE          (1 << 0)
#define CR0_PG          (1 << 31)

/* CR4 bits. */
#define CR4_PAE         (1 << 5)

	.globl stage2_enter_long_mode
stage2_enter_long_mode:
	/* edi = pml4_phys, esi = entry, edx = bootinfo_phys */
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%ebx
	pushl	%esi
	pushl	%edi
	pushl	%edx

	/* Enable PAE. This must precede the LME write: the CPU interprets
	 * EFER.LME only in the presence of PAE, and the paging format changes
	 * from 2-level to 4-level at the same time. */
	movl	%cr4, %eax
	orl	$CR4_PAE, %eax
	movl	%eax, %cr4

	/* Set EFER.LME (and NXE, so the kernel's page tables may use the NX bit
	 * for user/kernel separation). */
	movl	$MSR_EFER, %ecx
	rdmsr
	orl	$EFER_LME, %eax
	orl	$(EFER_NXE << 16), %eax
	wrmsr

	/* Load the bootstrap PML4. A NULL physical address would load the legacy
	 * PCID-free CR3 form; ours is a real physical address. */
	movl	%edi, %eax
	movl	%eax, %cr3

	/* Turn on paging. */
	movl	%cr0, %eax
	orl	$(CR0_PG | CR0_PE), %eax
	movl	%eax, %cr0

	/* Reload the GDT so the far jump can pick up a 64-bit code descriptor. */
	lgdt	(gdtr64)

	/* Far jump into long mode. The 64-bit segment has L=1, which clears
	 * CS.L and switches the default operand size to 64 bits. */
	ljmp	$0x08, $long_mode_entry

	.code64
long_mode_entry:
	movw	$0x10, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %fs
	movw	%ax, %gs
	movw	%ax, %ss

	/* EFER.LMA is set by hardware once long mode is genuinely active. Reading
	 * it back is the only way to confirm the transition really happened. */
	movl	$MSR_EFER, %ecx
	rdmsr
	testl	$EFER_LMA, %eax
	jz	.LMA_missing

	movl	4(%ebp), %eax		/* entry */
	movl	8(%ebp), %edx		/* bootinfo_phys */
	movq	%rdx, %rdi		/* SysV: first argument in RDI */
	jmp	*%rax

.LMA_missing:
	/* Getting here means the CPU accepted LME but is not reporting long mode
	 * as active. There is nothing useful left to do; halt so the failure is
	 * visible on the serial console rather than executing garbage. */
	movw	$msg_lma, %si
	call	hang_puts
1:	cli
	hlt
	jmp	1b

	/* Reached only on a failed transition. */
hang_puts:
	movl	$0x3f8, %edx
	lodsb
	testb	%al, %al
	jz	2f
1:	inb	%dx, %al
	testb	$0x20, %al
	jnz	2f
	jmp	1b
2:	movl	$0x3f8, %edx
	movb	%al, %al
	outb	%al, %dx
	jmp	hang_puts

/* ---------------------------------------------------------------------------
 * GDTR for the long-mode switch.
 *
 * It is built at run time because stage2.c must fill in the physical address of
 * the GDT it constructs. Both halves live in .data rather than .rodata: LGDT
 * only reads them, but they are written during initialisation, and the .data
 * section is the one guaranteed to be writable.
 * --------------------------------------------------------------------------- */
	.section .data
	.globl stage2_gdt_base
stage2_gdt_base:
	.quad	0			/* physical address of the GDT */
	.globl stage2_gdt_limit
stage2_gdt_limit:
	.word	0x17			/* 3 descriptors - null, code64, data */
	.align	8
	.globl gdtr64
gdtr64:
	.word	stage2_gdt_limit
	/*
	 * Split into two 32-bit halves rather than one .quad. This file is
	 * assembled as a 32-bit object, and a 64-bit absolute relocation does not
	 * exist in that world — the assembler rejects it outright. The address
	 * being described is a 32-bit physical address anyway, so the upper half
	 * is zero by construction, not by omission.
	 */
	.long	stage2_gdt_base
	.long	0

	.section .rodata
msg_lma:
	.asciz	"\r\n[boot2] FATAL: long mode not active\r\n"
