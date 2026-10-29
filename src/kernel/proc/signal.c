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
#include <interrupt.h>
#include <signals.h>
#include <syscall.h>
#include <uapi/errno.h>
#include <uapi/signal.h>

/* The user half of the address space ends here; a handler address at or
 * above it could never be entered from ring 3. */
#define USER_ADDR_LIMIT 0x0000800000000000ULL

#define SIG_BIT(sig) (1ULL << ((sig) - 1))
#define SIG_UNBLOCKABLE (SIG_BIT(SIGKILL) | SIG_BIT(SIGSTOP))

/* Signals raised by the instruction that was running. They cannot be left
 * pending while blocked: the instruction would simply fault again. */
#define SIG_FAULT_MASK (SIG_BIT(SIGSEGV) | SIG_BIT(SIGBUS) | SIG_BIT(SIGFPE) | \
                        SIG_BIT(SIGILL) | SIG_BIT(SIGTRAP))

/* Below the user stack pointer, a leaf function may keep data in the 128 bytes
 * the SysV ABI calls the red zone; the handler frame has to go below that. */
#define SIG_RED_ZONE    128

/* The frame a handler finds on the user stack, at the address rt_sigreturn is
 * entered with. Field order is fixed: it is read back by sys_rt_sigreturn(). */
struct sig_saved {
    u64 rax, rdi, rsi, rdx, r10, r8, r9, rcx, r11, rip, rflags, rsp, blocked;
};
#define SIG_FRAME_BYTES 112     /* >= sizeof(struct sig_saved), 16-aligned */

/* The RFLAGS bits a user program may choose: CF PF AF ZF SF TF DF OF. IF is
 * forced on, and IOPL, NT, AC, ID and the rest stay as the kernel set them. */
#define RFLAGS_USER_MASK 0xDD5ULL
#define RFLAGS_FIXED     0x202ULL
#define RFLAGS_DF        0x400ULL

/* The registers a signal frame preserves, as pointers into whichever frame the
 * return path owns: struct syscall_regs or struct interrupt_frame. Both have all
 * twelve. */
struct sig_regs {
    u64 *rax, *rdi, *rsi, *rdx, *r10, *r8, *r9, *rcx, *r11, *rip, *rflags, *rsp;
};

static bool sig_in_range(int sig)
{
    return sig >= 1 && sig < NSIG;
}

static bool sig_is_fault(int sig)
{
    return (SIG_BIT(sig) & SIG_FAULT_MASK) != 0;
}

/* Default action is ignore for these. There is no job control yet, so the
 * stop signals are ignored too, rather than stopping a process nothing can
 * continue. */
static bool sig_default_ignored(int sig)
{
    switch (sig) {
    case SIGCHLD:
    case SIGCONT:
    case SIGURG:
    case SIGWINCH:
    case SIGSTOP:
    case SIGTSTP:
    case SIGTTIN:
    case SIGTTOU:
        return true;
    default:
        return false;
    }
}

static int sig_next(const struct task *t)
{
    u64 pending = __atomic_load_n(&t->sig_pending, __ATOMIC_SEQ_CST);
    u64 deliverable = pending & (~t->sig_mask | SIG_FAULT_MASK);

    if (!deliverable)
        return 0;
    return __builtin_ctzll(deliverable) + 1;
}

static __noreturn void sig_terminate(struct task *t, int sig)
{
    t->term_signal = sig;
    task_exit_current(0);
}

void signal_send(struct task *t, int sig)
{
    if (!t || !sig_in_range(sig))
        return;
    __atomic_fetch_or(&t->sig_pending, SIG_BIT(sig), __ATOMIC_SEQ_CST);
}

void process_deliver_signal(int signo, struct interrupt_frame *frame)
{
    (void)frame;
    signal_send(current_task(), signo);
}

static void sig_setup_frame(struct task *t, int sig, struct kernel_sigaction *ka,
                struct sig_regs *r)
{
    struct sig_saved s;
    u64 handler = ka->sa_handler;
    u64 restorer = ka->sa_restorer;
    u64 top, frame, entry_sp;

    if (!(ka->sa_flags & SA_RESTORER) || restorer == 0)
        sig_terminate(t, SIGSEGV);
    if (*r->rsp < SIG_RED_ZONE + SIG_FRAME_BYTES + 8)
        sig_terminate(t, SIGSEGV);

    s.rax = *r->rax;
    s.rdi = *r->rdi;
    s.rsi = *r->rsi;
    s.rdx = *r->rdx;
    s.r10 = *r->r10;
    s.r8 = *r->r8;
    s.r9 = *r->r9;
    s.rcx = *r->rcx;
    s.r11 = *r->r11;
    s.rip = *r->rip;
    s.rflags = *r->rflags;
    s.rsp = *r->rsp;
    s.blocked = t->sig_mask;

    /* The handler is entered as if called: its return address (the restorer)
     * sits at entry_sp, and entry_sp + 8 is the saved context. */
    top = (*r->rsp - SIG_RED_ZONE) & ~0xFULL;
    frame = top - SIG_FRAME_BYTES;
    entry_sp = frame - 8;

    if (copy_to_user(frame, &s, sizeof(s)) < 0)
        sig_terminate(t, SIGSEGV);
    if (copy_to_user(entry_sp, &restorer, sizeof(restorer)) < 0)
        sig_terminate(t, SIGSEGV);

    t->sig_mask |= ka->sa_mask;
    if (!(ka->sa_flags & SA_NODEFER))
        t->sig_mask |= SIG_BIT(sig);
    t->sig_mask &= ~SIG_UNBLOCKABLE;

    if (ka->sa_flags & SA_RESETHAND)
        memset(ka, 0, sizeof(*ka));

    *r->rsp = entry_sp;
    *r->rip = handler;
    *r->rdi = (u64)sig;
    *r->rsi = 0;
    *r->rdx = 0;
    *r->rcx = handler;
    *r->rflags &= ~RFLAGS_DF;
}

static bool signal_deliver(struct sig_regs *r)
{
    struct task *t = current_task();
    int sig;

    if (!t)
        return false;
    for (;;) {
        struct kernel_sigaction *ka;

        sig = sig_next(t);
        if (!sig)
            return false;
        __atomic_fetch_and(&t->sig_pending, ~SIG_BIT(sig), __ATOMIC_SEQ_CST);
        ka = &t->sig_actions[sig];

        if (ka->sa_handler == SIGACT_IGN && !sig_is_fault(sig))
            continue;
        if (ka->sa_handler == SIGACT_DFL || ka->sa_handler == SIGACT_IGN) {
            if (!sig_is_fault(sig) && sig_default_ignored(sig))
                continue;
            sig_terminate(t, sig);
        }
        sig_setup_frame(t, sig, ka, r);
        return true;
    }
}

void signal_deliver_syscall(struct syscall_regs *regs)
{
    struct sig_regs r = {
        .rax = &regs->rax, .rdi = &regs->rdi, .rsi = &regs->rsi,
        .rdx = &regs->rdx, .r10 = &regs->r10, .r8 = &regs->r8,
        .r9 = &regs->r9, .rcx = &regs->rcx, .r11 = &regs->r11,
        .rip = &regs->rip, .rflags = &regs->rflags, .rsp = &regs->rsp,
    };

    signal_deliver(&r);
}

void signal_deliver_interrupt(struct interrupt_frame *frame)
{
    struct task *t = current_task();
    struct sig_resume *rec;
    struct sig_regs r;

    if (!t || !sig_next(t))
        return;

    rec = &t->sig_resume;
    rec->rax = frame->rax;
    rec->rdi = frame->rdi;
    rec->rsi = frame->rsi;
    rec->rdx = frame->rdx;
    rec->rcx = frame->rcx;
    rec->r8 = frame->r8;
    rec->r9 = frame->r9;
    rec->r10 = frame->r10;
    rec->r11 = frame->r11;
    rec->rip = frame->rip;
    rec->rflags = frame->rflags;
    rec->rsp = frame->rsp;

    r.rax = &rec->rax;
    r.rdi = &rec->rdi;
    r.rsi = &rec->rsi;
    r.rdx = &rec->rdx;
    r.r10 = &rec->r10;
    r.r8 = &rec->r8;
    r.r9 = &rec->r9;
    r.rcx = &rec->rcx;
    r.r11 = &rec->r11;
    r.rip = &rec->rip;
    r.rflags = &rec->rflags;
    r.rsp = &rec->rsp;

    rec->valid = signal_deliver(&r);
}

void signal_apply_resume(struct interrupt_frame *frame)
{
    struct task *t = current_task();
    struct sig_resume *rec;

    if (!t || !t->sig_resume.valid)
        return;
    rec = &t->sig_resume;
    rec->valid = false;
    frame->rax = rec->rax;
    frame->rdi = rec->rdi;
    frame->rsi = rec->rsi;
    frame->rdx = rec->rdx;
    frame->rcx = rec->rcx;
    frame->r8 = rec->r8;
    frame->r9 = rec->r9;
    frame->r10 = rec->r10;
    frame->r11 = rec->r11;
    frame->rip = rec->rip;
    frame->rflags = rec->rflags;
    frame->rsp = rec->rsp;
}

long sys_rt_sigreturn(struct syscall_regs *regs)
{
    struct task *t = current_task();
    struct sig_saved s;

    if (!t)
        return -ESRCH;
    if (copy_from_user(&s, regs->rsp, sizeof(s)) < 0)
        sig_terminate(t, SIGSEGV);
    if (s.rip >= USER_ADDR_LIMIT || s.rsp >= USER_ADDR_LIMIT)
        sig_terminate(t, SIGSEGV);

    regs->rdi = s.rdi;
    regs->rsi = s.rsi;
    regs->rdx = s.rdx;
    regs->r10 = s.r10;
    regs->r8 = s.r8;
    regs->r9 = s.r9;
    regs->rip = s.rip;
    regs->rcx = s.rip;
    regs->rflags = (s.rflags & RFLAGS_USER_MASK) | RFLAGS_FIXED;
    regs->r11 = regs->rflags;
    regs->rsp = s.rsp;
    t->sig_mask = s.blocked & ~SIG_UNBLOCKABLE;
    return (long)s.rax;
}

static bool sig_may_send(const struct task *from, const struct task *to)
{
    return from->cred.uid == 0 || from->cred.uid == to->cred.uid;
}

/*
 * kill(2). pid > 0 names one process; pid == 0 the caller's process group;
 * pid < -1 the group -pid; pid == -1 every process except the caller and init.
 * Signal 0 checks that the target exists and may be signalled, and sends
 * nothing.
 */
long sys_kill(s64 pid, s64 sig)
{
    struct task *me = current_task();
    struct list_head *pos;
    u64 flags;
    long matched = 0;
    long err = -ESRCH;

    if (!me)
        return -ESRCH;
    if (sig < 0 || sig >= NSIG)
        return -EINVAL;

    if (pid > 0) {
        struct task *t = task_find_by_pid((u32)pid);

        if (!t)
            return -ESRCH;
        if (!sig_may_send(me, t))
            return -EPERM;
        if (sig)
            signal_send(t, (int)sig);
        return 0;
    }

    flags = spinlock_irqsave(&task_all_lock);
    list_for_each(pos, &task_all_list) {
        struct task *t = list_entry(pos, struct task, all_node);
        bool hit;

        if (t->state == TASK_ZOMBIE || t->state == TASK_DEAD || t->reaped)
            continue;
        if (pid == -1)
            hit = t->pid != 1 && t != me;
        else
            hit = t->pgid == (pid == 0 ? me->pgid : (u32)-pid);
        if (!hit)
            continue;
        if (!sig_may_send(me, t)) {
            err = -EPERM;
            continue;
        }
        matched++;
        if (sig)
            signal_send(t, (int)sig);
    }
    spinlock_unlock_irqrestore(&task_all_lock, flags);

    return matched ? 0 : err;
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
    if (oset && copy_to_user(oset, &t->sig_mask, sizeof(u64)) < 0)
        return -EFAULT;
    if (!set)
        return 0;

    blocked = t->sig_mask;
    if (how == SIG_BLOCK)
        blocked |= new_set;
    else if (how == SIG_UNBLOCK)
        blocked &= ~new_set;
    else
        blocked = new_set;
    t->sig_mask = blocked & ~SIG_UNBLOCKABLE;
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
