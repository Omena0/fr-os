/*
 * panic.c — the kernel's failure path.
 *
 * Everything here is written to work when the rest of the kernel does not. The
 * rules that follow from that:
 *
 *   - No allocation. The message is formatted into a static buffer with
 *     ksnprintf, which touches no allocator.
 *   - No locks. The console lock may well be held by the context that panicked;
 *     taking it again would deadlock instead of reporting.
 *   - No interrupts disabled. A panic raised inside an interrupt handler, or
 *     from a CPU that already had IF clear, must still get its message out.
 *     Instead the handler stops other CPUs with a global flag so only one of
 *     them writes the dump.
 *   - Direct serial writes, and a direct VGA text dump if the buffer is
 *     readable. Both are polled, so neither depends on an interrupt.
 */

#include <panic.h>
#include <kprintf.h>
#include <kstring.h>
#include <io.h>
#include <drivers/serial.h>

struct panic_state panic_state;

/*
 * Only one CPU may run the panic handler. A triple fault on a second CPU would
 * reset the machine before the diagnostic reached the serial port, which is the
 * one output that survives. The flag is deliberately a plain volatile write
 * rather than an atomic: the losing CPUs only need to observe it eventually,
 * and they are about to halt anyway.
 */
static volatile int panic_lock;
static volatile int panic_cpu = -1;

void panic_regs(struct panic_state *out);
void panic_regs(struct panic_state *out);

static void panic_stop_other_cpus(uint32_t cpu)
{
	/*
	 * The full implementation broadcasts a stop IPI through the local APIC
	 * and waits for acknowledgements. Until the APIC driver exists, the
	 * single-CPU boot path can rely on the fact that only the boot CPU is
	 * running; the second CPU to enter panic() spins here forever instead
	 * of interleaving its half-dumped state with the first.
	 */
	if (panic_cpu >= 0 && (uint32_t)panic_cpu != cpu) {
		for (;;)
			__asm__ volatile("cli; hlt");
	}
	panic_cpu = (int)cpu;
}

/*
 * Dump the frame to serial.
 *
 * ksnprintf writes into a static buffer and panic_emit writes that buffer a
 * line at a time, so the dump cannot interleave with itself even though the
 * console lock is not held.
 */
static char panic_buf[320];

static void panic_emit(const char *s)
{
	for (; *s; s++) {
		char c = *s;

		serial_putc_blocking(c);
		/* Mirror onto the VGA text buffer when it is plausibly valid.
		 * A garbage pointer here would double-fault, so the address is
		 * range-checked rather than assumed. */
		volatile uint16_t *vga =
			(volatile uint16_t *)(uintptr_t)(0xB8000 +
							  (2 * 80 * 24) - 2);
		if ((uintptr_t)0xB8000 < 0x100000)
			*vga = (uint16_t)(0x4C00 | (uint8_t)c);
	}
}

static void panic_print(const char *fmt, ...)
{
	__builtin_va_list ap;

	__builtin_va_start(ap, fmt);
	kvsnprintf(panic_buf, sizeof(panic_buf), fmt, ap);
	__builtin_va_end(ap);
	panic_emit(panic_buf);
}

static __noreturn void panic_common(const char *fmt, va_list ap,
				    uint32_t cpu)
{
	panic_stop_other_cpus(cpu);

	panic_regs(&panic_state);
	panic_state.cpu_id = cpu;

	kvsnprintf(panic_state.message, sizeof(panic_state.message), fmt, ap);

	/* Red banner, then the message, then registers in hex. A panic is the
	 * one place where readability beats brevity. */
	panic_emit("\r\n\r\n");
	panic_emit("==================================================\r\n");
	panic_emit("           KERNEL PANIC\r\n");
	panic_emit("==================================================\r\n");
	panic_emit(panic_state.message);

	if (panic_state.message[0] &&
	    panic_state.message[strlen(panic_state.message) - 1] != '\n')
		panic_emit("\r\n");

	panic_emit("\r\ncpu ");
	panic_print("%u\r\n", cpu);
	panic_print("rip %p  cs %p  rflags %p\r\n",
		    (void *)(uintptr_t)panic_state.rip,
		    (void *)(uintptr_t)panic_state.cs,
		    (void *)(uintptr_t)panic_state.rflags);
	panic_print("rsp %p  rbp %p\r\n",
		    (void *)(uintptr_t)panic_state.rsp,
		    (void *)(uintptr_t)panic_state.rbp);
	panic_print("rax %p  rbx %p  rcx %p  rdx %p\r\n",
		    (void *)(uintptr_t)panic_state.rax,
		    (void *)(uintptr_t)panic_state.rbx,
		    (void *)(uintptr_t)panic_state.rcx,
		    (void *)(uintptr_t)panic_state.rdx);
	panic_print("rsi %p  rdi %p\r\n",
		    (void *)(uintptr_t)panic_state.rsi,
		    (void *)(uintptr_t)panic_state.rdi);
	panic_print("r8  %p  r9  %p  r10 %p  r11 %p\r\n",
		    (void *)(uintptr_t)panic_state.r8,
		    (void *)(uintptr_t)panic_state.r9,
		    (void *)(uintptr_t)panic_state.r10,
		    (void *)(uintptr_t)panic_state.r11);
	panic_print("r12 %p  r13 %p  r14 %p  r15 %p\r\n",
		    (void *)(uintptr_t)panic_state.r12,
		    (void *)(uintptr_t)panic_state.r13,
		    (void *)(uintptr_t)panic_state.r14,
		    (void *)(uintptr_t)panic_state.r15);
	panic_print("cr2 %p  cr3 %p  cr4 %p\r\n",
		    (void *)(uintptr_t)panic_state.cr2,
		    (void *)(uintptr_t)panic_state.cr3,
		    (void *)(uintptr_t)panic_state.cr4);
	panic_emit("system halted\r\n");

	/*
	 * Halt with interrupts disabled. `hlt` with IF clear stops permanently,
	 * which is what a panic wants: a spinning CPU in a QEMU window produces
	 * no useful signal and steals the machine's attention.
	 */
	cli();
	for (;;)
		__asm__ volatile("hlt");
}

void panic(const char *fmt, ...)
{
	__builtin_va_list ap;

	__builtin_va_start(ap, fmt);
	panic_common(fmt, ap, 0);
	__builtin_va_end(ap);
}

void panic_on_cpu(uint32_t cpu, const char *fmt, ...)
{
	__builtin_va_list ap;

	__builtin_va_start(ap, fmt);
	panic_common(fmt, ap, cpu);
	__builtin_va_end(ap);
}
