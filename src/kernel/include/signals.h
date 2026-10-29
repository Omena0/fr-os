/*
 * signals.h - signal dispositions, the blocked mask and the pending set.
 */
#ifndef SIGNALS_H
#define SIGNALS_H

#include <types.h>

struct task;
struct syscall_regs;
struct interrupt_frame;

long sys_rt_sigaction(int sig, u64 act, u64 oact, u64 sigsetsize);
long sys_rt_sigprocmask(int how, u64 set, u64 oset, u64 sigsetsize);
long sys_rt_sigpending(u64 set, u64 sigsetsize);
long sys_rt_sigreturn(struct syscall_regs *regs);
long sys_kill(s64 pid, s64 sig);

/* Mark a signal pending on a task. Delivery happens on the way back to user
 * mode, never here, so this is safe from interrupt context. */
void signal_send(struct task *t, int sig);

/* Called on the way back to user mode, after the syscall has produced its
 * return value in regs->rax and before the return path reads the registers. */
void signal_deliver_syscall(struct syscall_regs *regs);
void signal_deliver_interrupt(struct interrupt_frame *frame);

/* Called from the interrupt exit stub, after interrupt_dispatch and before the
 * frame is restored, to apply a redirect that delivery recorded. */
void signal_apply_resume(struct interrupt_frame *frame);

/* execve: caught signals return to SIG_DFL, ignored ones stay ignored. The
 * blocked and pending sets survive the exec, as they do in POSIX. */
void signal_exec_reset(struct task *t);

#endif /* SIGNALS_H */
