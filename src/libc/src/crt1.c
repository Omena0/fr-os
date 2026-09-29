/*
 * crt1.c — ELF entry point and libc initialization.
 *
 * _start is the global entry symbol. The kernel provides:
 *   RDI = argc, RSI = argv, RDX = envp, RCX = auxv,
 *   RSP points to [argc][argv...][NULL][envp...][NULL][auxv pairs...AT_NULL].
 *
 * _start stashes all four into globals, calls __libc_init() (initialises stdio
 * buffers and allocator arena state), calls main(argc, argv, envp), then
 * exit(ret). Never returns.
 *
 * Also provides: __libc_argc, __libc_argv, __libc_envp, __libc_auxv,
 * __libc_init, __stack_chk_guard (random-ish from auxv AT_RANDOM or TSC),
 * __stack_chk_fail, environ and __environ.
 */
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/auxv.h>
#include <stdlib.h>

/* __MORE__ */

extern void __libc_init(void);
extern int main(int argc, char **argv, char **envp);
extern void exit(int status) __attribute__((noreturn));

int __libc_argc;
char **__libc_argv;
char **__libc_envp;
unsigned long *__libc_auxv;
uintptr_t __stack_chk_guard;

char **environ;
char **__environ;

/* __MORE__ */

static unsigned long rdtsc(void)
{
	unsigned int lo, hi;
	__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
	return ((unsigned long)hi << 32) | lo;
}

static unsigned long get_random_seed(void)
{
	/* Try to get AT_RANDOM from auxv */
	if (__libc_auxv) {
		unsigned long *auxv = __libc_auxv;
		while (auxv[0] != 0) {  /* AT_NULL */
			if (auxv[0] == 25) {  /* AT_RANDOM */
				return *(unsigned long *)auxv[1];
			}
			auxv += 2;
		}
	}
	/* Fallback: TSC */
	return rdtsc();
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
	/* Note: stdio buffer initialization would go here if stdio existed */
}

void __stack_chk_fail(void)
{
	/* Stack smashing detected */
	__asm__ __volatile__("int3" ::: "memory");
	/* If we somehow continue, abort */
	while (1) {
		__asm__ __volatile__("hlt");
	}
}

__attribute__((noreturn, naked))
void _start(void)
{
	__asm__ __volatile__(
		/* RDI=argc, RSI=argv, RDX=envp, RCX=auxv */
		"mov %%rdi, %0\n\t"
		"mov %%rsi, %1\n\t"
		"mov %%rdx, %2\n\t"
		"mov %%rcx, %3\n\t"
		: "=m"(__libc_argc), "=m"(__libc_argv), "=m"(__libc_envp), "=m"(__libc_auxv)
		:
		: "memory"
	);

	/* environ and __environ point to envp */
	environ = __libc_envp;
	__environ = __libc_envp;

	/* Call libc initialization */
	__libc_init();

	/* Call main */
	int ret = main(__libc_argc, __libc_argv, __libc_envp);

	/* Exit with main's return value */
	exit(ret);

	__builtin_unreachable();
}