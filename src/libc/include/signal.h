/*
 * signal.h - signal dispositions and the signal mask.
 *
 * SIG_DFL and SIG_IGN are typed here as function pointers, as C programs
 * expect. The kernel stores them as the plain values SIGACT_DFL and SIGACT_IGN,
 * and sigaction() converts.
 */
#ifndef SIGNAL_H
#define SIGNAL_H

#include <uapi/signal.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

struct sigaction {
    sighandler_t sa_handler;
    unsigned long sa_flags;
    void (*sa_restorer)(void);
    sigset_t sa_mask;
};

int sigaction(int sig, const struct sigaction *act, struct sigaction *oact);
int sigprocmask(int how, const sigset_t *set, sigset_t *oset);
int sigpending(sigset_t *set);
int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int sig);
int sigdelset(sigset_t *set, int sig);
int sigismember(const sigset_t *set, int sig);
sighandler_t signal(int sig, sighandler_t handler);

#ifdef __cplusplus
}
#endif

#endif /* SIGNAL_H */
