/*
 * uapi/signal.h — signal numbers, dispositions and the mask ABI.
 *
 * Numbers are the Linux x86_64 ones, so the standard names mean what programs
 * expect. 1..31 are the classic signals; 32..64 are accepted as real-time
 * numbers.
 *
 * struct kernel_sigaction is four machine words. libc's struct sigaction has
 * the same layout with a typed handler, and sigaction() converts between the
 * two, so the kernel never sees a C function type.
 */
#ifndef UAPI_SIGNAL_H
#define UAPI_SIGNAL_H

#include <stdint.h>

#define NSIG 65

#define SIGHUP      1
#define SIGINT      2
#define SIGQUIT     3
#define SIGILL      4
#define SIGTRAP     5
#define SIGABRT     6
#define SIGBUS      7
#define SIGFPE      8
#define SIGKILL     9
#define SIGUSR1    10
#define SIGSEGV    11
#define SIGUSR2    12
#define SIGPIPE    13
#define SIGALRM    14
#define SIGTERM    15
#define SIGSTKFLT  16
#define SIGCHLD    17
#define SIGCONT    18
#define SIGSTOP    19
#define SIGTSTP    20
#define SIGTTIN    21
#define SIGTTOU    22
#define SIGURG     23
#define SIGXCPU    24
#define SIGXFSZ    25
#define SIGVTALRM  26
#define SIGPROF    27
#define SIGWINCH   28
#define SIGIO      29
#define SIGPWR     30
#define SIGSYS     31

/* Dispositions that are not a function address. */
#define SIGACT_DFL 0
#define SIGACT_IGN 1

/* sa_flags. */
#define SA_NOCLDSTOP  0x00000001u
#define SA_NOCLDWAIT  0x00000002u
#define SA_SIGINFO    0x00000004u
#define SA_RESTORER   0x04000000u
#define SA_ONSTACK    0x08000000u
#define SA_RESTART    0x10000000u
#define SA_NODEFER    0x40000000u
#define SA_RESETHAND  0x80000000u

/* how, for rt_sigprocmask. */
#define SIG_BLOCK    0
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2

typedef uint64_t sigset_t;

struct kernel_sigaction {
    uint64_t sa_handler;
    uint64_t sa_flags;
    uint64_t sa_restorer;
    uint64_t sa_mask;
};

#endif /* UAPI_SIGNAL_H */
