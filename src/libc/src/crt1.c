/*
 * crt1.c — ELF entry point and libc initialization.
 *
 * _start is the global entry symbol. The kernel enters a process through
 * iretq (process.c's process_enter_user -> ret_to_user), so the only state it
 * can hand over is the stack pointer. At the first instruction of _start the
 * stack holds the standard process vector, exactly as Linux builds it:
 *
 *   sp + 0            argc
 *   sp + 8            argv[0] .. argv[argc-1], then NULL
 *                     envp[0] .. , then NULL
 *                     auxv key/value pairs, terminated by AT_NULL
 *
 * The register-only SysV entry ABI (argc in RDI, argv in RSI, envp in RDX,
 * auxv in RCX) is a *glibc* convention, adopted here by an earlier version of
 * this file. iretq restores only RIP, CS, RFLAGS, RSP and SS, so RDI/RSI/RDX/RCX
 * hold whatever the kernel's last C statement left in them and every one of
 * them is garbage from the process's point of view.
 *
 * _start reads the vector off the stack, publishes it, establishes the thread
 * pointer, runs the .init_array constructors, calls __libc_init(), then
 * main(argc, argv, envp), then exit(ret). Never returns.
 *
 * Also provides: __libc_argc, __libc_argv, __libc_envp, __libc_auxv,
 * __libc_init, __libc_setup_tls, __libc_tls_sentinel, __stack_chk_guard (from
 * auxv AT_RANDOM or the TSC), __stack_chk_fail, environ and __environ.
 *
 * The order of the first three is not a preference. A `__thread` access is a
 * load relative to FS, and nothing about ring 3 establishes FS: the process
 * arrives from an iretq that restored RIP, CS, RFLAGS, RSP and SS and nothing
 * else, so the first thing that can make a thread-local variable reachable is
 * this file. libc's own globals are thread-local (errno is), so a constructor
 * that called into stdio before the thread pointer was installed would fault
 * at a low address rather than doing anything diagnosable.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <sys/auxv.h>
#include <stdlib.h>

extern void __libc_init(void);
extern int main(int argc, char **argv, char **envp);
extern void exit(int status) __attribute__((noreturn));

/* Raw syscall trampolines, for the paths that must not touch errno. */
extern long __syscall1(long number, long a1);
extern long __syscall3(long number, long a1, long a2, long a3);

extern long sys_arch_prctl(unsigned long code, unsigned long addr);
extern long sys_get_tls_base(void);

int __libc_argc;
char **__libc_argv;
char **__libc_envp;
unsigned long *__libc_auxv;
uintptr_t __stack_chk_guard;

char **environ;
char **__environ;

/*
 * The constructor table. GNU ld emits both of these for the .init_array output
 * section, but only when something references them, which is why they are named
 * here rather than left to the linker: without a reference the section still
 * exists and still holds every __attribute__((constructor)) in the program, and
 * nothing would ever call it. When the program has no constructors at all the
 * two symbols are equal and the loop below does nothing.
 */
extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);

static unsigned long rdtsc(void)
{
	unsigned int lo, hi;

	__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
	return ((unsigned long)hi << 32) | lo;
}

static unsigned long get_random_seed(void)
{
	/* AT_RANDOM's value is the *address* of 16 kernel-supplied random
	 * bytes, not the bytes themselves. */
	if (__libc_auxv) {
		unsigned long *auxv = __libc_auxv;

		while (auxv[0] != AT_NULL) {
			if (auxv[0] == AT_RANDOM) {
				unsigned long seed;

				/* Use memcpy instead of a pointer cast:
				 * auxv[1] is a kernel-supplied address
				 * whose alignment isn't guaranteed for
				 * unsigned long access. */
				__builtin_memcpy(&seed, (void *)(uintptr_t)auxv[1], sizeof(seed));
				return seed;
			}
			auxv += 2;
		}
	}
	/* Fallback: TSC */
	return rdtsc();
}

static void run_init_array(void)
{
	void (**ctor)(void);

	for (ctor = __init_array_start; ctor < __init_array_end; ctor++)
		(*ctor)();
}

/* ------------------------------------------------------- thread locals ----- */

/*
 * A thread-local variable in the startup object, and the reason it exists here
 * rather than in a header.
 *
 * Its purpose is to make "this image has thread-local storage" a fact the
 * program can check instead of assume. crt1.o is linked into every program
 * that has a main, so its .tdata is linked into every image, so every image
 * has a PT_TLS -- and therefore a nonzero answer from SYS_get_tls_base. That
 * turns "the kernel gave us no thread pointer" from a condition libc has to
 * reason about into a state that cannot occur without something having gone
 * wrong, which is the only way to make a check on it meaningful. Without this
 * sentinel the same check would have to be conditional on the program having
 * thread-locals, which is precisely the thing libc cannot see.
 *
 * The initial value doubles as a read-back check on the thread pointer itself.
 * The address the compiler will use for this variable is fixed at link time as
 * a displacement from FS, and the kernel's answer is the value that makes that
 * displacement land inside the block; if the two ever disagree, the byte
 * written here is not the byte read back, and the comparison below is what
 * notices -- before main, before any constructor, and while there is still a
 * diagnostic worth printing.
 */
#define LIBC_TLS_SENTINEL  0x4c6962635f544c53ULL	/* "Libc_TLS" */

__thread unsigned long __libc_tls_sentinel = LIBC_TLS_SENTINEL;

/*
 * Report a process that cannot have thread-local storage and stop, rather than
 * letting the first `__thread` access fault.
 *
 * The write is a raw syscall for a specific reason: libc's write() reports
 * failure by storing into __errno, __errno is thread-local, and the thread
 * pointer is what is broken. Going through the wrapper would turn a
 * diagnosable refusal to start into a #PF on the way to printing the
 * diagnosis. SYS_exit rather than _exit() for the same reason -- exit() runs
 * atexit handlers and flushes stdio, both of which reach errno.
 */
static __attribute__((noreturn)) void tls_fatal(const char *msg)
{
	__syscall3(SYS_write, 2, (long)msg, (long)strlen(msg));
	__syscall1(SYS_exit, 127);
	for (;;)
		__asm__ __volatile__("hlt");
}

/*
 * Install the thread pointer, before anything can use one.
 *
 * The value comes from the kernel because there is nowhere else in this tree to
 * get it: the auxiliary vector is assembled by the exec path, which cannot be
 * reached from here, and there is no dynamic linker to read PT_TLS out of the
 * program headers and pass it on. SYS_get_tls_base returns the address one
 * past the end of the image's TLS block, which is the value a local-exec
 * `__thread` access is written against -- the end, not the start. The kernel
 * side of that arithmetic, and the measurement it comes from, are in elf.c.
 */
static void __libc_setup_tls(void)
{
	unsigned long base = (unsigned long)sys_get_tls_base();

	if (!base) {
		tls_fatal("libc: the kernel loaded no TLS block for this "
			  "image, so every __thread access is a null-pointer "
			  "dereference. Refusing to start.\n");
	}

	if (sys_arch_prctl(ARCH_SET_FS, base) != 0) {
		tls_fatal("libc: arch_prctl(ARCH_SET_FS) failed; no thread "
			  "pointer, so __thread cannot work. Refusing to "
			  "start.\n");
	}

	if (__libc_tls_sentinel != LIBC_TLS_SENTINEL) {
		tls_fatal("libc: thread-local storage did not read back the "
			  "value the image was linked with; the thread pointer "
			  "is wrong. Refusing to start.\n");
	}
}

void __libc_init(void)
{
	/* Initialize stack guard */
	__stack_chk_guard = get_random_seed();
	/* Initialize allocator (creates initial arena if needed) */
	/* malloc(1) will trigger arena creation */
	void *p = malloc(1);

	if (p)
		free(p);
}

void __stack_chk_fail(void)
{
	/* Stack smashing detected */
	__asm__ __volatile__("int3" ::: "memory");
	/* If we somehow continue, abort */
	for (;;)
		__asm__ __volatile__("hlt");
}

/*
 * The kernel's half of the entry contract: read the process vector off the
 * stack, publish it, and hand over to main. Kept out of _start so that the
 * only code between the iretq target and the first ordinary C function is the
 * register shuffle below.
 *
 * `used` because the only reference to it is the `call` inside _start's
 * assembly template, which the C front end cannot see: without it GCC treats
 * the static function as unreferenced, does not emit its body, and the link
 * fails on an undefined symbol that the source plainly defines.
 */
static __attribute__((used, noreturn)) void
__libc_start_c(unsigned long *sp)
{
	char **argv;

	__libc_argc = (int)sp[0];
	argv = (char **)&sp[1];
	__libc_argv = argv;
	__libc_envp = argv + __libc_argc + 1;

	/* envp[] is NULL-terminated; the auxv pairs start one word later. */
	{
		unsigned long *scan = (unsigned long *)__libc_envp;

		while (*scan)
			scan++;
		__libc_auxv = scan + 1;
	}

	environ = __libc_envp;
	__environ = __libc_envp;

	/*
	 * Before the constructors, and before __libc_init. Both can reach
	 * thread-local storage -- a constructor that calls into stdio touches
	 * errno, and __libc_init() calls malloc() -- and neither can be trusted
	 * to do so before the thread pointer exists.
	 */
	__libc_setup_tls();

	/*
	 * Constructors run before __libc_init and before main. A constructor
	 * that calls into stdio or the allocator therefore sees a libc whose
	 * globals are published but whose arena does not exist yet; that is the
	 * same guarantee main() gets, and __libc_init is deliberately not made
	 * to run first because a constructor may legitimately be what decides
	 * the process is worth starting at all.
	 */
	run_init_array();

	__libc_init();

	int ret = main(__libc_argc, __libc_argv, __libc_envp);

	exit(ret);
}

__attribute__((noreturn, naked))
void _start(void)
{
	/*
	 * Written in the no-operand asm form on purpose. Inside a naked
	 * function GCC hands the template straight to the assembler without
	 * substituting operands, so an operand list here would make it try to
	 * read %ebp and %rsp as operand references and fail to parse. There is
	 * nothing to pass anyway: the stack pointer is the argument.
	 *
	 * RSP is already 16-byte aligned: ret_to_user pads the iretq frame so
	 * that its padding pop lands the user on the boundary. The AND is belt
	 * and braces -- a misaligned first call would misalign every stack the
	 * program ever has, and nothing later could recover it.
	 *
	 * __libc_start_c is called rather than jumped to, so that RSP is 8 mod
	 * 16 at the top of its own frame, per the SysV call convention, which
	 * is what lets it be an ordinary C function.
	 */
	__asm__(
		"xorl %ebp, %ebp\n\t"        /* terminate the backtrace chain */
		"movq %rsp, %rdi\n\t"
		"andq $-16, %rsp\n\t"
		"call __libc_start_c\n\t"
		"hlt\n\t");
}
