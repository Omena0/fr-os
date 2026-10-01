/*
 * kmain.S — the kernel's entry point.
 *
 * stage2 long mode entry jumps here with RDI holding the physical address of
 * struct bootinfo. Nothing else in the register file is meaningful: stage2 uses
 * RDI/ESI/EDX itself to stage the transition, so this code must treat every
 * other register as garbage.
 *
 * The code in this file is deliberately restricted to four things that cannot be
 * expressed in C: reading fixed-address linker symbols, switching to a known
 * stack, and the two MSR writes that must happen before any per-CPU code runs.
 * Everything else is in kmain() proper, where the compiler can keep the
 * registers alive across calls.
 *
 * Ordering here is load-bearing, not stylistic:
 *
 *   1. Stack first. Until RSP points at memory the compiler cannot spill a
 *      register, so this must precede the first call.
 *   2. GS second, via MSR_GS_BASE. percpu_setup() is a normal C function that
 *      reads this_cpu(), and this_cpu() is a GS-relative load. If GS were set
 *      after that call, the load would return garbage and the kernel would
 *      scribble somewhere unpredictable.
 *   3. EFER checks last, as assertions rather than setup. Stage2 owns the
 *      long-mode transition; the kernel's job is to notice if it did not happen
 *      and say so, because "kernel reached main in protected mode" is a bug
 *      that would otherwise present much later as an unexplained #GP.
 */

#include <boot.h>

/*
 * The stack top. Placed in the kernel's own BSS by the linker script, so it is
 * always valid regardless of how much BSS stage2 zeroed: stage2 cannot know
 * the kernel's BSS size, and the kernel's .bss is the one region it must not
 * trample. __stack_chk_fail is referenced so a stack-protector build cannot
 * silently lose its canary check on this file.
 */
	.extern __kernel_stack_top

/* The identity mapping stage2 built. Replaced by the VMM's own tables. */
	.extern boot_pml4_phys

	.section .text.boot, "ax", @progbits
	.globl kernel_start
	.type kernel_start, @function
kernel_start:
	/*
	 * Load the stack pointer. RBP is zeroed at the same time: several things
	 * downstream (unwinding, the panic backtrace) treat RBP==0 as the end of
	 * the frame chain, and leaving stage2's frame pointer in place would make
	 * the first backtrace walk into the bootloader's stack.
	 */
	leaq	__kernel_stack_top(%rip), %rsp
	xorq	%rbp, %rbp

	/*
	 * Set the GS base before the first call. percpu_setup() is the first C
	 * function to run, and every per-CPU access in the kernel is GS-relative, so
	 * the base has to be right before the first call rather than after it.
	 *
	 * A plain WRMSR to IA32_GS_BASE (0xC0000101), then read it back.
	 *
	 * This file once preferred WRGSBASE, gated on CPUID.(EAX=7,ECX=0):EBX[0]
	 * with CR4.FSGSBASE enabled immediately before use. The reasoning behind
	 * that change was sound and the change itself was not, so both are recorded
	 * here because the same reasoning will otherwise be repeated:
	 *
	 *   - The #GP that motivated it was not this WRMSR. It was the WRMSR to
	 *     IA32_KERNEL_GS_BASE (0xC0000102), which this file also used to do and
	 *     which has since been removed. Placing `wrmsr 0xC0000102` alone at
	 *     kernel_start reproduces the fault under KVM on `-cpu host` and
	 *     `-cpu max` alike: err=0, CR2=0, RIP = kernel_start+0x1d. TCG accepts
	 *     the identical image. Routing around a *different* MSR's fault by
	 *     changing this one is not a fix.
	 *   - `wrmsr 0xC0000101` does not fault under KVM. It does not fault under
	 *     TCG either. It takes, on both, and the read-back below is what proves
	 *     it: IA32_GS_BASE reads back as percpu_data under both accelerators.
	 *   - FSGSBASE is not advertised by either accelerator as configured here
	 *     (`-cpu host`, `-cpu max`, TCG and KVM), so the WRGSBASE path was never
	 *     taken on the machine all of this was measured on.
	 *
	 * So the gate bought nothing and cost a CR4 write. CR4.FSGSBASE is left
	 * clear, which is also the safer of the two states: WRGSBASE is not a
	 * privileged instruction, so enabling CR4.FSGSBASE hands CPL 3 the ability
	 * to set GS.base to any canonical address, and the syscall path reaches the
	 * per-CPU area through GS. In 64-bit mode the base fields of the FS/GS
	 * descriptors are ignored, so while CR4.FSGSBASE is clear the only ways for
	 * ring 3 to move GS.base are the privileged ones. One serialising WRMSR,
	 * once per boot, is worth keeping that property.
	 *
	 * R11 carries the expected address because CPUID overwrites EAX -- and so all
	 * of RAX -- as well as EBX, ECX and EDX. A version of this block kept the
	 * address in RAX, ran `mov $7,%eax; cpuid` over it, and then wrote whatever
	 * CPUID returned into the GS base.
	 *
	 * The three instructions that split the address are not decoration either.
	 * WRMSR takes the MSR value from EDX:EAX, so the *high* half of the address
	 * has to be in EDX. Loading EDX with the whole 64-bit address --
	 * `movq %rax, %rdx`, which is what this file has always done -- puts
	 * percpu_data's low 32 bits in EDX and stores 0x000080168016c300 where
	 * 0xffffffff8016c300 was meant. That value is a canonical address inside
	 * the loader's identity window, so it is mapped and writable: it produced no
	 * fault and no diagnostic, only a kernel whose every per-CPU access went to
	 * the wrong place. Under KVM it was also rejected outright -- MSR_GS_BASE is
	 * serviced by the hypervisor there, and the malformed value came back as #GP
	 * at the wrmsr. Both symptoms had this one cause, which is why the earlier
	 * attempts to explain them as "KVM filters the FSGSBASE MSRs" all led
	 * nowhere: the filter, if any, was never the problem.
	 */
	/*
	 * Install it: WRMSR takes its value from EDX:EAX, so the address has to be
	 * split by hand.
	 */
	movq	$percpu_data, %r11
	movl	%r11d, %eax			/* MSR low 32  */
	shrq	$32, %r11
	movl	%r11d, %edx			/* MSR high 32 */
	movq	$percpu_data, %r11		/* expected value, for the check below */
	movl	$0xC0000101, %ecx		/* MSR_GS_BASE */
	wrmsr

	/*
	 * Read IA32_GS_BASE back with RDMSR, and reassemble RDX:EAX so it is one
	 * comparison.
	 *
	 * Two claims about this are worth killing off explicitly, because both have
	 * been in this file and both were checked on TCG *and* KVM:
	 *
	 *   - "MOV from a segment register is 32-bit in long mode and REX.W is
	 *     ignored, so `mov %gs, %rax` returns the selector." That half is right:
	 *     measured with GS.base = 0xffffffff8016c300, `mov %gs, %eax` returns
	 *     0x10 -- selector 4 in the GDT -- and hand-writing REX.W (48 8c e8)
	 *     changes nothing. It still returns 0x10.
	 *   - "so with REX.W it returns the full 64-bit base." This half is false.
	 *     The instruction is accepted and the selector still comes back.
	 *
	 * Either way, `mov %gs, %rax` cannot verify a GS base, so a read-back built
	 * on it reports failure on a correctly installed base. There is no RDGSBASE
	 * on Intel; RDFSGSBASE reads FS. RDMSR is the only read of GS.base, and it
	 * is kept in step with WRGSBASE by the hardware, so it covers both ways of
	 * installing the base.
	 *
	 * A base that silently did not take would not fault at the next
	 * instruction: this_cpu() would return garbage and every subsystem indexing
	 * by cpu_id would read and write through it, into a page that is mapped and
	 * writable, so there is no #PF pointing at the mistake.
	 */
	movl	$0xC0000101, %ecx		/* MSR_GS_BASE */
	rdmsr
	movq	%rax, %r10
	shlq	$32, %rdx
	orq	%rdx, %r10
	cmpq	%r11, %r10
	jne	.Lgs_base_failed

	/*
	 * IA32_KERNEL_GS_BASE (0xC0000102) is deliberately NOT written, and the
	 * value it holds is load-bearing.
	 *
	 * `swapgs` exchanges IA32_GS_BASE with IA32_KERNEL_GS_BASE. It is not a
	 * no-op, and nothing here can make it one: the only instruction that writes
	 * the kernel half is a WRMSR to 0xC0000102, which faults under KVM
	 * whatever it is given. Its reset value is 0, so what the hardware hands
	 * the syscall path is the arrangement Linux uses:
	 *
	 *     ring 0  IA32_GS_BASE       = &percpu_data
	 *     ring 3  IA32_KERNEL_GS_BASE = 0, i.e. GS.base == 0
	 *
	 * A `swapgs` on syscall entry therefore loads GS.base with 0, and the
	 * matching `swapgs` on exit puts &percpu_data back. Anything in the syscall
	 * path that reads the per-CPU area through GS *after* the swap is reading
	 * address 0 -- which is mapped, so it will not fault, and there is no
	 * #PF to point at the mistake. That is syscall_entry.S's problem to solve,
	 * and it has to be solved by taking the kernel's GS base from a register the
	 * user cannot influence, never by trusting %gs once the swap has happened.
	 */

	/*
	 * Fail loudly and early if the long-mode transition did not happen. The
	 * symptom of skipping this check is not a crash here: it is a kernel that
	 * runs, prints through a silently misidentified mode, and corrupts memory
	 * somewhere that gives no hint about the real cause. Two instructions to
	 * turn a day of debugging into a message.
	 */
	/*
	 * LMA is bit 10 of EFER, and RDMSR leaves the low half of the MSR in EAX.
	 *
	 * This read EFER, shifted RAX left by 8 and then tested bit 8 -- which is
	 * a two-step way of testing bit 16 of a value whose bit 10 is the one that
	 * matters. EFER.LMA is set by the hardware the moment paging is enabled in
	 * long mode, so it is always set here, and this check rejected every
	 * correct boot. The shift then bit 10 down to bit 2, and `btl $8` on that
	 * never matches either, so the jump was always taken and the kernel went
	 * straight to its "not long mode" path: a message to port 0xE9, which
	 * nothing is listening on, and a halt. Serial stayed completely silent.
	 */
	movq	$0xC0000080, %rcx		/* MSR_EFER */
	rdmsr
	btl	$10, %eax			/* EFER.LMA */
	jnc	.Lnot_long_mode

	/*
	 * Pass bootinfo across to C. RDI already holds this address, put there by
	 * stage2's far jump. Reloading it here only gave a second place for the
	 * two images to disagree about where the record lives, and they did: this
	 * was E820_ADDR, the 4 KiB page below the record, so kmain read the
	 * memory map's first bytes as a magic number and panicked every boot.
	 * BOOTINFO_ADDR is now a shared constant in boot.h.
	 */
	movq	$BOOTINFO_ADDR, %rdi

	/* Clear the direction flag: the BIOS can leave it set, and it silently
	 * turns every string primitive backwards. */
	cld
	xorl	%ebx, %ebx
	xorl	%ebp, %ebp

	call	kmain
	.size kernel_start, . - kernel_start

	/*
	 * kmain() is not expected to return — it ends in the idle loop, and a
	 * return would mean the idle task was torn down. Halt forever rather than
	 * running off the end of an unknown stack.
	 */
.Lhang:
	cli
	hlt
	jmp	.Lhang

.Lnot_long_mode:
	/*
	 * No console yet: the framebuffer and serial backends are set up from C
	 * and neither is guaranteed to exist yet. Port 0xE9 is the diagnostic port
	 * the platform debug card exposes on QEMU, and it needs no
	 * initialisation, so it is the only sink available this early.
	 */
	/* RIP-relative, not `movq $LMM_REPORT`. The immediate form asks the
	 * assembler for an absolute relocation against the symbol, and a local
	 * label beginning with `.L` is never emitted into the symbol table, so
	 * the reference is unresolvable at link time. */
	leaq	.LMM_REPORT(%rip), %rsi
	jmp	.Lreport_e9

.Lgs_base_failed:
	/*
	 * The GS base did not take, and there is no correct way to continue: the
	 * per-CPU area is unreachable, this_cpu() returns garbage, and every
	 * subsystem that indexes by cpu_id would silently read and write the low
	 * identity window. Halting here with a message beats booting into that.
	 *
	 * The value that was actually read back is printed too, because "it did not
	 * take" is only half a diagnosis. GS.base == 0 means the MSR write was
	 * dropped; GS.base == 0x10 means the *selector* came back, i.e. a 32-bit
	 * MOV from the segment register was used instead of a 64-bit one; anything
	 * else is a value this kernel did not ask for and needs explaining.
	 */
	leaq	.LGS_FAIL(%rip), %rsi
	movq	%r10, %r13			/* what the MSR actually holds */
	movl	$16, %ecx
.Lgs_fail_hex:
	rolq	$4, %r13
	movl	%r13d, %edx
	andl	$0xF, %edx
	leaq	.Lhexdigits(%rip), %rdi
	movzbl	(%rdi,%rdx,1), %eax

	movw	$0x3F8, %dx			/* COM1, then 0xE9 */
	outb	%al, %dx
	movw	$0xE9, %dx
	outb	%al, %dx

	decl	%ecx
	jnz	.Lgs_fail_hex

	movb	$'\r', %al
	movw	$0x3F8, %dx
	outb	%al, %dx
	movw	$0xE9, %dx
	outb	%al, %dx
	movb	$'\n', %al
	outb	%al, %dx

/*
 * Print the NUL-terminated string at %rsi to port 0xE9 and halt. Port 0xE9 is
 * the QEMU/Bochs debug console: one byte out per character, no initialisation,
 * no dependency on anything the kernel has set up. The only sink that exists at
 * this point in the boot.
 */
.Lreport_e9:
	movq	%rsi, %r15			/* string start, kept for the 2nd pass */
	movw	$0x3F8, %dx			/* COM1 -- stage2 already programmed it */
.Lreport_serial:
	lodsb
	testb	%al, %al
	jz	.Lreport_debug_port
	outb	%al, %dx
	jmp	.Lreport_serial
.Lreport_debug_port:
	movq	%r15, %rsi
	movw	$0xE9, %dx			/* QEMU/Bochs debug console */
.Lreport_e9_loop:
	lodsb
	testb	%al, %al
	jz	.Lreport_done
	outb	%al, %dx
	jmp	.Lreport_e9_loop
.Lreport_done:
	cli
	hlt
	jmp	.Lreport_done
	.size kernel_start, . - kernel_start

	.section .rodata
.LMM_REPORT:
	.asciz "OS: stage2 did not enter long mode; kernel cannot start.\r\n"
.LGS_FAIL:
	.asciz "OS: IA32_GS_BASE did not take. GS.base = 0x"
.Lhexdigits:
	.asciz "0123456789abcdef"

/*
 * kernel_panic_halt — the last instruction executed by a panicked kernel.
 *
 * Deliberately does not call into C. The panic path may be running with the
 * console lock held, the allocator in an inconsistent state, or on a stack
 * that has already overflowed; a path that only touches the CPU is the only
 * one guaranteed to execute.
 */
	.section .text, "ax", @progbits
	.globl kernel_panic_halt
	.type kernel_panic_halt, @function
kernel_panic_halt:
	cli
1:	hlt
	jmp	1b
	.size kernel_panic_halt, . - kernel_panic_halt

	.section .note.GNU-stack, "", @progbits
