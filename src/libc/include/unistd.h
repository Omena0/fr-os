/*
 * unistd.h — POSIX unistd.
 *
 * This header pulls in the kernel ABI for the system calls libc actually
 * makes on behalf of the program, plus the standard typedefs and the small
 * set of wrappers libc provides.
 */
#ifndef UNISTD_H
#define UNISTD_H

#include <stddef.h>
#include <sys/types.h>
#include <uapi/syscall.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#ifndef R_OK
#define R_OK 4
#define W_OK 2
#define X_OK 1
#define F_OK 0
#endif

extern int    write(int fd, const void *buf, size_t count);
extern int    read(int fd, void *buf, size_t count);
extern int    close(int fd);
extern int    open(const char *pathname, int flags, ...);
extern int    creat(const char *pathname, mode_t mode);
extern off_t  lseek(int fd, off_t offset, int whence);
extern int    fsync(int fd);
extern int    fdatasync(int fd);
extern int    ftruncate(int fd, off_t length);
extern int    isatty(int fd);
extern int    unlink(const char *pathname);
extern int    rmdir(const char *pathname);
extern int    access(const char *pathname, int mode);
extern int    sleep(unsigned int seconds);
extern int    usleep(useconds_t usec);
extern int    nanosleep(const struct timespec *req, struct timespec *rem);
extern unsigned int alarm(unsigned int seconds);
extern int    pause(void);

extern long   getauxval(long type);
extern int    getpagesize(void);
extern pid_t  getpid(void);
extern pid_t  getppid(void);
extern pid_t  gettid(void);
extern uid_t  getuid(void);
extern uid_t  geteuid(void);
extern gid_t  getgid(void);
extern gid_t  getegid(void);

extern int    dup(int oldfd);
extern int    dup2(int oldfd, int newfd);
extern int    pipe(int pipefd[2]);

extern pid_t  fork(void);
extern int    execve(const char *path, char *const argv[], char *const envp[]);
extern pid_t  waitpid(pid_t pid, int *wstatus, int options);
extern pid_t  wait4(pid_t pid, int *wstatus, int options, void *rusage);

extern void    _exit(int status) __attribute__((noreturn));

extern int    chdir(const char *path);
extern int    fchdir(int fd);
extern char   *getcwd(char *buf, size_t size);
extern int    truncate(const char *path, off_t length);

extern long   sysconf(int name);

#define _SC_PAGESIZE            0
#define _SC_NPROCESSORS_ONLN    1
#define _SC_NPROCESSORS_CONF    2
#define _SC_PHYS_PAGES          3
#define _SC_USER_PROCESSES      4
#define _SC_PID_MAX             5
#define _SC_OPEN_MAX            6
#define _SC_CLK_TCK             7
#define _SC_HOST_NAME_MAX       8
#define _SC_LOGIN_NAME_MAX      9
#define _SC_ARG_MAX             10

extern int getcpu(unsigned *cpu, unsigned *node);

#ifdef __cplusplus
}
#endif

#endif /* UNISTD_H */