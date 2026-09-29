/*
 * panic.h — unrecoverable kernel failure handling.
 */
#ifndef PANIC_H
#define PANIC_H

#include <types.h>

/*
 * Halt the machine with a diagnostic.
 *
 * The handler must work when almost nothing else does. Specifically it cannot
 * assume: the allocator works (it may be the thing that failed), interrupts are
 * enabled, the scheduler can run, the console lock is free, or the framebuffer
 * is initialised. So panic() owns its output path: raw serial writes, a direct
 * VGA text dump, and a register dump built on a stack buffer.
 *
 * It never returns.
 */
__noreturn void panic(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));

/* Panic with an explicit CPU id, for secondary-CPU assertion failures. */
__noreturn void panic_on_cpu(uint32_t cpu, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

/*
 * Panic if a condition does not hold. Used for invariants that indicate a bug
 * rather than an unexpected runtime condition — the ones where continuing would
 * corrupt memory.
 */
#define BUG_ON(cond)                                                      \
	do {                                                               \
		if (unlikely(cond))                                         \
			panic("BUG at %s:%d: %s", __FILE__, __LINE__, #cond); \
	} while (0)

#define BUG() panic("BUG at %s:%d", __FILE__, __LINE__)

/* Assertion with an explanation, for conditions a reader should see verbatim. */
#define ASSERT_MSG(cond, ...)                                             \
	do {                                                               \
		if (unlikely(!(cond)))                                      \
			panic("assertion failed: " #cond "\n  " __VA_ARGS__);\
	} while (0)

/* Register dump buffer filled by panic(); exposed so the debug subsystem can
 * pick it up without re-deriving it. */
struct panic_state {
	uint64_t rax, rbx, rcx, rdx;
	uint64_t rsi, rdi, rbp, rsp;
	uint64_t r8,  r9,  r10, r11;
	uint64_t r12, r13, r14, r15;
	uint64_t rip, cs, rflags;
	uint64_t cr2, cr3, cr4;
	uint32_t cpu_id;
	char     message[256];
};

extern struct panic_state panic_state;

#endif /* PANIC_H */
