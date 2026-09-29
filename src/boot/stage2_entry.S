/*
 * stage2_entry.S — real mode to 32-bit protected mode transition.
 *
 * stage1 far-jumps here at 0x8000 in 16-bit real mode with interrupts
 * disabled and the boot drive number pushed on the stack. This stub loads a
 * GDT, sets CR0.PE, far-jumps into a 32-bit code segment, clears the BSS, and
 * calls the C loader.
 *
 * The transition is a far jump rather than a far call on purpose: ljmp loads
 * CS from the GDTR and abandons the 16-bit code path entirely, so no return
 * address is left pointing into a segment that no longer exists. Nothing in
 * stage2 or the kernel ever returns to real mode.
 */
	.code16
	.section .text
#include "boot_layout.h"

	.globl stage2_entry
stage2_entry:
	cli
	cld

	/* The 16-bit C calling convention pushes arguments right to left, so the
	 * single argument sits at the top of the stack on entry.
	 *
	 * The boot drive is kept on the stack rather than written to its global
	 * here. In .code16 there is no 32-bit absolute addressing, so
	 * `movb %al, stage1_boot_drive` would assemble as a 16-bit absolute
	 * write, which the linker can only satisfy if the symbol happens to sit
	 * below 64 KiB — true today and a silent memory corruption the day it is
	 * not. Round-tripping through the stack costs two instructions and has no
	 * such precondition; the store happens in 32-bit mode below. */
	popw	%ax			/* boot drive from stage1 */

	/* Set up a stack in low memory that stage1's image does not overlap.
	 *
	 * This has to come between the pop and the push, not before both. The
	 * value stage1 pushed is on *its* stack; changing SP first and then
	 * pushing would leave it behind, and the later `popl` would read four
	 * bytes of whatever happens to sit at the new top of stack. The drive
	 * then reads as 0, the kernel is handed a boot device number of zero,
	 * and nothing downstream can tell that apart from a correct answer. */
	movw	$STAGE2_STACK_TOP, %sp

	pushw	%ax			/* ... and put it on the new stack */

	/*
	 * Install a real IDT *before* the mode switch. Two reasons, and the
	 * second is the urgent one.
	 *
	 * The logical reason: the real-mode IVT is still loaded at this point,
	 * and a far jump changes the meaning of every byte in it. The 16-bit
	 * gate at vector 8 is not a 32-bit gate, so the first interrupt taken
	 * after the switch would be decoded from the wrong shape.
	 *
	 * The urgent reason: without an IDT, an exception taken during the
	 * switch is a triple fault, and a triple fault is a machine reset with
	 * no register state and no explanation. That is precisely how a fault at
	 * the ljmp below presents itself: the machine reboots silently and the
	 * cause has to be guessed at from the surrounding instructions. With a
	 * handler in place the same fault prints CR2 and stops.
	 */
	movw	$idtr, %si
	/* Written as bytes for the same reason as the CR0 sequence below: GAS
	 * assembles `lidtl (%si)` in .code16 as `66 0f 01 1c`, and that prefix
	 * selects the m16&16 form. LIDT would then read a four-byte operand --
	 * a 16-bit limit and a 16-bit base -- leaving the top half of the real
	 * base uninitialised. The loaded table would land in the right place by
	 * luck and contain the idtr structure's own bytes as its first gate, so
	 * the first exception would decode a garbage descriptor and escalate to
	 * a triple fault. The bare `0f 01 1c` is the m16&32 form: 6 bytes, the
	 * full 32-bit base. */
	.byte	0x0f, 0x01, 0x1c

	/* Load the GDTR. In 16-bit mode LGDT takes an m16:32 operand, so the
	 * limit and base live in adjacent words and the instruction reads m16. */
	movw	$gdtr, %si
	lgdt	(%si)

	/*
	 * Enter protected mode. Written as bytes because GAS will not assemble
	 * the equivalent in .code16.
	 *
	 * CR0.PE is bit 0. Bit 31 is CR0.PG, and setting that here is a trap
	 * worth naming: it turns on paging while the CPU is still in real mode
	 * with CR3 = 0, so every instruction fetch is translated through the
	 * null page tables and faults immediately. It presents as a page fault
	 * at whatever instruction happens to be next -- which is a far jump
	 * that never completes -- so the symptom points squarely at the far
	 * jump and not at all at the control register write that caused it.
	 *
	 * Inside .code16 the assembler narrows `orl $imm32, %eax` to a 16-bit OR
	 * of AX, so the 0x66 prefixes here are what force 32-bit operand size on
	 * instructions still being fetched as 16-bit code. The prefixes on the
	 * CR0 moves are not optional either: MOV to and from a control register
	 * takes its register from the operand size, so without one these would
	 * load and store AX and leave the rest of EAX undefined.
	 */
	.byte	0x66, 0x0f, 0x20, 0xc0			/* movl %cr0, %eax           */
	.byte	0x66, 0x83, 0xc8, 0x01			/* orl $0x1, %eax  (PE = b0) */
	.byte	0x66, 0x0f, 0x22, 0xc0			/* movl %eax, %cr0           */

	ljmp	$0x08, $protected_mode

	.code32
protected_mode:
	movw	$0x10, %ax		/* flat data descriptor */
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %fs
	movw	%ax, %gs
	movw	%ax, %ss

	/* Retire the boot drive from the 16-bit stack now that 32-bit addressing
	 * is available. This has to happen before ESP is repointed. */
	popl	%eax
	movb	%al, stage1_boot_drive

	/* The CPU does not switch stacks on a far jump, so the 16-bit stack
	 * pointer is still live. Point ESP at a 32-bit stack allocated above the
	 * stage2 image. */
	movl	$stack32, %esp

	/* Stage2 is loaded by sector rather than through an ELF loader, so the
	 * linker-emitted BSS bounds are the only record of what needs zeroing.
	 * Clearing it before any C runs keeps .bsetup initialised by the C
	 * runtime assumptions the compiler already made. */
	movl	$bss_start, %edi
	movl	$bss_end, %ecx
	subl	%edi, %ecx
	xorl	%eax, %eax
	cld
	rep stosb

	call	stage2_start

	/* stage2_start only returns if the loader gave up. */
	.globl stage2_halt
stage2_halt:
	cli
1:	hlt
	jmp	1b

/* ---------------------------------------------------------------------------
 * The 32-bit GDT used for the initial protected-mode switch. The long-mode
 * switch installs a different table (with the L bit set on the code segment)
 * from boot_gdt_init() in stage2.c.
 * --------------------------------------------------------------------------- */
	.section .rodata
	.align 8
gdt:
	.quad	0			/* 0x00 null */
	.quad	0x00CF9A000000FFFF	/* 0x08 32-bit code, base 0, limit 4G */
	.quad	0x00CF93000000FFFF	/* 0x10 32-bit data, base 0, limit 4G */
	/* gdt_end is what the GDTR limit is measured from, and it is the one
	 * label that cannot be inferred: the entries are 8 bytes each, but the
	 * limit field counts bytes and the table has to end on a descriptor
	 * boundary or the last descriptor is only partly loaded. */
gdt_end:
	.equ	gdt_bytes, gdt_end - gdt
gdtr:
	.word	gdt_bytes - 1
	.long	gdt

/* --------------------------------------------------------------- IDT ------- */

/*
 * 256 32-bit interrupt gates, all pointing at one reporter defined below.
 *
 * The reporter is assembly rather than a call into stage2.c because it has to
 * work before the C runtime is reachable and while the only stack in existence
 * is the 0x7e00 one set a few instructions earlier. A fault taken during the
 * mode switch is exactly the case this exists for, and without it that fault is
 * a triple fault: a machine reset that leaves no register state behind to
 * explain it. This way the same fault prints CR2 and stops.
 *
 * The logical reason for having an IDT here at all: the real-mode IVT is still
 * loaded at this point, and a far jump changes the meaning of every byte in
 * it. The 16-bit gate at vector 8 is not a 32-bit gate, so the first interrupt
 * taken after the switch would be decoded from the wrong shape.
 */
/* --------------------------------------------------- exception reporter ---- */

	.code32

/* outb to COM1, clobbers nothing */
	.set	COM1, 0x3F8

exc_putc:
	movl	%edi, %edx		/* EDX = port; caller passes the byte in EDI */
	movl	%edi, %eax
	outb	%al, (%dx)
	ret

/* Print EAX as 16 hex digits, then a space. Clobbers EBX, ECX, EDX, EDI. */
exc_hex32:
	movl	%eax, %ebx
	movl	$16, %ecx
	movl	$COM1, %edx
1:	roll	$4, %ebx
	movl	%ebx, %edi
	andl	$0xF, %edi
	addl	$'0', %edi
	cmpl	$10, %edi
	jb	2f
	addl	$('a' - '0' - 10), %edi	/* nibbles 10-15 print as a-f */
2:	movl	%ebx, %eax		/* AL must hold the digit, not the shifted value */
	outb	%al, (%dx)
	loop	1b

	movb	$' ', %al
	outb	%al, (%dx)
	ret

exc_puts:
	pushl	%esi
1:	lodsb
	testb	%al, %al
	jz	2f
	movl	%eax, %edi
	call	exc_putc
	jmp	1b
2:	popl	%esi
	ret

/*
 * Common entry for every vector. The stack on arrival is, low to high:
 *   vector, error code, EIP, CS, EFLAGS  (and ESP/SS if the fault came from
 * ring 3, which cannot happen before the kernel installs its own tables).
 */
	.globl	exc_stub		/* stage2.ld splits its address for the gate */
exc_stub:
	pushl	%eax
	pushl	%ebx
	pushl	%ecx
	pushl	%edx
	pushl	%esi
	pushl	%edi

	/* vector sits above the six saved registers, the error code, and CR2. */
	movl	(6 * 4 + 4)(%esp), %eax	/* error code, 0 for vectors without one */
	call	exc_hex32

	movl	%cr2, %eax
	call	exc_hex32

	movl	(6 * 4 + 8)(%esp), %eax	/* vector number */
	call	exc_hex32

	movl	(6 * 4 + 12)(%esp), %eax	/* faulting EIP */
	call	exc_hex32

	movl	$exc_msg, %edi
	call	exc_puts

	/* Stop here. Returning would re-execute the faulting instruction and
	 * fault again forever, which looks exactly like a hang. Falling through
	 * to stage2_halt, which is defined after the call to stage2_start. */
	jmp	stage2_halt

	.section .rodata
exc_msg:
	.asciz	"\r\n[stage2] exception: error CR2 vector RIP\r\n"

/*
 * The table itself, emitted after the stub rather than before it.
 *
 * The gate offset has to be split into a low and a high 16-bit half, and
 * GAS can only do that arithmetic on a symbol it has already resolved. A
 * definition placed before the stub makes exc_stub a forward reference, and
 * `.set STUB_OFF, exc_stub` then fails to assemble -- or, worse, silently
 * evaluates against an assumed value. Ordering it after the stub makes the
 * reference backward and the masking is done by the assembler, which knows
 * the address.
 *
 * A forward reference is fine for `movw $idtr, %si` at the top of the file,
 * because a 16-bit immediate is emitted with a relocation for the linker to
 * fill in. Only the bit splitting needs the value at assembly time.
 */
	.section .rodata
	.align 8
idt:
	/*
	 * One gate, four words, in the order a 32-bit gate stores them:
	 *   offset[15:0] | selector | type/attributes | offset[31:16]
	 *
	 * exc_stub_lo and exc_stub_hi are defined in stage2.ld, where the linker
	 * can split the address. See the comment there for why this is not done
	 * with `.set` and a mask.
	 *
	 * selector 0x08 is GDT index 1, the 32-bit code segment defined above.
	 * type 0x8E is present, DPL 0, a 32-bit *interrupt* gate rather than a
	 * trap gate: an interrupt gate clears IF on entry, so the reporter cannot
	 * be re-entered by the timer while it is printing.
	 */
	.rept	256
	.word	exc_stub_lo
	.word	0x0008
	.word	0x8E00
	.word	exc_stub_hi
	.endr
idtr:
	.word	256 * 8 - 1
	.long	idt

	.section .bss
	.align 16
stack32:
	.space	0x8000			/* 32 KiB of loader stack */

	.globl stage1_boot_drive
stage1_boot_drive:
	.byte	0
