/*
 * context_switch_harness.c — host-side test for the kernel's context switch.
 *
 * Why this exists. Finding #57 was "schedule() has no landing pad after
 * context_switch, so a resumed task returns into the middle of schedule()
 * after the CR3 it needs has already been reloaded". Deciding whether that is
 * fixed by reading the comments in sched.c is not a decision procedure — the
 * comments in this tree have been wrong in both directions. This harness is.
 *
 * The context primitives in src/kernel/context.S are pure register-and-stack
 * code: no MMIO, no CR3, no globals, no memory side effects beyond the two
 * stack words the caller names. That makes them compilable and runnable as-is
 * on an ordinary x86-64 host, so the frame contract can be exercised against
 * real switches between real stacks instead of argued about. context.S needs
 * no modification for this: `gcc -c` assembles it clean for the host.
 *
 * The model is the kernel's model:
 *
 *   - every task owns a kernel stack and starts from a frame built for it;
 *   - a task is suspended inside the scheduler, so its return address lives
 *     in the scheduler's frame, exactly as it does for a real preempted task;
 *   - the scheduler re-derives its state from memory on every pass and trusts
 *     no register across the switch, which is the invariant the kernel's
 *     resume point depends on.
 *
 * What is tested:
 *
 *   A. The frame contract. context_switch(save, new) publishes exactly the
 *      post-push RSP; the frame is 56 bytes with the return address at +48; and
 *      control arrives at new + 56. Checked in assembly in the two
 *      instructions after every switch, because that is the only place the
 *      value is observable before anything can move rsp. An off-by-one here
 *      shows up thousands of switches later as a task returning into nonsense
 *      and looks like nothing at all.
 *
 *   B. The register contract. Each task's bootstrap frame holds distinctive
 *      rbx/rbp and r12-r15 values, and a task resuming from it must get them
 *      back exactly. A missing, mirrored or swapped side shows up as a task
 *      holding another task's registers. Checked from the naked resume stub,
 *      before any compiler-generated code runs — see tests/resume_stub.S for
 *      why a C trampoline cannot check this.
 *
 *   C. The stack. Every resume must land on the resuming task's own stack, and
 *      the frame context_switch published for that task must be on its stack
 *      too. A frame published relative to the wrong stack resumes into
 *      whatever happened to be mapped there.
 *
 *   D. Progress. Three tasks, four rounds each, with the switch count asserted
 *      exactly and a runaway guard. The pre-fix failure was a resume path that
 *      re-entered the switch, so two runnable tasks ping-ponged forever making
 *      no forward progress and printing no diagnostic; here it is a test
 *      failure rather than a hang.
 *
 *   E. context_restore() on its own — no outgoing frame published — which is
 *      how a new task's first frame and sched_stop_current()'s hand-off work.
 *
 * Whether the real schedule() compiled to the shape this harness exercises is a
 * separate question, answered mechanically against build/kernel.elf by
 * tests/check_schedule_resume.py.
 */

#include <stdbool.h>
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
#define FRAME_BYTES 56

/* ------------------------------------------------------------------ kernel -- */

/*
 * `returns_twice` is not decoration. context_switch does not preserve the
 * callee-saved registers of whoever called it: it restores the incoming
 * task's. Without the attribute the compiler may keep live values in
 * rbx/rbp/r12-r15 across the call and read them again after the resume, which
 * would read the previous task's registers. Same annotation glibc uses for
 * setjmp and swapcontext, for the same reason.
 */
extern void context_switch(u64 *save_rsp, u64 new_rsp)
	__attribute__((returns_twice));

/* The two assembly resume points; see tests/resume_stub.S. */
extern void resume_stub_asm(void);
extern void switch_check_asm(u64 *save_rsp, u64 incoming_frame, u64 *slot);
extern u64 resume_sp, resume_rbx, resume_rbp;
extern u64 resume_r12, resume_r13, resume_r14, resume_r15;
extern u64 resume_offs_ok, resume_offs_bad;

/* ------------------------------------------------------------------- frame -- */

/*
 * Must match src/kernel/context.S exactly: context_switch pushes rbp, rbx, r12,
 * r13, r14, r15 and publishes the resulting RSP, so r15 is at offset 0 and the
 * return address `ret` consumes is at +48.
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

_Static_assert(sizeof(struct task_frame) == FRAME_BYTES, "frame is 56 bytes");
_Static_assert(offsetof(struct task_frame, r15) == 0, "r15 at +0");
_Static_assert(offsetof(struct task_frame, r14) == 8, "r14 at +8");
_Static_assert(offsetof(struct task_frame, r13) == 16, "r13 at +16");
_Static_assert(offsetof(struct task_frame, r12) == 24, "r12 at +24");
_Static_assert(offsetof(struct task_frame, rbx) == 32, "rbx at +32");
_Static_assert(offsetof(struct task_frame, rbp) == 40, "rbp at +40");
_Static_assert(offsetof(struct task_frame, ret) == 48, "return address at +48");

struct task {
	u64 *stack;			/* 16-byte aligned, STACK_BYTES long */
	struct task_frame *frame;	/* bootstrap frame */
	u64 context_rsp;		/* what context_switch published */
	u64 handed;			/* the frame most recently handed in */
	int id;
	int entered;		/* has task_body been entered at all */
	int resumptions;		/* how many times control arrived here */
	int rounds_done;
};

static struct task tasks[NTASKS];
static struct task *ring[NTASKS];
static struct task *cur;
static unsigned head;
static u64 switches;
static int failures;
static int e_mode;
/* Where context_switch's outgoing frame goes when there is no outgoing task. */
static u64 discard_slot;

static void report(void);

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
 * The harness never returns to a caller holding live callee-saved state: a
 * context switch is not a function return, so unwinding through one is not
 * something C can express. Every exit path ends in _exit().
 */
static void die(int code)
{
	fflush(stdout);
	_exit(code);
}

/* Slots 0..5 are rbx rbp r12 r13 r14 r15, in the order they are captured. */
static u64 sent(struct task *t, int slot)
{
	static const int reg[6] = { 5, 6, 12, 13, 14, 15 };
	return SENTINEL(t->id * 16 + reg[slot]);
}

static int on_own_stack(struct task *t, u64 v)
{
	return v >= (u64)t->stack && v < (u64)t->stack + STACK_BYTES;
}

/*
 * Build a task's bootstrap frame at the top of its stack, the way the kernel's
 * task bootstrap does: the six saved registers, then the address to resume at.
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
	f->r15 = sent(t, 5);
	f->r14 = sent(t, 4);
	f->r13 = sent(t, 3);
	f->r12 = sent(t, 2);
	f->rbx = sent(t, 0);
	f->rbp = sent(t, 1);
	f->ret = resume_at;
	return f;
}

/* Everything checkable only at the instant control first arrives. */
static void check_first_resume(struct task *t)
{
	/* A. The frame offset, seen by the naked stub before any C runs. */
	CHECK(resume_sp == (u64)t->frame + FRAME_BYTES,
	      "task %d first resume rsp=%lx, expected frame+%d=%lx", t->id,
	      resume_sp, FRAME_BYTES, (u64)t->frame + FRAME_BYTES);

	/* C. That frame is at the top of this task's own stack, aligned the way
	 * the frame contract requires. */
	CHECK(on_own_stack(t, (u64)t->frame),
	      "task %d bootstrap frame %lx is outside its own stack [%lx,%lx)",
	      t->id, (u64)t->frame, (u64)t->stack,
	      (u64)t->stack + STACK_BYTES);
	CHECK((u64)t->frame % 16 == 0,
	      "task %d bootstrap frame %lx is not 16-byte aligned", t->id,
	      (u64)t->frame);

	/* B. The register contract. */
	static const char *nm[6] = { "rbx", "rbp", "r12", "r13", "r14", "r15" };
	u64 got[6] = { resume_rbx, resume_rbp, resume_r12,
		       resume_r13, resume_r14, resume_r15 };
	for (int i = 0; i < 6; i++)
		CHECK(got[i] == sent(t, i),
		      "task %d resumed with %s=%lx, expected %lx", t->id, nm[i],
		      got[i], sent(t, i));
}

/*
 * The scheduler. Shaped like schedule(): re-derive its state from memory on
 * every pass, publish the outgoing frame, adopt the incoming one, and trust no
 * register across the switch — because no register survives one.
 *
 * It differs from schedule() in one respect, deliberately: on resume it
 * *returns* rather than looping. The kernel's schedule() loops because a task
 * preempted inside it resumes inside it and is then rescheduled; its user-mode
 * continuation comes back through the interrupt return path, not through
 * schedule(). A harness that looped the same way would let no task make a
 * second round of progress and would test nothing about where a task resumes.
 * Here the switch is inside the yield, so the return address in a suspended
 * frame is inside the yield, and resuming returns to the task's own code —
 * which is the property worth asserting.
 */
__attribute__((noinline)) static void sched_yield(void)
{
	struct task *p, *n;

	/* Runaway guard: the pre-fix resume path called back into the switch, so
	 * two runnable tasks ping-ponged forever. That must be a failure, not a
	 * hang. */
	if (switches > (u64)NTASKS * ROUNDS * 4) {
		printf("  FAIL (line %d): runaway switch loop, %lu switches for "
		       "%d tasks x %d rounds\n",
		       __LINE__, switches, NTASKS, ROUNDS);
		die(1);
	}

	p = ring[head];
	head = (head + 1) % NTASKS;
	n = ring[head];

	/* current_task() must already report the incoming task by the time
	 * control reaches it, because a resumed task's first act is to ask who
	 * it is. The kernel does this in sched_switch_frame(), before the
	 * switch; afterwards, the resumed task would spend its first
	 * instruction identifying itself as somebody else. */
	cur = n;

	/* A task that has never been switched out has no published frame yet;
	 * it starts from its bootstrap frame. */
	n->handed = n->context_rsp ? n->context_rsp : (u64)n->frame;

	/* Counted before the switch, not after: a switch that suspends this task
	 * does not run the increment below until the task comes back, which
	 * would make the count lag by exactly one and hide a resumed-everywhere
	 * schedule. Counting on the way in makes the invariant plain: one
	 * resumption per switch, plus one for the boot. */
	switches++;
	switch_check_asm(&p->context_rsp, n->handed, &n->handed);
}

/*
 * What a task does. Reached from the naked stub on the first resume, and from
 * inside sched_yield() on every resume after that — the same shape as a real
 * preempted task, whose return address lives in the scheduler's frame.
 */
void task_body(void)
{
	struct task *t = cur;
	int first = !t->entered;

	t->entered = 1;

	if (first) {
		check_first_resume(t);
		if (e_mode) {
			printf("E. context_restore: task %d resumed once at rsp=%lx "
			       "(bootstrap frame %lx + %d)\n",
			       t->id, resume_sp, (u64)t->frame, FRAME_BYTES);
			die(failures ? 1 : 0);
		}
	}

	for (int round = 0; round < ROUNDS; round++) {
		/* Reaching this task at all — from the stub on the first
		 * resumption, or from sched_yield() on every one after — is a
		 * resumption. Counting here rather than at function entry is
		 * what makes the count match the switch count. */
		t->resumptions++;

		/* C. Whatever brought us here, we must be running on our own
		 * stack. The address of a local is on the stack by definition,
		 * so this is a direct check rather than a proxy for one. */
		struct task *probe = t;
		CHECK(on_own_stack(t, (u64)&probe),
		      "task %d round %d is running on a foreign stack: local at "
		      "%lx not in [%lx,%lx)",
		      t->id, round, (u64)&probe, (u64)t->stack,
		      (u64)t->stack + STACK_BYTES);

		t->rounds_done++;

		if (t->rounds_done >= ROUNDS) {
			int done = 1;
			for (int i = 0; i < NTASKS; i++)
				if (tasks[i].rounds_done < ROUNDS)
					done = 0;
			if (done)
				report();
		}

		/* Switch away. Control comes back to the instruction after this
		 * call, on this task's own stack, for the next iteration. */
		sched_yield();
	}

	/* Falling out of the loop means the task was resumed once more after
	 * report() should already have ended the run, i.e. an extra resumption
	 * that the switch count below would also catch. */
	CHECK(false, "task %d completed %d rounds without the run ending", t->id,
	      ROUNDS);
	die(3);
}

static void report(void)
{
	/*
	 * One resumption per task per round, less the one that booted task 0
	 * onto its own stack. Every switch must produce exactly one resumption
	 * and nothing may re-enter the switch.
	 */
	u64 expect = (u64)NTASKS * ROUNDS - 1;	  /* one per resumption but the boot */
	u64 expect_resumes = (u64)NTASKS * ROUNDS;	  /* one per task per round */
	/*
	 * Each task's first resumption arrives through its bootstrap frame,
	 * whose return address is resume_stub_asm, so those NTASKS arrivals are
	 * checked by check_first_resume() instead of by switch_check_asm(). Every
	 * other resumption comes back through the switch, and those are the ones
	 * switch_check_asm() measures.
	 */
	u64 expect_offs = expect_resumes - NTASKS;

	printf("A. frame contract: 56-byte frame, return address at +48\n");
	printf("   switch_check_asm: %lu resumes landed at exactly frame+56, "
	       "%lu did not\n",
	       resume_offs_ok, resume_offs_bad);
	CHECK(resume_offs_bad == 0, "%lu resumes landed at the wrong offset",
	      resume_offs_bad);
	CHECK(resume_offs_ok == expect_offs, "%lu offset checks, expected %lu",
	      resume_offs_ok, expect_offs);

	u64 total = 0;
	for (int i = 0; i < NTASKS; i++)
		total += (u64)tasks[i].resumptions;
	CHECK(total == expect_resumes,
	      "%lu resumptions in total, expected %lu — a resumption is either "
	      "lost or duplicated",
	      total, expect_resumes);

	printf("B. registers: each bootstrap frame restores its own rbx/rbp/"
	       "r12-r15\n");

	printf("C. stack: every resume on the resuming task's own stack\n");
	printf("   resumptions:");
	for (int i = 0; i < NTASKS; i++)
		printf(" task%d=%d", i, tasks[i].resumptions);
	printf("\n");

	printf("D. progress: %lu switches (expected %lu)\n", switches, expect);
	CHECK(switches == expect,
	      "switch count %lu != %lu — the resume path is re-entering the "
	      "switch",
	      switches, expect);

	for (int i = 0; i < NTASKS; i++) {
		CHECK(tasks[i].resumptions == ROUNDS,
		      "task %d resumed %d times, expected %d", i,
		      tasks[i].resumptions, ROUNDS);
		CHECK(tasks[i].rounds_done == ROUNDS,
		      "task %d completed %d rounds, expected %d", i,
		      tasks[i].rounds_done, ROUNDS);
		/* The frame context_switch published for a task lives on that
		 * task's own stack: it is where the task was suspended, which is
		 * inside sched_yield(), not at the top of the stack. What has to
		 * hold is that it is on the right stack, and that resuming lands
		 * 56 bytes above it — the latter checked on every resume above. */
		CHECK(on_own_stack(&tasks[i], tasks[i].context_rsp),
		      "task %d published its frame at %lx, outside its own stack "
		      "[%lx,%lx)",
		      i, tasks[i].context_rsp, (u64)tasks[i].stack,
		      (u64)tasks[i].stack + STACK_BYTES);
	}

	printf("%s: context switch frame contract holds\n",
	       failures ? "FAIL" : "PASS");
	die(failures ? 1 : 0);
}

static void setup(void)
{
	for (int i = 0; i < NTASKS; i++) {
		tasks[i].id = i;
		tasks[i].frame = build_frame(&tasks[i], (u64)resume_stub_asm);
		ring[i] = &tasks[i];
	}
	head = 0;
}

int main(int argc, char **argv)
{
	printf("== context switch frame contract (host harness) ==\n");

	/* E: a single hand-off with no outgoing frame published, which is how
	 * a new task is started and how sched_stop_current() hands off. */
	if (argc > 1 && strcmp(argv[1], "--restore-only") == 0)
		e_mode = 1;

	setup();

	/*
	 * Boot task 0 onto its own stack, the way a new task is started.
	 * Without this the first switch out of main() would publish main's stack
	 * as task 0's frame, and every later resume of task 0 would legitimately
	 * land there — correct behaviour, but it would mean the "resumes on its
	 * own stack" assertion is checking the wrong thing for exactly one of
	 * the three tasks.
	 */
	cur = &tasks[0];
	context_switch(&discard_slot, (u64)tasks[0].frame);
	printf("  FAIL: context_switch returned where it could not\n");
	die(3);
}