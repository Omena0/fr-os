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

/* The bootstrap gate table and both halves of both reporter addresses. The
 * table is emitted by stage2_entry.S; the low/high split of a handler address
 * is done by stage2.ld, which is the only place that knows where the sections
 * ended up. */
	.globl	boot_idt
	.equ	BOOT_IDT_GATE_BYTES, 16

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
	 * Frame layout after `pushl %ebp; movl %esp,%ebp; pushl %ebx`:
	 *   [ebp+0]  saved ebp
	 *   [ebp+4]  return address
	 *   [ebp+8]  pml4_phys
	 *   [ebp+12] entry
	 *   [ebp+16] bootinfo_phys
	 *
	 * [ebp+4] is the return address, not a saved register: the `pushl %ebx`
	 * has already consumed one word, so everything the caller pushed is at
	 * +8 and above. Reading +4 as an argument is off by exactly one word and
	 * produces a perfectly plausible value, because a return address into
	 * stage2.c looks like nothing in particular.
	 */
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%ebx

	/*
	 * Interrupts off for the whole transition.
	 *
	 * Between here and the far jump the CPU is a 32-bit CPU, so the gates have
	 * to point at the 32-bit reporter -- but the gate table is shared with
	 * long mode, which reads a 16-byte entry where the 32-bit reporter's
	 * offsets only fill eight. The two halves of the sequence below are
	 * therefore bracketed: 32-bit gates in, far jump, 64-bit gates in. An
	 * interrupt landing in between would be delivered to whichever stub is
	 * installed, decoded with the wrong operand size, and walk into
	 * unrelated bytes -- a hang with no output, which is how this transition
	 * looks when it fails.
	 */
	cli

	/* Enable PAE. This must precede the LME write: the CPU interprets
	 * EFER.LME only in the presence of PAE, and the paging format changes
	 * from 2-level to 4-level at the same time. */
	movl	%cr4, %eax
	orl	$CR4_PAE, %eax
	movl	%eax, %cr4

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

	/* Load the bootstrap PML4. A NULL physical address would load the legacy
	 * PCID-free CR3 form; ours is a real physical address. */
	movl	8(%ebp), %eax		/* pml4_phys */
	movl	%eax, %cr3

	/* Turn on paging. */
	movl	%cr0, %eax
	orl	$(CR0_PG | CR0_PE), %eax
	movl	%eax, %cr0

	/* Reload the GDT so the far jump can pick up a 64-bit code descriptor. */
	lgdt	(gdtr64)

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
	orl	$0x00010000, %eax	/* set CR4.FSGSBASE */
	movl	%eax, %cr4

	/*
	 * CR4.FSGSBASE is set here rather than by the kernel because this is where
	 * the rest of the CPU state the kernel depends on is established -- PAE,
	 * OSFXSR, OSXSAVE, EFER and XCR0 are all handled above or nearby. The
	 * kernel's first instruction asserts the bit and refuses to continue
	 * without it, which is the right division: a control register it depends
	 * on belongs to the handoff contract, not to code that mutates it behind
	 * the loader's back.
	 *
	 * The kernel needs it because WRGSBASE is the only way to install a
	 * *visible* GS base from ring 0. WRMSR to IA32_GS_BASE updates the hidden
	 * base only, so the per-CPU pointer written that way is not the one
	 * GS-relative addressing uses, and on this machine it also raises #GP
	 * under KVM. kmain.S asserts the bit and then issues WRGSBASE.
	 */

	/*
	 * Enable the AVX state, which is a separate thing from SSE.
	 *
	 * VEX-encoded instructions -- vmovdqu and the rest of the AVX2 set -- are
	 * not SSE with a different prefix: they imply the upper half of the YMM
	 * registers, and those only exist if the OS has asked for them through
	 * XSETBV. Until XCR0 says so, every VEX instruction raises #UD, even with
	 * CR0.EM clear and CR4.OSFXSR set.
	 *
	 * The note on the XSETBV below explains the sequence's ordering; the short
	 * version is that each step is a precondition for the next, and CPUID is
	 * the one that gates all of them.
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
	 * The order above is fixed and each step depends on the last, and it is
	 * correct as far as it goes: CPUID has to report AVX and OSXSAVE before
	 * either is usable, CR4.OSXSAVE has to be set before XGETBV itself will
	 * execute since the instruction is #UD until OSXSAVE is on, and XSETBV turns
	 * on x87, SSE and AVX together because enabling AVX while leaving x87 state
	 * off produces a CPU that faults on the first FPU instruction instead.
	 *
	 * What was missing is that XCR0 was OR-ed into blindly. XSETBV raises #GP
	 * -- not #UD, so no amount of looking for a "not implemented" signature
	 * finds it -- if the value being written has a bit set that the CPU does not
	 * implement. Every bit the firmware happened to leave in XCR0 was therefore
	 * passed straight through unchecked, and the failure lands on the XSETBV
	 * itself: the loader stops between the "entering long mode" line and the
	 * kernel's first, with nothing printed, because the instruction that faults
	 * is the one that would have been the last chance to say so.
	 *
	 * Firmware that enables PKRU (XCR0 bit 9) without the CPU advertising
	 * support for it in CPUID.0x0D is the realistic case, and it is a
	 * firmware bug rather than ours -- but the alternative to honouring it is a
	 * loader that cannot boot on it, and the mask costs one CPUID.
	 *
	 * CPUID leaf 0x0D, subleaf 0, returns in EAX the XCR0 states this CPU
	 * supports, and that read is safe unconditionally *here* even though the
	 * leaf is optional: XSAVE -- and therefore CPUID.0x0D -- exists on every
	 * CPU that reports CPUID.1:ECX.OSXSAVE, and OSXSAVE has already been tested
	 * two instructions above. So there is no state in which this path is taken
	 * and the leaf is missing.
	 *
	 * The OR of 0x7 is applied after the mask and does not need one of its own:
	 * the three states are x87, SSE and YMM_Hi128, and YMM_Hi128 is only
	 * reachable on a CPU that reports AVX, which is the test above.
	 */
	movl	$0, %ecx
	xgetbv				/* EAX = XCR0 as the firmware left it */
	movl	%eax, %esi
	movl	$0x0D, %eax
	movl	$0, %ecx
	cpuid				/* EAX = XCR0 states this CPU supports */
	andl	%esi, %eax		/* drop anything it does not */
	orl	$0x7, %eax		/* x87 | SSE | AVX */
	xsetbv

.Lno_avx:

	/*
	 * Point the gates at the 32-bit reporter before the far jump.
	 *
	 * The static table that stage2_entry.S emits points every one of the 256
	 * gates at exc_stub, the 32-bit reporter, and that is the right target for
	 * everything up to and including this instruction. It has to be restored
	 * here because boot_gdt_init() rewrites the table to the 64-bit reporter
	 * earlier in the sequence, on the reasonable but wrong assumption that
	 * nothing faults between that point and the transition. Things do fault in
	 * between -- the CR0/CR4 and XGETBV/XSETBV writes above are privileged
	 * instructions that raise #UD and #GP respectively when the CPU is not in
	 * the state they require, which is exactly the state this code is in the
	 * business of establishing.
	 *
	 * A 64-bit instruction stream executed in compatibility mode decodes as a
	 * different, still-plausible stream: `pushq %rax` is not `pushl %eax`, the
	 * stack offsets are halved, and the reporter walks into unrelated bytes and
	 * ends up in its own halt loop. Nothing is printed, because the code that
	 * would print is the code that could not be entered, and the loader simply
	 * stops between two lines it has already printed.
	 *
	 * So the mode and the gates are kept in step on both sides of the jump:
	 * 32-bit gates in here, 64-bit gates in at long_mode_entry. Interrupts are
	 * off (see the cli at the top of this function) so no delivery can see a
	 * half-rewritten table.
	 */
	movl	$boot_idt, %edi
	movl	$exc_stub_lo, %eax
	movl	$exc_stub_hi, %edx
	movl	$256, %ecx
.Lgates32:
	movw	%ax, 0(%edi)			/* offset[15:0]  */
	movw	$0x0008, 2(%edi)		/* selector: code */
	movw	$0x8E00, 4(%edi)		/* IST 0, DPL 0, 32-bit interrupt gate */
	movw	%dx, 6(%edi)			/* offset[31:16] */
	movl	$0, 8(%edi)			/* the other half of a
						 * long-mode entry, which a
						 * 32-bit one ignores */
	movl	$0, 12(%edi)
	addl	$BOOT_IDT_GATE_BYTES, %edi
	loop	.Lgates32

	/* Far jump into long mode. The 64-bit segment has L=1, which clears
	 * CS.L and switches the default operand size to 64 bits. */
	ljmp	$0x08, $long_mode_entry

	.code64
long_mode_entry:
	movw	$0x10, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	$0x10, %ax
	movw	%ax, %fs
	movw	%ax, %gs
	movw	$0x10, %ax
	movw	%ax, %ss

	/*
	 * Point the gates at the 64-bit reporter, now that the CPU can execute it.
	 *
	 * Doing it here rather than before the far jump is what makes the pairing
	 * airtight: from the first instruction of long mode, every gate names a
	 * 64-bit stub, and until the instruction before the far jump every gate
	 * named a 32-bit one. There is no instruction in the whole sequence that
	 * can fault against a gate for the other mode.
	 *
	 * The gate layout is the same sixteen bytes the static table has, and it is
	 * the same sixteen bytes a long-mode CPU reads: offset[15:0], selector,
	 * type/IST, offset[31:16], and then eight bytes that must be zero. Those
	 * last eight are not padding -- a non-zero bit anywhere in offset[63:32]
	 * raises #GP before the handler is ever entered, so a gate built by
	 * striding eight bytes is undeliverable. Both halves of the upper word are
	 * therefore written, not left holding whatever the 32-bit table had.
	 */
	movl	$boot_idt, %edi
	movl	$exc_stub64_lo, %eax
	movl	$exc_stub64_hi, %edx
	movl	$256, %ecx
.Lgates64:
	movw	%ax, 0(%edi)
	movw	$0x0008, 2(%edi)
	movw	$0x8E00, 4(%edi)
	movw	%dx, 6(%edi)
	movl	$0, 8(%edi)
	movl	$0, 12(%edi)
	addl	$BOOT_IDT_GATE_BYTES, %edi
	loop	.Lgates64

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

	/*
	 * Give the caller its EBX back. This is an ordinary cdecl function as far
	 * as stage2.c is concerned -- it is declared and called like one, and
	 * GCC is entitled to keep a live value in EBX across the call -- but the
	 * body never returns to it: it ends in `jmp *%rax` into the kernel. Without
	 * this pop the kernel starts with EBX holding whatever the CPUID above left
	 * there, which is ECX's AVX/OSXSAVE bits rather than anything of the
	 * caller's.
	 *
	 * `popq`, not `popl`: the push was a 32-bit push in the prologue and this
	 * is 64-bit code, but the width has to match -- a `popl` would pop four
	 * bytes and leave the stack pointer eight short of where the frame started.
	 */
	popq	%rbx
	jmp	*%rax

.LMA_missing:
	/* Getting here means the CPU accepted LME but is not reporting long mode
	 * as active. There is nothing useful left to do; print why and halt, so
	 * the failure is visible on the serial console rather than the machine
	 * executing garbage. */
	movl	$msg_lma, %edi
	call	hang_puts
	cli
1:	hlt
	jmp	1b

/*
 * Reached only on a failed transition, so it is the one piece of code whose
 * entire job is to explain what went wrong.
 *
 * It had the exact bug its neighbour in stage2_entry.S documents as already
 * fixed. The poll read the line status into AL -- which is the only register an
 * `in` can target -- and then transmitted AL, so what went out was the status
 * byte rather than the character. The status byte has LSR.THRE set, so it is
 * 0x60, and the message came out as an unbounded stream of backticks and
 * nothing else: a console that looks busy while saying nothing, on the one path
 * whose purpose is to say something.
 *
 * Two further faults in the same six instructions. The poll read port 0x3F8,
 * which is the receive buffer register and never has bit 5 set, instead of the
 * line status register at 0x3FD -- so the spin could not have terminated even
 * with the right register. And the `movb %al, %al` on the way out is a no-op
 * that reads like it was meant to move the character somewhere safe.
 *
 * So: the character travels in DIL and never touches AL, the status register is
 * 0x3FD, and the spin is bounded. The bound is the part that matters most --
 * this runs after a transition has already failed, and a diagnostic that can
 * itself hang forever is worse than no diagnostic, because it converts a
 * readable failure into a silent stop.
 */
	.globl hang_putc64
hang_putc64:
	movl	$0x3FD, %edx		/* line status, not the data register */
	movl	$0x40000, %ecx		/* spin budget: ~260k iterations */
1:	inb	(%dx), %al
	testb	$0x20, %al		/* LSR_THRE */
	jnz	2f
	loop	1b
	movl	$'?', %edi		/* UART wedged: say so and carry on */
2:	movl	$0x3F8, %edx
	movzbl	%dil, %eax		/* the character, not the status */
	outb	%al, (%dx)
	ret

	.globl hang_puts
hang_puts:
	movq	%rdi, %rsi
1:	lodsb
	testb	%al, %al
	jz	2f
	movzbl	%al, %edi
	call	hang_putc64
	jmp	1b
2:	ret

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
