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

/* Poll LSR.THRE first. A byte written while the 16-entry FIFO still holds
 * an unsent one is dropped silently, and a burst of unpokked writes loses
 * exactly the characters that mark how far the CPU got. */
#define TRACE(c) \
	movl	$0x3FD, %edx;			\
1:	inb	(%dx), %al;			\
	testb	$0x20, %al;			\
	jz	1b;				\
	movl	$0x3F8, %edx;			\
	movb	$(c), %al;			\
	outb	%al, (%dx)


	.globl stage2_enter_long_mode
stage2_enter_long_mode:
	/*
	 * The arguments arrive on the stack, not in registers.
	 *
	 * stage2.c is compiled with -m32, so this call uses the i386 SysV ABI,
	 * where every argument is pushed and none arrives in a register. Reading
	 * pml4_phys out of %edi looks like it works: %edi is callee-saved, so it
	 * still holds whatever stage2_main last stored there, and a plausible
	 * address appears to be in hand.
	 *
	 * The address is never checked, so paging comes up on a CR3 that was
	 * never the PML4 -- one whose low bits are set, which is not a legal
	 * legacy CR3 at all. Enabling CR0.PG with it faults on the very next
	 * instruction fetch, and the loader stops a few characters after
	 * announcing that it is entering long mode.
	 *
	 * Frame layout after `pushl %ebp; movl %esp,%ebp`:
	 *   [ebp+0]  saved ebp
	 *   [ebp+4]  saved ebx
	 *   [ebp+8]  pml4_phys
	 *   [ebp+12] entry
	 *   [ebp+16] bootinfo_phys
	 */
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%ebx

	/* Enable PAE. This must precede the LME write: the CPU interprets
	 * EFER.LME only in the presence of PAE, and the paging format changes
	 * from 2-level to 4-level at the same time. */
	TRACE('a')
	movl	%cr4, %eax
	orl	$CR4_PAE, %eax
	movl	%eax, %cr4
	TRACE('b')

	/*
	 * Set EFER.LME, and NXE so the kernel's page tables may use the NX bit to
	 * separate user and kernel pages.
	 *
	 * Both are set in EAX, because both are low bits of the MSR. `rdmsr`
	 * leaves the low half of a 64-bit value in EAX and the high half in EDX,
	 * and EFER's bits live at 8, 10 and 11 -- all inside the low half.
	 *
	 * Spreading these across the two halves is the easy mistake here, and it
	 * is silent in the worst way. Setting EFER.LME in EDX sets EFER bit 40,
	 * which is a reserved bit with no name; the read-back below still shows
	 * LMA clear, so the failure is visible, but only if something reads it.
	 * The loader does not. It prints "entering long mode", enables paging,
	 * and the far jump to a descriptor with L=1 does not enter long mode.
	 * The CPU instead takes the descriptor's base and limit, keeps executing
	 * 32-bit code out of the 64-bit page tables, and eventually faults with
	 * nothing left to report it.
	 */
	movl	$MSR_EFER, %ecx
	rdmsr
	orl	$EFER_LME, %eax
	orl	$EFER_NXE, %eax
	wrmsr
	TRACE('c')

	/* Load the bootstrap PML4. A NULL physical address would load the legacy
	 * PCID-free CR3 form; ours is a real physical address. */
	movl	8(%ebp), %eax		/* pml4_phys */
	movl	%eax, %cr3
	TRACE('d')

	/* Turn on paging. */
	movl	%cr0, %eax
	orl	$(CR0_PG | CR0_PE), %eax
	TRACE('e')
	movl	%eax, %cr0
	TRACE('f')

	/* Reload the GDT so the far jump can pick up a 64-bit code descriptor. */
	lgdt	(gdtr64)
	TRACE('g')

	/*
	 * Enable SSE before handing the CPU to the kernel.
	 *
	 * x86-64 has SSE2 on by default, so the kernel is compiled to use it: the
	 * first thing its memmove lowers to is a pair of `vmovdqu`. This CPU has
	 * CR0.EM set, because real mode sets it, and CR4.OSFXSR clear, because
	 * the legacy FPU enable sequence was never run. In that state every SSE
	 * instruction raises #UD.
	 *
	 * The fault is at the first vector instruction, which is to say very
	 * early: the kernel sets up its stack, writes its GS base, checks EFER,
	 * and calls kmain, whose first copy faults. Nothing has been printed yet,
	 * so the console shows the loader's "entering long mode" line and then
	 * nothing -- the same symptom as every other way this handoff can fail.
	 *
	 * EM has to be clear for SSE to execute at all, and MP has to be set so
	 * that a pending x87 instruction reports #DE instead of deferring it.
	 */
	movl	%cr0, %eax
	andl	$0xFFFFFFFB, %eax	/* clear CR0.EM */
	orl	$0x00000002, %eax	/* set CR0.MP */
	movl	%eax, %cr0

	movl	%cr4, %eax
	orl	$0x00000200, %eax	/* set CR4.OSFXSR */
	movl	%eax, %cr4

	/*
	 * Enable the AVX state, which is a separate thing from SSE and is what
	 * the kernel's code actually uses.
	 *
	 * The kernel is compiled -mavx2 -mxsave, so GCC emits VEX-encoded
	 * instructions -- vmovdqu for a 16-byte move, and so on. VEX is not SSE
	 * with a different prefix: it implies the upper half of the YMM
	 * registers, and those only exist if the OS has asked for them through
	 * XSETBV. Until XCR0 says so, every VEX instruction raises #UD, even
	 * with CR0.EM clear and CR4.OSFXSR set.
	 *
	 * The order is fixed and each step depends on the last. CPUID has to
	 * report AVX and OSXSAVE before either is usable. CR4.OSXSAVE has to be
	 * set before XGETBV itself will execute, since the instruction is #UD
	 * until OSXSAVE is on. XCR0 must already have the XMM and YMM_Hi128 bits
	 * set -- the BIOS may have set them, and if it did not then this CPU
	 * cannot be asked to enable YMM and the kernel cannot run here at all.
	 * Only then does XSETBV turn on the x87, SSE and AVX state together,
	 * because enabling AVX while leaving x87 state off produces a CPU that
	 * faults on the first FPU instruction instead.
	 */
	movl	$1, %eax
	movl	$0, %ecx
	cpuid
	movl	%ecx, %ebx
	andl	$(1 << 27), %ebx		/* AVX */
	jz	.Lno_avx
	testl	$(1 << 28), %ecx		/* OSXSAVE */
	jz	.Lno_avx

	movl	%cr4, %eax
	orl	$(1 << 18), %eax		/* CR4.OSXSAVE */
	movl	%eax, %cr4

	/*
	 * Read the current XCR0 and add the three states we need, rather than
	 * insisting they are already there. Firmware sets XCR0 to 3 -- x87 and
	 * SSE only -- and leaves the YMM_Hi128 bit clear, so a loader that
	 * checks for the AVX state before setting it concludes the CPU cannot do
	 * AVX and skips the whole sequence. The check is the wrong way round:
	 * enabling that state is the loader's job, not the firmware's. Reading
	 * XCR0 first and OR-ing into it keeps any state the firmware did set,
	 * such as the LWP bit, instead of overwriting it.
	 */
	movl	$0, %ecx
	xgetbv
	orl	$0x7, %eax			/* x87 | SSE | AVX */
	xsetbv

.Lno_avx:

	/*
	 * Switch the gates to the 64-bit reporter and go, with nothing in
	 * between.
	 *
	 * The table can only hold gates for one mode at a time. Pointed at the
	 * 64-bit reporter, a fault taken while the CPU is still a 32-bit CPU
	 * executes a 64-bit instruction stream in compatibility mode, where the
	 * same bytes decode as a different -- still plausible, still silent --
	 * instruction stream. The timer interrupt is enough to trigger this: it
	 * fires at any point after the firmware round trips, and if it lands in
	 * that window the machine walks into the reporter's own halt loop having
	 * printed nothing at all.
	 *
	 * So interrupts are cleared first, the gates are rewritten second, and the
	 * far jump is third. Nothing can be delivered into the mixed state, and
	 * there is no window in which the table disagrees with the mode.
	 *
	 * Doing this here rather than from C is the point: boot_gdt_init() runs
	 * before the page tables exist and long before the jump, so anything it
	 * did to the table left every instruction in between exposed.
	 */
	cli

	movl	$boot_idt, %edi
	movl	$exc_stub_lo, %eax
	movl	$exc_stub_hi, %edx
	movl	$(256 * 16), %ecx
2:	movb	%al, 0(%edi)
	movb	%ah, 1(%edi)
	movb	$0x08, 2(%edi)		/* selector: 64-bit code segment */
	movb	$0x00, 3(%edi)
	movb	$0x00, 4(%edi)		/* IST 0 */
	movb	$0x8E, 5(%edi)		/* present, DPL 0, 64-bit interrupt gate */
	movb	%dl, 6(%edi)
	movb	%dh, 7(%edi)
	movl	$0, 0x8(%edi)		/* offset[63:32] must be zero in long mode */
	movl	$0, 0xC(%edi)
	addl	$16, %edi
	loop	2b

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

	/*
	/*
	 * %rbp, not %ebp.
	 *
	 * In 64-bit code a memory operand written as `n(%ebp)` is assembled with a
	 * 32-bit address-size prefix, and the CPU honours that prefix by using the
	 * low half of the *matching* register -- RDI rather than RBP. GAS does not
	 * warn; the disassembly reads `mov 0x10(%rdi),%edx` where the source says
	 * `movl 12(%ebp), %edx`.
	 *
	 * So both loads come out of whatever RDI happens to point at instead of
	 * the frame, the entry point is never loaded, and the final `jmp *%rax`
	 * uses whatever EAX happened to hold -- here the EFER value read a few
	 * instructions earlier. The machine lands in the middle of the loader's
	 * low memory, decodes it as 64-bit instructions, and takes #UD. By then
	 * long mode is genuinely running, which is why the console shows the
	 * loader's own "entering long mode" line and then nothing.
	 */
	/*
	 * The entry is a kernel-half address, so it has to be rebuilt as a
	 * 64-bit value rather than used as a zero-extended 32-bit one.
	 *
	 * KERNEL_VIRT_BASE is 0xFFFFFFFF80000000, so the entry's low half is
	 * 0x80000180 and a plain `movl` leaves RAX = 0x0000000080000180. Jumping
	 * to that lands 2 GiB into the identity map, which is present, writable
	 * RAM holding nothing -- so nothing faults and there is no diagnostic to
	 * read. The CPU walks through whatever it decodes there until it reaches
	 * an address it cannot execute, which is why the failure surfaces as the
	 * machine wandering off to look like a random physical address rather than
	 * as a jump to the wrong place.
	 *
	 * Sign extension restores the high half exactly, because the kernel is
	 * linked in the top of the address space and bit 31 of the low half is
	 * always set for it.
	 */
	/*
	 * The entry is a kernel-half address, so it has to be rebuilt as a
	 * 64-bit value rather than used as a zero-extended 32-bit one.
	 *
	 * KERNEL_VIRT_BASE is 0xFFFFFFFF80000000, so the entry's low half is
	 * 0x80000180 and a plain `movl` leaves RAX = 0x0000000080000180. Jumping
	 * to that lands 2 GiB into the identity map, which is present, writable
	 * RAM holding nothing -- so nothing faults and there is no diagnostic to
	 * read. The CPU walks through whatever it decodes there until it reaches
	 * an address it cannot execute, which is why the failure surfaces as the
	 * machine wandering off to look like a random physical address rather than
	 * as a jump to the wrong place.
	 *
	 * Sign extension restores the high half exactly, because the kernel is
	 * linked in the top of the address space and bit 31 of the low half is
	 * always set for it.
	 */
	movl	12(%rbp), %eax		/* entry, low half */
	movslq	%eax, %rax		/* -> 0xffffffff80000180 */
	movl	16(%rbp), %edx		/* bootinfo_phys */
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

	.section .data
	.align	4
/*
 * The six bytes LGDT reads: a 16-bit limit, then a 32-bit base.
 *
 * These must be the *contents* of the two values, not their addresses.
 * `.word stage2_gdt_limit` looks like it does that and does not:
 * stage2_gdt_limit is a label, so the assembler emits a relocation and the
 * linker fills in the label's own address. LGDT then loads a limit that is an
 * address and a base that points at the two variables in .data -- whose
 * contents are 0 and 0x17. The far jump to selector 0x08 reads a null
 * descriptor out of there and takes #GP(0x08) at the first instruction of
 * long mode, with the loader's own "entering long mode" line as the last
 * thing on the console and a GDT limit that matches no array in the program.
 *
 * boot_gdt_init() writes all six bytes once the GDT exists.
 */
	.globl	gdtr64
gdtr64:
	.word	0			/* limit */
	.long	0			/* base */
	.long	0			/* trailing padding */


	.section .rodata
msg_lma:
	.asciz	"\r\n[boot2] FATAL: long mode not active\r\n"
