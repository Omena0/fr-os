/*
 * signal.c - sigaction, the signal mask, and the set helpers.
 */
#include <signal.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

extern int sys_rt_sigaction(int sig, const struct kernel_sigaction *act, struct kernel_sigaction *oact);
extern int sys_rt_sigprocmask(int how, const sigset_t *set, sigset_t *oset);
extern int sys_rt_sigpending(sigset_t *set);

static int sig_valid(int sig)
{
    return sig >= 1 && sig < NSIG;
}

static int sig_bad(void)
{
    __errno = EINVAL;
    return -1;
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *oact)
{
    struct kernel_sigaction ka, old;
    int rc;

    if (!sig_valid(sig))
        return sig_bad();
    if (act) {
        ka.sa_handler = (uint64_t)(uintptr_t)act->sa_handler;
        ka.sa_flags = act->sa_flags;
        ka.sa_restorer = (uint64_t)(uintptr_t)act->sa_restorer;
        ka.sa_mask = act->sa_mask;
    }
    rc = sys_rt_sigaction(sig, act ? &ka : NULL, oact ? &old : NULL);
    if (rc < 0)
        return -1;
    if (oact) {
        oact->sa_handler = (sighandler_t)(uintptr_t)old.sa_handler;
        oact->sa_flags = old.sa_flags;
        oact->sa_restorer = (void (*)(void))(uintptr_t)old.sa_restorer;
        oact->sa_mask = old.sa_mask;
    }
    return 0;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oset)
{
    return sys_rt_sigprocmask(how, set, oset);
}

int sigpending(sigset_t *set)
{
    return sys_rt_sigpending(set);
}

int sigemptyset(sigset_t *set)
{
    *set = 0;
    return 0;
}

int sigfillset(sigset_t *set)
{
    *set = ~(sigset_t)0;
    return 0;
}

int sigaddset(sigset_t *set, int sig)
{
    if (!sig_valid(sig))
        return sig_bad();
    *set |= (sigset_t)1 << (sig - 1);
    return 0;
}

int sigdelset(sigset_t *set, int sig)
{
    if (!sig_valid(sig))
        return sig_bad();
    *set &= ~((sigset_t)1 << (sig - 1));
    return 0;
}

int sigismember(const sigset_t *set, int sig)
{
    if (!sig_valid(sig))
        return sig_bad();
    return (int)((*set >> (sig - 1)) & 1);
}

sighandler_t signal(int sig, sighandler_t handler)
{
    struct sigaction act, old;

    act.sa_handler = handler;
    act.sa_flags = 0;
    act.sa_restorer = NULL;
    sigemptyset(&act.sa_mask);
    if (sigaction(sig, &act, &old) < 0)
        return SIG_ERR;
    return old.sa_handler;
}
