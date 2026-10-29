/*
 * signals.h - signal dispositions, the blocked mask and the pending set.
 */
#ifndef SIGNALS_H
#define SIGNALS_H

#include <types.h>

struct task;

long sys_rt_sigaction(int sig, u64 act, u64 oact, u64 sigsetsize);
long sys_rt_sigprocmask(int how, u64 set, u64 oset, u64 sigsetsize);
long sys_rt_sigpending(u64 set, u64 sigsetsize);

/* execve: caught signals return to SIG_DFL, ignored ones stay ignored. The
 * blocked and pending sets survive the exec, as they do in POSIX. */
void signal_exec_reset(struct task *t);

#endif /* SIGNALS_H */
