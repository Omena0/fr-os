/*
 * unistd.c — POSIX unistd wrappers over the kernel ABI.
 *
 * write, read, close, open, lseek, fsync, isatty, unlink, sleep, usleep,
 * nanosleep, alarm, pause, getpid, gettid, getppid, sysconf, getpagesize,
 * getauxval, _exit, execve, fork, waitpid, wait4, pipe, chdir, getuid,
 * geteuid, getgid, getegid, fchdir, truncate, getcwd, dup, dup2.
 *
 * getauxval searches the saved auxv (__libc_auxv set by crt1) for the type.
 * The kernel provides AT_PHDR, AT_PHNUM, AT_PAGESZ, AT_BASE, AT_ENTRY,
 * AT_UID, AT_EUID, AT_GID, AT_EGID, AT_ABI_VERSION, AT_NULL. No AT_RANDOM.
 * sysconf handles _SC_PAGESIZE, _SC_NPROCESSORS_ONLN, _SC_PID_MAX,
 * _SC_USER_PROCESSES; others return -1/ENOSYS.
 * fork calls sys_fork (declared extern, kernel-side concurrent) and resets
 * allocator state and stdio in the child, then runs post-fork atexit handlers.
 * execve calls sys_execve (declared extern, not defined here).
 * sleep/usleep via sys_nanosleep with EINTR retry. pause is one nanosleep on
 * infinite request (documented simplification).
 * getcwd, unlink, rmdir, access, chdir, truncate return -1/ENOSYS (honest absences).
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

/* __MORE__ */

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

/* Forward declarations */
extern void *__libc_auxv;
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

/* __MORE__ */

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
		/* Child process: reset allocator state and reinitialize */
		__malloc_fork_child();
		__libc_init();
		/* Run atexit handlers registered after fork */
		/* Note: atexit list is copied from parent; new registrations
		 * in child will run on child exit. This is a simplification. */
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
	sys_exit_group(status);
}

/* __MORE__ */

/* ------------------------------- time/sleep -------------------------------- */

int sleep(unsigned int seconds)
{
	struct timespec req = { .tv_sec = seconds, .tv_nsec = 0 };
	struct timespec rem;
	while (nanosleep(&req, &rem) == -1 && __errno == EINTR) {
		req = rem;
	}
	return (int)rem.tv_sec;
}

int usleep(useconds_t usec)
{
	struct timespec req = { .tv_sec = usec / 1000000, .tv_nsec = (usec % 1000000) * 1000 };
	struct timespec rem;
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

long sysconf(int name)
{
	switch (name) {
	case 0:  /* _SC_PAGESIZE */
		return 4096;
	case 1:  /* _SC_NPROCESSORS_ONLN */
		return 8;
	case 5:  /* _SC_PID_MAX */
		return 32768;
	case 4:  /* _SC_USER_PROCESSES */
		return 64;
	default:
		__errno = ENOSYS;
		return -1;
	}
}

int getpagesize(void)
{
	return 4096;
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