/*
 * syscall.c — libc's syscall wrappers.
 *
 * Every wrapper here is a thin shim over the assembly trampolines in
 * syscall.S. The shim's only job is to translate the kernel's negative-errno
 * convention into the libc convention: a negative return becomes a positive
 * errno stored in the thread-local __errno, and the wrapper returns -1.
 *
 * Functions that return a value (read, write, mmap) return it directly on
 * success; functions that return a fd (open) or a status likewise. brk and
 * munmap return the kernel's value verbatim because the caller must
 * distinguish "new break" from "old break on failure" and "0" from "error".
 */
#include <errno.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

extern long __syscall0(long number);
extern long __syscall1(long number, long a1);
extern long __syscall2(long number, long a1, long a2);
extern long __syscall3(long number, long a1, long a2, long a3);
extern long __syscall4(long number, long a1, long a2, long a3, long a4);
extern long __syscall5(long number, long a1, long a2, long a3, long a4, long a5);
extern long __syscall6(long number, long a1, long a2, long a3, long a4, long a5,
		       long a6);

/* Convert a kernel return into a libc return. */
static long check(long ret)
{
	if (ret < 0) {
		__errno = (int)(-ret);
		return -1;
	}
	return ret;
}

/* Like check but for value-returning syscalls where 0 is a valid result. */
static long check0(long ret)
{
	if (ret < 0) {
		__errno = (int)(-ret);
		return -1;
	}
	return ret;
}

int sys_write(int fd, const void *buf, size_t count)
{
	return (int)check0(__syscall3(SYS_write, fd, (long)buf, (long)count));
}

int sys_read(int fd, void *buf, size_t count)
{
	return (int)check0(__syscall3(SYS_read, fd, (long)buf, (long)count));
}

int sys_open(const char *path, int flags, mode_t mode)
{
	return (int)check0(__syscall3(SYS_open, (long)path, flags, (long)mode));
}

int sys_close(int fd)
{
	return (int)check0(__syscall1(SYS_close, fd));
}

off_t sys_lseek(int fd, off_t offset, int whence)
{
	return (off_t)check0(__syscall3(SYS_lseek, fd, offset, whence));
}

int sys_fstat(int fd, struct kstat *st)
{
	return (int)check0(__syscall2(SYS_fstat, fd, (long)st));
}

int sys_ioctl(int fd, unsigned long request, ...)
{
	void *argp;
	va_list ap;

	/* The kernel only implements TIOCGWINSZ, which takes a pointer to a
	 * struct winsize. Pass that pointer through verbatim; callers are
	 * responsible for pointing it at a valid struct. */
	va_start(ap, request);
	argp = va_arg(ap, void *);
	va_end(ap);
	return (int)check0(__syscall3(SYS_ioctl, fd, (long)request,
				       (long)argp));
}

long sys_brk(void *addr)
{
	/* brk(0) returns the current break; brk(addr) returns the new break or
	 * the old one on failure. The return is not errno-encoded. */
	return __syscall1(SYS_brk, (long)addr);
}

void *sys_mmap(void *addr, size_t length, int prot, int flags, int fd,
	       off_t offset)
{
	long ret = __syscall6(SYS_mmap, (long)addr, (long)length, prot, flags, fd,
			      offset);
	if (ret < 0) {
		__errno = (int)(-ret);
		return MAP_FAILED;
	}
	return (void *)ret;
}

int sys_munmap(void *addr, size_t length)
{
	return (int)check0(__syscall2(SYS_munmap, (long)addr, (long)length));
}

int sys_mprotect(void *addr, size_t len, int prot)
{
	return (int)check0(__syscall3(SYS_mprotect, (long)addr, (long)len, prot));
}

pid_t sys_getpid(void)
{
	return (pid_t)__syscall0(SYS_getpid);
}

pid_t sys_gettid(void)
{
	return (pid_t)__syscall0(SYS_gettid);
}

pid_t sys_getppid(void)
{
	return (pid_t)__syscall0(SYS_getppid);
}

int sys_sched_yield(void)
{
	return (int)check0(__syscall0(SYS_sched_yield));
}

int sys_getcpu(unsigned *cpu, void *cache)
{
	return (int)check0(__syscall2(SYS_getcpu, (long)cpu, (long)cache));
}

int sys_clock_gettime(clockid_t clk_id, struct timespec *tp)
{
	return (int)check0(__syscall2(SYS_clock_gettime, clk_id, (long)tp));
}

int sys_nanosleep(const struct timespec *req, struct timespec *rem)
{
	return (int)check0(__syscall2(SYS_nanosleep, (long)req, (long)rem));
}

int sys_pipe(int pipefd[2])
{
	return (int)check0(__syscall1(SYS_pipe, (long)pipefd));
}

void sys_exit(int status)
{
	__syscall1(SYS_exit, status);
	__builtin_unreachable();
}

void sys_exit_group(int status)
{
	__syscall1(SYS_exit_group, status);
	__builtin_unreachable();
}

int sys_dup(int oldfd)
{
	return (int)check0(__syscall1(SYS_dup, oldfd));
}

int sys_dup2(int oldfd, int newfd)
{
	/*
	 * There is no SYS_dup2 number in the ABI: the file domain stops at
	 * SYS_fcntl, and the kernel's SYS_dup handler has no newfd operand to
	 * read. dup2 is therefore layered on dup and close here rather than
	 * asking the kernel for a syscall that would only ever answer -ENOSYS.
	 */
	int ret = (int)check0(__syscall1(SYS_dup, oldfd));

	if (ret < 0)
		return ret;
	if (ret == newfd)
		return ret;
	if (sys_close(newfd) < 0 && __errno != EBADF)
		return -1;
	return ret;
}

int sys_execve(const char *path, char *const argv[], char *const envp[])
{
	/* execve only returns on failure; on success the process is replaced. */
	int ret = (int)check0(__syscall3(SYS_execve, (long)path, (long)argv,
					 (long)envp));
	__builtin_unreachable();
	return ret;
}

pid_t sys_fork(void)
{
	return (pid_t)check0(__syscall0(SYS_fork));
}

pid_t sys_wait4(pid_t pid, int *wstatus, int options, void *rusage)
{
	return (pid_t)check0(__syscall4(SYS_wait4, pid, (long)wstatus, options,
					(long)rusage));
}

uid_t sys_getuid(void)
{
	return (uid_t)__syscall0(SYS_getuid);
}

gid_t sys_getgid(void)
{
	return (gid_t)__syscall0(SYS_getgid);
}

/*
 * There is no SYS_geteuid or SYS_getegid, and there is no way to add one
 * without editing the ABI header, which this file does not own. Inventing a
 * number here would be worse than the duplication: the kernel's dispatch table
 * has no entry for it, so the call would return -ENOSYS and every program that
 * checks privileges through the effective ids would fail closed. The kernel's
 * struct cred holds a single uid and a single gid with no saved-set and no
 * privilege-drop path, so the effective id *is* the real id and SYS_getuid and
 * SYS_getgid already return the right answer.
 */
uid_t sys_geteuid(void)
{
	return sys_getuid();
}

gid_t sys_getegid(void)
{
	return sys_getgid();
}