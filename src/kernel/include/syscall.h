/*
 * syscall.h — the ring-3 entry path and the dispatch table.
 *
 * Path
 * ----
 *   user:  syscall                 -> LSTAR
 *   entry:  syscall_entry.S        -> syscall_dispatch
 *   disp:  syscall_dispatch        -> table[rax]
 *   exit:  syscall_entry.S        -> sysretq
 *
 * SYSCALL does not switch stacks, so the entry code cannot use whatever RSP
 * points at. It loads a per-CPU kernel stack instead, taken from
 * syscall_cpus[this_cpu_id()].kstack_top. That field is maintained by the
 * scheduler: every context switch points it at the incoming task's kernel stack
 * top, so a task preempted in the middle of a syscall finds its own partial
 * frame intact when it is switched back in, and a task that never entered a
 * syscall has a stack nothing is using.
 *
 * The alternative — reading the TSS RSP0 slot — would work too, but it couples
 * the syscall path to a structure the interrupt agent owns, and the value would
 * have to be duplicated here anyway to avoid a memory read of a descriptor that
 * is not in cache.
 *
 * The frame the entry builds is exactly the struct passed to the dispatcher,
 * so a handler sees the arguments as they were at the syscall instruction
 * rather than as they happen to be after the entry code has used them.
 */
#ifndef SYSCALL_H
#define SYSCALL_H

#include <types.h>
#include <stdbool.h>
#include <io.h>
#include <percpu.h>
#include <process.h>
#include <uapi/syscall.h>
#include <uapi/errno.h>

/*
 * The register state at a syscall boundary. Only the registers the ABI passes
 * arguments in, plus the two the instruction itself clobbered, are modelled:
 * everything else either has to be preserved by the callee or is dead by the
 * time the return value is in RAX.
 */
struct syscall_regs {
    u64 rax;     /* syscall number in, return value out */
    u64 rdi;
    u64 rsi;
    u64 rdx;
    u64 r10;
    u64 r8;
    u64 r9;
    u64 rcx;     /* clobbered by SYSCALL: user RIP */
    u64 r11;     /* clobbered by SYSCALL: user RFLAGS */
    u64 rip;     /* user RIP, copied out of rcx */
    u64 rflags;  /* user RFLAGS, copied out of r11 */
    u64 rsp;     /* user RSP, saved by the entry before the kernel stack */
};

/* ------------------------------------------------------- per-CPU state ------ */

/*
 * The entry path needs two values reachable in a handful of instructions from
 * ring 3, where a call to find them would be both a latency and a nesting
 * hazard. The structure is sized to a cache line and to a power of two so the
 * entry code can index it with a shift.
 *
 * syscall_entry.S addresses this array as `syscall_cpus + cpu_id * 64` and
 * hardcodes the field offsets below; the static assertion in syscall.c is what
 * keeps the two in agreement.
 */
#define SYSCALL_CPU_SIZE   64
#define SC_KSTACK_TOP      0    /* kernel stack top for this CPU's current task */
#define SC_SYSCALL_RSP     8    /* user RSP saved across the entry */
#define SC_RET_TO_USER     16   /* non-zero: this frame may sysretq */

struct syscall_cpu {
    u64 kstack_top;
    u64 user_rsp;
    u64 ret_to_user;
    u64 pad[5];
};

STATIC_ASSERT(sizeof(struct syscall_cpu) == SYSCALL_CPU_SIZE,
          "syscall_entry.S indexes this array with a shift");

extern struct syscall_cpu syscall_cpus[MAX_CPUS];

/* The SYSCALL entry point, in syscall_entry.S. Named here because LSTAR is
 * loaded with its address and the two must not drift. */
extern void syscall_entry(void);

/* Point the entry path at a new kernel stack. Called by the scheduler on every
 * context switch and by syscall_init() for the bootstrap stack. */
void syscall_set_kernel_stack(void *stack_top);

/* ------------------------------------------------------------- dispatch ----- */

/* Install LSTAR/STAR/SFMASK and EFER.SCE. */
void syscall_init(void);

/* Bounds-check `rax`, then invoke the handler. Unknown numbers return -ENOSYS
 * without consulting the table at all: a garbage RAX must never become an
 * indirect call through an attacker-influenced index. */
long syscall_dispatch(struct syscall_regs *regs);

/* The entry point's return path, for a frame that cannot sysretq. Exposed so
 * the assembly can name it and so the panic message can be specific. */
__noreturn void syscall_lost_frame(void);

/*
 * Declare that the current syscall entry frame is abandoned, because the task
 * is exiting and will never return to it. Without this the entry path would
 * sysretq into a user stack frame belonging to a task that no longer exists.
 * Called before the switch away in task_exit_current().
 */
void syscall_abandon_frame(void);

/* ------------------------------------------------------------- helpers ------ */

/* Argument accessors. Each one reads through the saved frame, so a handler
 * that reads the same argument twice sees the same value even though the
 * entry code reused the register. */
static inline u64 arg0(struct syscall_regs *r) { return r->rdi; }
static inline u64 arg1(struct syscall_regs *r) { return r->rsi; }
static inline u64 arg2(struct syscall_regs *r) { return r->rdx; }
static inline u64 arg3(struct syscall_regs *r) { return r->r10; }
static inline u64 arg4(struct syscall_regs *r) { return r->r8; }
static inline u64 arg5(struct syscall_regs *r) { return r->r9; }

/* Set the syscall return value. */
static inline void ret_value(struct syscall_regs *r, u64 v) { r->rax = v; }

/* The address space of the calling task, or NULL for a kernel thread. */
struct address_space *syscall_user_mm(void);

#endif /* SYSCALL_H */
