/*
 * unistd.c — POSIX unistd wrappers over the kernel ABI.
 *
 * write, read, close, open, lseek, fsync, isatty, unlink, sleep, usleep,
 * nanosleep, alarm, pause, getpid, gettid, getppid, sysconf, getpagesize,
 * getauxval, _exit, execve, fork, waitpid, wait4, pipe, chdir, getuid,
 * geteuid, getgid, getegid, fchdir, truncate, getcwd, dup, dup2.
 *
 * getauxval searches the saved auxv (__libc_auxv set by crt1) for the type.
 * The kernel publishes AT_PHDR, AT_PHNUM, AT_PAGESZ, AT_BASE, AT_ENTRY,
 * AT_UID, AT_EUID, AT_GID, AT_RANDOM and AT_ABI_VERSION, terminated by
 * AT_NULL (process.c build_user_stack). There is no AT_EGID.
 * sysconf handles _SC_PAGESIZE, _SC_NPROCESSORS_ONLN, _SC_PID_MAX,
 * _SC_USER_PROCESSES; others return -1/ENOSYS.
 * fork calls sys_fork (declared extern, kernel-side concurrent) and resets
 * allocator state in the child.
 * execve calls sys_execve (declared extern, defined in syscall.c).
 * sleep/usleep via sys_nanosleep with EINTR retry. pause is one nanosleep on
 * an effectively infinite request (documented simplification).
 * getcwd, unlink, rmdir, access, chdir, truncate, ftruncate return -1/ENOSYS
 * (honest absences): there is no filesystem and no writable file behind an fd.
 */
#include <unistd.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <stdarg.h>

/* Missing POSIX types not in sys/types.h */
typedef uint32_t uid_t;
typedef uint32_t gid_t;


/* Internal syscall wrappers (from syscall.c) */
extern int sys_write(int fd, const void *buf, size_t count);
extern int sys_read(int fd, void *buf, size_t count);
extern int sys_close(int fd);
extern int sys_open(const char *path, int flags, mode_t mode);
extern off_t sys_lseek(int fd, off_t offset, int whence);
extern pid_t sys_getpid(void);
extern pid_t sys_gettid(void);
extern pid_t sys_getppid(void);
extern int sys_sched_yield(void);
extern int sys_clock_gettime(clockid_t clk_id, struct timespec *tp);
extern int sys_nanosleep(const struct timespec *req, struct timespec *rem);
extern int sys_pipe(int pipefd[2]);
extern void sys_exit_group(int status);
extern int sys_fstat(int fd, void *st);  /* for isatty */
extern int sys_dup(int oldfd);
extern int sys_dup2(int oldfd, int newfd);
extern int sys_execve(const char *path, char *const argv[], char *const envp[]);
extern pid_t sys_fork(void);
extern pid_t sys_wait4(pid_t pid, int *wstatus, int options, void *rusage);
extern uid_t sys_getuid(void);
extern uid_t sys_geteuid(void);
extern gid_t sys_getgid(void);
extern gid_t sys_getegid(void);
extern int sys_getcpu(unsigned *cpu, void *cache);
extern int sys_ioctl(int fd, unsigned long request, ...);

/* sys_mmap / sys_munmap / sys_mprotect live in syscall.c, which owns the ABI
 * wrappers; they are redeclared here because sys/mman.h exposes only the public
 * mmap/munmap/mprotect spellings. */
extern void *sys_mmap(void *addr, size_t length, int prot, int flags, int fd,
		      off_t offset);
extern int sys_munmap(void *addr, size_t length);
extern int sys_mprotect(void *addr, size_t len, int prot);

/* sys_exit terminates one task; sys_exit_group the process. _exit(2) is
 * specified to do the first. */
extern void sys_exit(int status) __attribute__((noreturn));

/* crt1.c owns the process vector. The type here has to match its definition
 * exactly: declaring it as `void *` compiled and linked, and was still a
 * constraint violation (C11 6.2.7) that happened to be harmless because both
 * are pointers. */
extern unsigned long *__libc_auxv;
extern void __libc_init(void);

/* Reset allocator state after fork */
extern void __malloc_fork_child(void);

/* Forward declare wait4 for waitpid */
pid_t wait4(pid_t pid, int *wstatus, int options, void *rusage);

/* -------------------------- basic I/O wrappers ----------------------------- */

int write(int fd, const void *buf, size_t count)
{
	return sys_write(fd, buf, count);
}

int read(int fd, void *buf, size_t count)
{
	return sys_read(fd, buf, count);
}

int close(int fd)
{
	return sys_close(fd);
}

int open(const char *pathname, int flags, ...)
{
	mode_t mode = 0;
	if (flags & O_CREAT) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}
	return sys_open(pathname, flags, mode);
}

off_t lseek(int fd, off_t offset, int whence)
{
	return sys_lseek(fd, offset, whence);
}

int fsync(int fd)
{
	(void)fd;
	/* No persistent storage; treat as success */
	return 0;
}

int fdatasync(int fd)
{
	return fsync(fd);
}

int ftruncate(int fd, off_t length)
{
	(void)fd;
	(void)length;
	__errno = ENOSYS;
	return -1;
}


int isatty(int fd)
{
	/* Only fd 0,1,2 are valid and they're ttys */
	return (fd >= 0 && fd <= 2) ? 1 : 0;
}

int dup(int oldfd)
{
	return sys_dup(oldfd);
}

int dup2(int oldfd, int newfd)
{
	return sys_dup2(oldfd, newfd);
}

int pipe(int pipefd[2])
{
	return sys_pipe(pipefd);
}

/* --------------------------- process management ---------------------------- */

pid_t getpid(void)
{
	return sys_getpid();
}

pid_t gettid(void)
{
	return sys_gettid();
}

pid_t getppid(void)
{
	return sys_getppid();
}

pid_t fork(void)
{
	pid_t ret = sys_fork();

	if (ret == 0) {
		/* Child process: reset allocator state and reinitialize.
		 * __malloc_fork_child() drops the parent's cached chunks so
		 * the child does not hand out blocks the parent still owns.
		 * A fresh arena is created by the first malloc(1) below.
		 *
		 * __libc_init() is NOT called here: it re-runs the full libc
		 * startup (stack guard, atexit, etc.) which only needs to
		 * happen once, at process creation. The allocator reset is
		 * all the child needs. */
		__malloc_fork_child();
		{
			void *p = malloc(1);

			if (p)
				free(p);
		}
	}
	return ret;
}

int execve(const char *path, char *const argv[], char *const envp[])
{
	return sys_execve(path, argv, envp);
}

pid_t waitpid(pid_t pid, int *wstatus, int options)
{
	return wait4(pid, wstatus, options, NULL);
}

pid_t wait4(pid_t pid, int *wstatus, int options, void *rusage)
{
	return sys_wait4(pid, wstatus, options, rusage);
}

void _exit(int status)
{
	/*
	 * SYS_exit, not SYS_exit_group: _exit(2) terminates the calling thread
	 * and leaves the rest of the process alone. Reaching for exit_group
	 * here is indistinguishable from exit() in every program that has one
	 * thread -- which is all of them today -- and wrong in the first
	 * program that has two.
	 */
	sys_exit(status);
}


/* ------------------------------- time/sleep -------------------------------- */

/*
 * sleep returns the number of seconds left unslept, which is 0 when the sleep
 * completed. The kernel only writes *rem on -EINTR, so the "did it finish?"
 * question cannot be answered by reading rem afterwards -- that read an
 * uninitialised stack frame. It is answered by whether nanosleep returned 0.
 */
int sleep(unsigned int seconds)
{
	struct timespec req = { .tv_sec = seconds, .tv_nsec = 0 };
	struct timespec rem = { .tv_sec = 0, .tv_nsec = 0 };

	while (nanosleep(&req, &rem) == -1) {
		if (__errno != EINTR)
			break;
		req = rem;
	}
	return (int)rem.tv_sec;
}

int usleep(useconds_t usec)
{
	struct timespec req = { .tv_sec = usec / 1000000, .tv_nsec = (usec % 1000000) * 1000 };
	struct timespec rem = { .tv_sec = 0, .tv_nsec = 0 };

	while (nanosleep(&req, &rem) == -1 && __errno == EINTR) {
		req = rem;
	}
	return 0;
}

unsigned int alarm(unsigned int seconds)
{
	(void)seconds;
	__errno = ENOSYS;
	return 0;
}

int pause(void)
{
	/* Simplified: sleep forever. Real implementation would wait for a signal. */
	struct timespec req = { .tv_sec = 0x7FFFFFFF, .tv_nsec = 0 };
	nanosleep(&req, NULL);
	return -1;  /* Never reached unless interrupted */
}

/* -------------------------------- sysconf ---------------------------------- */

/*
 * _SC_PAGESIZE and the cpu count are answers, not queries: there is no
 * scheduler interface on this target to ask, so _SC_NPROCESSORS_ONLN reports
 * what the kernel was built for rather than what the machine has. Naming the
 * selectors instead of repeating their numbers matters because the header's
 * values are the POSIX ones, not an internal enumeration, and a program that
 * passes _SC_NPROCESSORS_CONF must not silently get _SC_NPROCESSORS_ONLN.
 */
long sysconf(int name)
{
	switch (name) {
	case _SC_PAGESIZE:
		return (long)getpagesize();
	case _SC_NPROCESSORS_ONLN:
	case _SC_NPROCESSORS_CONF:
		return 8;
	case _SC_PID_MAX:
		return 32768;
	case _SC_USER_PROCESSES:
		return 64;
	default:
		__errno = ENOSYS;
		return -1;
	}
}

int getpagesize(void)
{
	/* AT_PAGESZ when the kernel published it, the build's page size
	 * otherwise. It always publishes it, but a program that runs before
	 * the auxv is walked should still get the right answer. */
	long aux_pagesz = getauxval(AT_PAGESZ);

	return aux_pagesz > 0 ? (int)aux_pagesz : 4096;
}

/* ----------------------------- cpu identity -------------------------------- */

int getcpu(unsigned *cpu, unsigned *node)
{
	unsigned pair = 0;
	int ret;

	ret = sys_getcpu(&pair, NULL);
	if (ret < 0)
		return -1;
	if (cpu)
		*cpu = pair;
	if (node)
		*node = 0;   /* one NUMA node is the whole topology here */
	return 0;
}

/* --------------------------- memory mapping -------------------------------- */

void *mmap(void *addr, size_t length, int prot, int flags, int fd,
	   off_t offset)
{
	return sys_mmap(addr, length, prot, flags, fd, offset);
}

int munmap(void *addr, size_t length)
{
	return sys_munmap(addr, length);
}

int mprotect(void *addr, size_t len, int prot)
{
	return sys_mprotect(addr, len, prot);
}

int ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	void *argp;
	int ret;

	va_start(ap, request);
	argp = va_arg(ap, void *);
	va_end(ap);
	ret = sys_ioctl(fd, request, argp);
	return ret;
}

/* -------------------------------- getauxval -------------------------------- */

long getauxval(long type)
{
	if (!__libc_auxv)
		return 0;
	unsigned long *auxv = (unsigned long *)__libc_auxv;
	while (auxv[0] != AT_NULL) {
		if (auxv[0] == (unsigned long)type)
			return (long)auxv[1];
		auxv += 2;
	}
	return 0;
}

/* ------------------------------ user/group --------------------------------- */

uid_t getuid(void)
{
	return (uid_t)sys_getuid();
}

uid_t geteuid(void)
{
	return (uid_t)sys_geteuid();
}

gid_t getgid(void)
{
	return (gid_t)sys_getgid();
}

gid_t getegid(void)
{
	return (gid_t)sys_getegid();
}

/* ---------------------------- filesystem stubs ----------------------------- */

int chdir(const char *path)
{
	(void)path;
	__errno = ENOSYS;
	return -1;
}

int fchdir(int fd)
{
	(void)fd;
	__errno = ENOSYS;
	return -1;
}

char *getcwd(char *buf, size_t size)
{
	(void)buf;
	(void)size;
	__errno = ENOSYS;
	return NULL;
}

int unlink(const char *pathname)
{
	(void)pathname;
	__errno = ENOSYS;
	return -1;
}

int rmdir(const char *pathname)
{
	(void)pathname;
	__errno = ENOSYS;
	return -1;
}

int access(const char *pathname, int mode)
{
	(void)pathname;
	(void)mode;
	__errno = ENOSYS;
	return -1;
}

int truncate(const char *path, off_t length)
{
	(void)path;
	(void)length;
	__errno = ENOSYS;
	return -1;
}

int creat(const char *pathname, mode_t mode)
{
	return open(pathname, O_CREAT | O_WRONLY | O_TRUNC, mode);
}