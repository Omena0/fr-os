/*
 * context_switch_harness.c — host-side test for the kernel's context switch.
 *
 * Why this exists. Finding #57 was "schedule() has no landing pad after
 * context_switch, so a resumed task returns into the middle of schedule()
 * after the CR3 it needs has already been reloaded". Deciding whether that is
 * fixed by reading the comments in sched.c is not a decision procedure — the
 * comments in this tree have been wrong in both directions. This harness is.
 *
 * The kernel context primitives in src/kernel/context.S are pure
 * register-and-stack code: no MMIO, no CR3, no globals, no memory side effects
 * beyond the two stack words the caller names. That makes them compilable and
 * runnable as-is on an ordinary x86-64 host, so the frame contract can be
 * exercised against real switches between real stacks instead of argued about.
 * src/kernel/context.S needs no modification for this; it assembles clean with
 * `gcc -c` for the host.
 *
 * What is actually tested:
 *
 *   A. The frame contract. context_switch(save, new) must publish exactly the
 *      post-push RSP, the frame must be 56 bytes, and control must arrive at
 *      new + 56 (48 for the six saved registers plus the 8-byte return
 *      address that `ret` consumed). An off-by-one anywhere in here shows up
 *      as a resume one word off, thousands of switches later, and looks like
 *      nothing at all.
 *
 *   B. The sentinels. Each fake task gets distinctive values in rbx/rbp and
 *      r12-r15. If the save or restore side is missing, mirrored or swapped, a
 *      task resumes holding another task's registers.
 *
 *   C. The stack. Every resume must land on the resuming task's own stack, at
 *      exactly the offset the frame predicts. A frame published relative to the
 *      wrong stack resumes into whatever happened to be mapped there.
 *
 *   D. The round trip. Three tasks, four rounds each, with the switch count
 *      asserted. A resume path that re-enters the switch — the pre-fix failure,
 *      where the resume address turned out to be a `call` into the
 *      address-space/FPU bookkeeping, so two runnable tasks ping-ponged and
 *      made no forward progress and printed no diagnostic — shows up here as a
 *      switch count far above NTASKS*ROUNDS and as a task resuming more often
 *      than it has rounds.
 *
 *   E. context_restore() on its own, which is how a new task's first frame and
 *      sched_stop_current()'s hand-off work.
 *
 * The scheduler below is shaped like schedule() on purpose: a `reschedule:`
 * label at the top, every derived value recomputed there, the switch as the
 * last statement of the body, and a jump back to the label after it. That is
 * the pattern the resume contract depends on, so exercising the pattern here is
 * the point. Whether the real schedule() actually compiled to that pattern is a
 * separate question, answered mechanically against the artefact by
 * tests/check_schedule_resume.py.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef uint64_t u64;

#define NTASKS 3
#define ROUNDS 4
#define STACK_BYTES (256 * 1024)
#define SENTINEL(n) (0xa5a5a5a500000000ULL + (u64)(n))

/* ------------------------------------------------------------------ kernel -- */

/*
 * `returns_twice` is not decoration. context_switch does not preserve the
 * callee-saved registers of whoever called it: it restores the incoming
 * task's. Without the attribute the compiler is entitled to keep live values
 * in rbx/rbp/r12-r15 across the call and re-read them after the resume, which
 * would read the *previous* task's registers. Same annotation glibc uses for
 * setjmp and swapcontext, for the same reason.
 */
extern void context_switch(u64 *save_rsp, u64 new_rsp)
	__attribute__((returns_twice));
extern void context_restore(u64 rsp) __attribute__((returns_twice));

/*
 * The resume point, in naked assembly so that it can observe the register
 * contract (see tests/resume_stub.S for why a C trampoline cannot).
 */
extern void resume_stub_asm(void);
extern u64 resume_sp, resume_rbx, resume_rbp;
extern u64 resume_r12, resume_r13, resume_r14, resume_r15;

/* ------------------------------------------------------------------- frame -- */

/*
 * Must match src/kernel/context.S exactly: context_switch pushes rbp, rbx,
 * r12, r13, r14, r15 and publishes the resulting RSP, so r15 is at offset 0
 * and the return address `ret` consumes is at offset 48.
 */
struct task_frame {
	u64 r15;
	u64 r14;
	u64 r13;
	u64 r12;
	u64 rbx;
	u64 rbp;
	u64 ret;
};

_Static_assert(sizeof(struct task_frame) == 56, "frame must be 56 bytes");
_Static_assert(offsetof(struct task_frame, r15) == 0, "r15 must be at +0");
_Static_assert(offsetof(struct task_frame, rbx) == 32, "rbx must be at +32");
_Static_assert(offsetof(struct task_frame, rbp) == 40, "rbp must be at +40");
_Static_assert(offsetof(struct task_frame, ret) == 48, "ret addr at +48");

/* The post-push RSP is where a suspended frame begins; `ret` lands 56 above. */
#define RESUME_SP(frame_base) ((frame_base) + 56)

#define NOBJ(n) (n + 2)

struct task {
	u64 *stack;		/* 16-byte aligned, STACK_BYTES long */
	struct task_frame *frame;	/* == task->context_rsp */
	u64 context_rsp;	/* what context_switch published */
	int id;
	int resumptions;
	int rounds_done;
	u64 sp[NOBJ(ROUNDS)];
	u64 rbx[NOBJ(ROUNDS)];
	u64 rbp[NOBJ(ROUNDS)];
	u64 r12[NOBJ(ROUNDS)];
	u64 r13[NOBJ(ROUNDS)];
	u64 r14[NOBJ(ROUNDS)];
	u64 r15[NOBJ(ROUNDS)];
};

static struct task tasks[NTASKS];
static struct task *cur;
static u64 switches;
static int failures;
static int seeded;
static int e_mode;

/* ----------------------------------------------------------------- harness -- */

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			failures++;                                            \
			printf("  FAIL (line %d): ", __LINE__);                 \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
		}                                                              \
	} while (0)

/*
 * The harness never returns to a caller that has live callee-saved state: a
 * context switch is not a function return, so unwinding through one is not
 * something C can express. Every exit path therefore ends in _exit().
 */
static void die(int code)
{
	fflush(stdout);
	_exit(code);
}

/*
 * Build a task's frame at the top of its stack, the way the kernel's task
 * bootstrap does: the six saved registers, then the address to resume at.
 */
static struct task_frame *build_frame(struct task *t, u64 resume_at)
{
	t->stack = aligned_alloc(16, STACK_BYTES);
	if (!t->stack)
		die(2);

	/* Leave headroom below the frame so an overrun faults instead of
	 * silently producing a plausible-looking frame. */
	u64 top = ((u64)t->stack + STACK_BYTES - 256) & ~15ULL;

	struct task_frame *f = (struct task_frame *)top;
	f->r15 = SENTINEL(t->id * 16 + 15);
	f->r14 = SENTINEL(t->id * 16 + 14);
	f->r13 = SENTINEL(t->id * 16 + 13);
	f->r12 = SENTINEL(t->id * 16 + 12);
	f->rbx = SENTINEL(t->id * 16 + 5);
	f->rbp = SENTINEL(t->id * 16 + 6);
	f->ret = resume_at;
	return f;
}

static void report_round_robin(void)
{
	u64 expect = (u64)NTASKS * ROUNDS;

	printf("A. frame contract: 56-byte frame, ret addr at +48, resume rsp at frame+56\n");
	printf("B. saved registers: each task resumes holding its own rbx/rbp/r12-r15\n");
	printf("C. stack:          every resume on the resuming task's own stack\n");
	printf("   resumptions:");
	for (int i = 0; i < NTASKS; i++)
		printf(" task%d=%d", i, tasks[i].resumptions);
	printf("\n");

	/*
	 * D. The switch count. Every switch in this test must produce exactly
	 * one resumption, and nothing may re-enter the switch. The pre-fix
	 * resume path called back into the switch, so this number ran away and
	 * no task ever completed a round.
	 */
	printf("D. switch count:   %lu (expected %lu)\n", switches, expect);
	CHECK(switches == expect,
	      "switch count %lu != %lu — the resume path is re-entering the switch",
	      switches, expect);

	for (int i = 0; i < NTASKS; i++) {
		CHECK(tasks[i].resumptions == ROUNDS,
		      "task %d resumed %d times, expected %d", i,
		      tasks[i].resumptions, ROUNDS);
		CHECK((u64)tasks[i].frame % 16 == 0,
		      "task %d frame %lx is not 16-byte aligned", i,
		      (u64)tasks[i].frame);
		CHECK(tasks[i].context_rsp == (u64)tasks[i].frame,
		      "task %d published rsp %lx != frame %lx", i,
		      tasks[i].context_rsp, (u64)tasks[i].frame);
	}

	die(failures ? 1 : 0);
}

/*
 * The scheduler. Shaped like schedule() on purpose: recompute everything at
 * the top, switch as the last statement, jump back. Nothing derived before the
 * switch is trusted after it, because nothing derived before the switch can be
 * trusted after it — that is the invariant the kernel's resume point relies on,
 * and a harness that violated it would be testing something other than what the
 * kernel does.
 */
__attribute__((noinline)) static void sched_step(void)
{
	static struct task *rq[NTASKS];
	struct task *next;

	if (!seeded) {
		for (int i = 0; i < NTASKS; i++)
			rq[i] = &tasks[i];
		seeded = 1;
	}

reschedule:
	/* Re-derive from the run-queue-equivalent, never from a register that
	 * survived the last switch: the run queue is memory, the registers are
	 * whichever task ran last. */
	cur = rq[0];
	next = rq[1];
	rq[1] = cur;

	/* A task that has never been switched out has no published frame yet;
	 * it starts from the frame built for it. */
	context_switch(&cur->context_rsp, next->context_rsp ? next->context_rsp
							     : (u64)next->frame);
	switches++;
	goto reschedule;
}

/*
 * Where every task resumes: the analogue of the instruction after
 * `call context_switch` in schedule(). It has to run on the resuming task's own
 * stack, and it has to be reachable without touching anything the switch
 * mutated.
 */
void task_resume(void)
{
	struct task *t = cur;
	int n = t->resumptions;

	/* Captured by the naked stub at the instant control arrived. */
	u64 sp = resume_sp;
	u64 rbx = resume_rbx;
	u64 rbp = resume_rbp;
	u64 r12 = resume_r12;
	u64 r13 = resume_r13;
	u64 r14 = resume_r14;
	u64 r15 = resume_r15;

	if (n < NOBJ(ROUNDS)) {
		t->sp[n] = sp;
		t->rbx[n] = rbx;
		t->rbp[n] = rbp;
		t->r12[n] = r12;
		t->r13[n] = r13;
		t->r14[n] = r14;
		t->r15[n] = r15;
	}
	t->resumptions++;

	/* A task is only ever resumed at the frame it published. The one
	 * exception is the very first hand-off, which context_restore() makes
	 * without publishing anything: there is no outgoing task in that case. */
	if (!e_mode)
		CHECK(t->context_rsp == (u64)t->frame,
		      "task %d resumed but published rsp=%lx, frame=%lx", t->id,
		      t->context_rsp, (u64)t->frame);

	/* The stack: this must be the resuming task's own stack, at exactly the
	 * offset the 56-byte frame predicts. */
	u64 lo = (u64)t->stack;
	u64 hi = lo + STACK_BYTES;
	CHECK(sp >= lo && sp < hi,
	      "task %d resumed on a foreign stack: rsp=%lx not in [%lx,%lx)", t->id,
	      sp, lo, hi);
	CHECK(sp == RESUME_SP((u64)t->frame),
	      "task %d resumed at rsp=%lx, expected frame+56=%lx", t->id, sp,
	      RESUME_SP((u64)t->frame));

	/* The saved registers must be the task's own. */
	CHECK(rbx == SENTINEL(t->id * 16 + 5), "task %d rbx=%lx", t->id, rbx);
	CHECK(rbp == SENTINEL(t->id * 16 + 6), "task %d rbp=%lx", t->id, rbp);
	CHECK(r12 == SENTINEL(t->id * 16 + 12), "task %d r12=%lx", t->id, r12);
	CHECK(r13 == SENTINEL(t->id * 16 + 13), "task %d r13=%lx", t->id, r13);
	CHECK(r14 == SENTINEL(t->id * 16 + 14), "task %d r14=%lx", t->id, r14);
	CHECK(r15 == SENTINEL(t->id * 16 + 15), "task %d r15=%lx", t->id, r15);

	if (e_mode) {
		printf("E. context_restore: task %d frame=%lx, resumed once at rsp=%lx\n",
		       t->id, (u64)t->frame, sp);
		die(failures ? 1 : 0);
	}

	t->rounds_done++;

	if (t->rounds_done >= ROUNDS) {
		int done = 1;
		for (int i = 0; i < NTASKS; i++)
			if (tasks[i].rounds_done < ROUNDS)
				done = 0;
		if (done)
			report_round_robin();
	}

	sched_step();
}

static void run_frame_contract(void)
{
	for (int i = 0; i < NTASKS; i++) {
		tasks[i].id = i;
		tasks[i].frame = build_frame(&tasks[i], (u64)resume_stub_asm);
	}

	cur = &tasks[0];
	sched_step();
	printf("sched_step returned (unexpected)\n");
	die(3);
}

/* E: context_restore() alone — no outgoing frame published. */
static void run_restore_only(void)
{
	for (int i = 0; i < NTASKS; i++) {
		tasks[i].id = i;
		tasks[i].frame = build_frame(&tasks[i], (u64)resume_stub_asm);
	}

	cur = &tasks[0];
	e_mode = 1;
	context_restore((u64)tasks[0].frame);
	printf("context_restore returned (unexpected)\n");
	die(3);
}

int main(int argc, char **argv)
{
	int restore_only = argc > 1 && strcmp(argv[1], "--restore-only") == 0;

	printf("== context switch frame contract (host harness) ==\n");
	if (restore_only)
		run_restore_only();
	else
		run_frame_contract();
	printf("unreachable\n");
	return 3;
}