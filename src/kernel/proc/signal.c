/*
 * signal.c - signal dispositions, the blocked mask and the pending set.
 *
 * A disposition is SIGACT_DFL, SIGACT_IGN, or the address of a user handler.
 * The kernel only stores them here; running a handler or taking the default
 * action happens when the task returns to user mode.
 */
#include <types.h>
#include <kstring.h>
#include <task.h>
#include <sched.h>
#include <process.h>
#include <signals.h>
#include <uapi/errno.h>
#include <uapi/signal.h>

/* The user half of the address space ends here; a handler address at or
 * above it could never be entered from ring 3. */
#define USER_ADDR_LIMIT 0x0000800000000000ULL

#define SIG_UNBLOCKABLE (SIG_BIT(SIGKILL) | SIG_BIT(SIGSTOP))

#define SIG_BIT(sig) (1ULL << ((sig) - 1))

static bool sig_in_range(int sig)
{
    return sig >= 1 && sig < NSIG;
}

long sys_rt_sigaction(int sig, u64 act, u64 oact, u64 sigsetsize)
{
    struct task *t = current_task();
    struct kernel_sigaction ka;

    if (!t)
        return -ESRCH;
    if (sigsetsize != sizeof(sigset_t))
        return -EINVAL;
    if (!sig_in_range(sig))
        return -EINVAL;
    if (act && (SIG_BIT(sig) & SIG_UNBLOCKABLE))
        return -EINVAL;

    if (act) {
        if (copy_from_user(&ka, act, sizeof(ka)) < 0)
            return -EFAULT;
        if (ka.sa_handler > SIGACT_IGN && ka.sa_handler >= USER_ADDR_LIMIT)
            return -EINVAL;
    }
    if (oact && copy_to_user(oact, &t->sig_actions[sig], sizeof(ka)) < 0)
        return -EFAULT;
    if (act)
        t->sig_actions[sig] = ka;
    return 0;
}

long sys_rt_sigprocmask(int how, u64 set, u64 oset, u64 sigsetsize)
{
    struct task *t = current_task();
    u64 new_set = 0;
    u64 blocked;

    if (!t)
        return -ESRCH;
    if (sigsetsize != sizeof(sigset_t))
        return -EINVAL;
    if (set) {
        if (copy_from_user(&new_set, set, sizeof(new_set)) < 0)
            return -EFAULT;
        if (how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK)
            return -EINVAL;
    }
    if (oset && copy_to_user(oset, &t->sig_blocked, sizeof(u64)) < 0)
        return -EFAULT;
    if (!set)
        return 0;

    blocked = t->sig_blocked;
    if (how == SIG_BLOCK)
        blocked |= new_set;
    else if (how == SIG_UNBLOCK)
        blocked &= ~new_set;
    else
        blocked = new_set;
    t->sig_blocked = blocked & ~SIG_UNBLOCKABLE;
    return 0;
}

long sys_rt_sigpending(u64 set, u64 sigsetsize)
{
    struct task *t = current_task();
    u64 pending;

    if (!t)
        return -ESRCH;
    if (sigsetsize != sizeof(sigset_t))
        return -EINVAL;
    pending = __atomic_load_n(&t->sig_pending, __ATOMIC_SEQ_CST);
    if (copy_to_user(set, &pending, sizeof(pending)) < 0)
        return -EFAULT;
    return 0;
}

void signal_exec_reset(struct task *t)
{
    for (int sig = 1; sig < NSIG; sig++) {
        if (t->sig_actions[sig].sa_handler <= SIGACT_IGN)
            continue;
        memset(&t->sig_actions[sig], 0, sizeof(t->sig_actions[sig]));
    }
}
