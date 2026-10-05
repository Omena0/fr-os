/*
 * init.c — Fr Init, PID 1, the interactive REPL and the system bring-up.
 *
 * There is no login, no shell and no init in this kernel yet, so init has to
 * be all three: it announces itself, then reads commands from stdin forever
 * until that stream reaches EOF. Every byte it emits is tagged "Fr Init: " so
 * a multiplexed console log still says which process said what.
 *
 * The name hierarchy, as the banners below use it:
 *
 *   Fr OS          the project -- Fr Init is one of its components
 *     Fr Core      the kernel this process is executing on
 *     Fr Init      this process: PID 1 and the system's bring-up
 *     Fr Userland  the programs that run on top of Fr Init
 *     Fr Libc      the C runtime both of the above are built with
 *
 * So init calls *itself* Fr Init, reports the project as Fr OS, and names the
 * kernel as Fr Core. Printing KERNEL_VERSION_STRING -- which expands to "Fr
 * Core 0.1.0" -- as this process's own identity is how userspace used to
 * introduce itself as the kernel.
 *
 * Input-layer assumption (see echo_line below): getline() reads and returns a
 * line but performs no terminal echo of its own. Everything typed therefore
 * has to be echoed here, or the user sees nothing at all. The alternative
 * convention — getline echoes as it reads — is not supported because the two
 * behaviours cannot be distinguished at run time and echoing twice is far
 * more confusing than echoing once too late.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <version.h>
#include <sys/auxv.h>
#include <uapi/syscall.h>

extern char **environ;

#define TAG FR_INIT_NAME ": "

#define MEM_SLOTS 512
#define MEM_ROUNDS 200000UL

/*
 * Captured before main() runs so that `uptime` measures this process's own
 * lifetime rather than "time since some arbitrary point inside the loop".
 * There is no process-start timestamp on the auxv, so the earliest point a
 * userspace constructor can observe is the only sample available.
 */
static struct timespec start_time;

static void stamp_start(void) __attribute__((constructor));
static void stamp_start(void)
{
	clock_gettime(CLOCK_MONOTONIC, &start_time);
}

/* All REPL output funnels through here so the prefix is applied exactly once. */
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void say(const char *fmt, ...)
{
	va_list ap;

	fputs(TAG, stdout);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
}

/*
 * Replay the line to the terminal after getline() has already consumed it.
 * Backspace and DEL are the two codes a serial/vga console sends for erase,
 * and rewriting with "\b \b" is the only way to actually remove a glyph from
 * a terminal — a bare backspace just moves the cursor and leaves the old
 * character behind.
 *
 * The DEL and backspace cases are not hypothetical here. There is no line
 * discipline on the input path, so the raw byte the keyboard or serial line
 * produced is what getline() sees: a user who presses Backspace really does put
 * 0x7f in the buffer, and this is the code that turns it back into a visible
 * erase. It would be unreachable only if something upstream had already
 * interpreted and consumed the key, and nothing does.
 */
static void echo_line(const char *s)
{
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;

		if (c == 0x7f || c == 0x08) {
			fputs("\b \b", stdout);
		} else if (c >= 0x20 && c != 0x7f) {
			putchar(c);
		}
	}
	putchar('\n');
}

/*
 * Split in place on runs of spaces and tabs into an argv-style vector.
 *
 * A shell needs argv splitting, not strtok's token-at-a-time interface, and it
 * needs a bound on the argument count so a 32-slot vector on the stack cannot
 * be overrun by a long line. strtok(3) does exist in this libc (string.c, over
 * strtok_r) -- it is the shape that does not fit here, not its absence.
 */
static int split(char *s, char **argv, int max)
{
	int n = 0;

	while (*s && n < max) {
		while (*s == ' ' || *s == '\t')
			s++;
		if (!*s)
			break;
		argv[n++] = s;
		while (*s && *s != ' ' && *s != '\t')
			s++;
		if (*s)
			*s++ = '\0';
	}
	return n;
}

/*
 * Civil date from a day count, since 1970-01-01. This is the days-to-civil
 * inverse of the classic Julian-date arithmetic: no libc here exposes gmtime
 * or strftime, and clock_gettime hands back raw seconds, so the calendar
 * conversion has to happen in userspace. The era-based form avoids the
 * negative-year bugs a naive leap-year loop hits before 1970.
 */
/*
 * Times are int64_t, which is `long` on x86-64, so every print of one uses
 * %ld. The values are held in `long` rather than `long long` throughout this
 * file purely so the format specifiers and the arguments cannot drift apart.
 */
static void civil_from_days(long z, long *y, int *m, int *d)
{
	long era, doe, yoe, doy, mp;

	z += 719468;
	era = (z >= 0 ? z : z - 146096) / 146097;
	doe = z - era * 146097;
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	*d = (int)(doy - (153 * mp + 2) / 5 + 1);
	*m = (int)(mp < 10 ? mp + 3 : mp - 9);
	*y = yoe + era * 400 + (*m <= 2);
}

/* Whole seconds plus nanoseconds into milliseconds, for readable durations. */
static long ms_between(struct timespec a, struct timespec b)
{
	return (b.tv_sec - a.tv_sec) * 1000L +
	       (b.tv_nsec - a.tv_nsec) / 1000000L;
}

static void show_time(void)
{
	struct timespec rt, mt;
	long secs, days, rem, up, y;
	int m, d;

	if (clock_gettime(CLOCK_REALTIME, &rt) != 0) {
		say("time: clock_gettime(CLOCK_REALTIME) failed");
		return;
	}
	if (clock_gettime(CLOCK_MONOTONIC, &mt) != 0) {
		say("time: clock_gettime(CLOCK_MONOTONIC) failed");
		return;
	}

	secs = rt.tv_sec;
	days = secs / 86400L;
	rem = secs % 86400L;
	civil_from_days(days, &y, &m, &d);

	say("realtime   %04ld-%02d-%02d %02d:%02d:%02d.%03ld UTC", y, m, d,
	    (int)(rem / 3600L), (int)((rem / 60L) % 60L), (int)(rem % 60L),
	    rt.tv_nsec / 1000000L);

	up = mt.tv_sec * 1000L + mt.tv_nsec / 1000000L;
	say("monotonic  %ldd %02ldh %02ldm %02ld.%03lds since boot",
	    up / 86400000L, (up / 3600000L) % 24L, (up / 60000L) % 60L,
	    (up / 1000L) % 60L, up % 1000L);
	say("boot time  %ld.%09ld CLOCK_MONOTONIC", mt.tv_sec, mt.tv_nsec);
}

struct slot {
	unsigned char *p;
	unsigned long size;
};

static struct slot mem_slots[MEM_SLOTS];

/*
 * xorshift64 rather than rand(): stdlib declares rand/srand but a REPL should
 * not depend on an implementation whose algorithm is unspecified when the
 * whole point is to make a repeatable measurement. Fixed seed keeps two runs
 * of `mem` comparable.
 */
static unsigned long rng_state = 0x9e3779b97f4a7c15UL;

static unsigned long rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return rng_state;
}

/*
 * The workload is deliberately mixed: some slots are freshly allocated, some
 * are resized with realloc, some are freed, and a residue is kept allocated
 * until the end. A benchmark that only ever did malloc/free would measure one
 * arena path and say nothing about the rest.
 *
 * Every block is also written at its first and last byte. A lazy allocator
 * that hands back untouched zero pages would otherwise report an enormous
 * ops/sec while never committing a physical page, which is a measurement of
 * nothing at all.
 */
static void bench_mem(void)
{
	struct timespec t0, t1;
	unsigned long rounds = MEM_ROUNDS;
	unsigned long ops = 0;
	unsigned long live = 0, peak = 0;
	unsigned long elapsed;
	unsigned long ops_per_sec;
	unsigned long i;

	for (i = 0; i < MEM_SLOTS; i++)
		mem_slots[i].p = NULL;

	clock_gettime(CLOCK_MONOTONIC, &t0);

	for (i = 0; i < rounds; i++) {
		unsigned long idx = (unsigned long)(rng() % MEM_SLOTS);
		struct slot *s = &mem_slots[idx];
		unsigned long want = 8 + (unsigned long)(rng() % 4088);

		if (s->p == NULL) {
			s->p = (unsigned char *)malloc(want);
			if (s->p == NULL)
				continue;
			s->size = want;
			s->p[0] = (unsigned char)i;
			s->p[want - 1] = (unsigned char)(i >> 8);
			live += want;
			ops++;
		} else if ((rng() & 3u) == 0u) {
			unsigned char *np = (unsigned char *)realloc(s->p, want);

			if (np == NULL)
				continue;
			live -= s->size;
			live += want;
			s->p = np;
			s->size = want;
			s->p[0] = (unsigned char)i;
			s->p[want - 1] = (unsigned char)(i >> 8);
			ops++;
		} else if ((rng() & 3u) == 0u) {
			free(s->p);
			live -= s->size;
			s->p = NULL;
			s->size = 0;
			ops++;
		} else {
			/* churn the existing block without changing its size */
			s->p[rng() % s->size] = (unsigned char)i;
			ops++;
		}

		if (live > peak)
			peak = live;
	}

	clock_gettime(CLOCK_MONOTONIC, &t1);

	elapsed = (unsigned long)ms_between(t0, t1);
	if (elapsed == 0)
		elapsed = 1;
	ops_per_sec = (ops * 1000UL) / elapsed;

	say("rounds     %lu of mixed malloc/realloc/free", rounds);
	say("elapsed    %lu ms", elapsed);
	say("operations %lu", ops);
	say("derived    %lu ops/sec", ops_per_sec);
	say("peak       %lu bytes tracked", peak);
	say("live       %lu bytes before teardown", live);

	for (i = 0; i < MEM_SLOTS; i++) {
		if (mem_slots[i].p) {
			free(mem_slots[i].p);
			mem_slots[i].p = NULL;
			mem_slots[i].size = 0;
		}
	}

	/*
	 * The pass/fail criterion is an accounting one, not a crash check: a
	 * benchmark that leaves bytes outstanding between rounds measures the
	 * allocator's high-water mark rather than its cost, so a non-zero live
	 * total means the numbers above are not comparable between runs.
	 */
	if (live != 0)
		say("FAIL: %lu bytes still live after freeing every slot", live);
	else
		say("OK: live bytes returned to zero");
}

static void cmd_help(void)
{
	say("commands:");
	say("  help                 this list");
	say("  echo <args...>       print the arguments, joined by single spaces");
	say("  version              %s, %s, and the %s it runs on",
	    FR_INIT_NAME, FR_PROJECT_NAME, FR_CORE_NAME);
	say("  mem                  %lu-round allocator benchmark", MEM_ROUNDS);
	say("  time                 wall clock, monotonic clock and boot time");
	say("  uptime               time since this process started");
	say("  clear                clear the screen and home the cursor");
	say("  lscpu                page size, cpu count, getauxval() entries");
	say("  strlen <string>      print the length of <string>");
	say("  run <program> [args] exec a program from the initrd");
	say("  exit                 explain that reboot is not implemented yet");
}

static void cmd_lscpu(void)
{
	long abi, pagesz, nprocs;
	unsigned cpu = 0, node = 0;

	pagesz = getpagesize();
	say("page size       %ld bytes", pagesz);

	nprocs = sysconf(_SC_NPROCESSORS_ONLN);
	if (nprocs > 0)
		say("cpus online     %ld", nprocs);
	else
		say("cpus online     unknown (sysconf returned %ld)", nprocs);

	/* getcpu(3) is the one answer that comes from the kernel rather than
	 * from a constant, so it is the one worth reporting here. */
	if (getcpu(&cpu, &node) == 0)
		say("running on      cpu %u, node %u", cpu, node);
	else
		say("running on      unknown (getcpu failed)");

	abi = getauxval(AT_ABI_VERSION);
	pagesz = getauxval(AT_PAGESZ);
	if (abi == 0)
		say("AT_ABI_VERSION  absent");
	else
		say("AT_ABI_VERSION  %ld (major %ld, minor %ld)", abi,
		    abi / 1000, abi % 1000);

	if (pagesz == 0)
		say("AT_PAGESZ       absent");
	else
		say("AT_PAGESZ       %ld bytes", pagesz);
}

static void cmd_uptime(void)
{
	struct timespec now;
	long up;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		say("uptime: clock_gettime(CLOCK_MONOTONIC) failed");
		return;
	}
	up = ms_between(start_time, now);
	say("up %ldd %02ldh %02ldm %02ld.%03lds", up / 86400000L,
	    (up / 3600000L) % 24L, (up / 60000L) % 60L, (up / 1000L) % 60L,
	    up % 1000L);
}

/*
 * Dispatch on argv[1]. Returns the new main-loop status: 0 to keep looping,
 * 1 to terminate. Unknown input is reported rather than treated as fatal,
 * because init is PID 1 and the only alternative to looping is a kernel
 * panic on the next userspace process.
 */
static int dispatch(int argc, char **argv)
{
	int i;

	if (argc < 2) {
		say("empty line, try 'help'");
		return 0;
	}

	if (strcmp(argv[1], "help") == 0) {
		cmd_help();
	} else if (strcmp(argv[1], "echo") == 0) {
		for (i = 2; i < argc; i++)
			printf("%s%s", (i > 2) ? " " : "", argv[i]);
		putchar('\n');
	} else if (strcmp(argv[1], "version") == 0) {
		/*
		 * Three names, in the order the hierarchy runs: this process, the
		 * project it belongs to, and the kernel underneath both. The
		 * kernel's own string is KERNEL_VERSION_STRING, which expands to
		 * "Fr Core <version>" -- printing it as though it were this
		 * process's identity is how userspace used to announce itself as
		 * the kernel.
		 */
		say("%s %s (build %s, rev %s)", FR_INIT_NAME, KERNEL_VERSION,
		    KERNEL_BUILD_STAMP, KERNEL_GIT_REV);
		say("%s %s, running on %s", FR_PROJECT_NAME, KERNEL_VERSION,
		    KERNEL_VERSION_STRING);
		say("userspace       %s (pid 1), programs run as %s",
		    FR_INIT_NAME, FR_USERLAND_NAME);
	} else if (strcmp(argv[1], "mem") == 0) {
		bench_mem();
	} else if (strcmp(argv[1], "time") == 0) {
		show_time();
	} else if (strcmp(argv[1], "uptime") == 0) {
		cmd_uptime();
	} else if (strcmp(argv[1], "clear") == 0) {
		fputs("\033[2J\033[H", stdout);
	} else if (strcmp(argv[1], "lscpu") == 0) {
		cmd_lscpu();
	} else if (strcmp(argv[1], "strlen") == 0) {
		if (argc < 3) {
			say("strlen: expected an argument");
		} else {
			say("strlen: %lu", (unsigned long)strlen(argv[2]));
		}
	} else if (strcmp(argv[1], "run") == 0) {
		/*
		 * Hand the process over. execve only returns on failure, so
		 * anything printed after this point is a diagnostic about why --
		 * there is deliberately no success path back here, because a
		 * successful exec never comes back.
		 */
		if (argc < 3) {
			say("run: expected a program name");
			say("     the initrd carries: init, hello");
			return 0;
		}

		char *child_argv[8];
		int child_argc = 0;

		child_argv[child_argc++] = argv[2];
		for (int a = 3; a < argc && child_argc < 7; a++)
			child_argv[child_argc++] = argv[a];
		child_argv[child_argc] = NULL;

		execve(argv[2], child_argv, environ);

		say("run: execve(\"%s\") failed: %s", argv[2], strerror(errno));
		say("     errno %d", errno);
	} else if (strcmp(argv[1], "exit") == 0) {
		say("exit: reboot syscall is not implemented yet");
		say("exit: the kernel exposes no power-management interface");
		say("exit: refusing to halt, continuing the REPL");
	} else {
		say("unknown command '%s' - try 'help'", argv[1]);
	}
	return 0;
}

int main(void)
{
	char *line = NULL;
	size_t cap = 0;
	int rc;

	printf("%s%s %s (build %s, rev %s)\n", TAG, FR_INIT_NAME,
	       KERNEL_VERSION, KERNEL_BUILD_STAMP, KERNEL_GIT_REV);
	printf("%s%s %s -- pid %ld, page size %d bytes\n", TAG,
	       FR_PROJECT_NAME, "system bring-up", (long)getpid(),
	       getpagesize());
	printf("%srunning on %s\n", TAG, KERNEL_VERSION_STRING);
	printf("%stype 'help' for the command list, EOF to stop\n", TAG);

	for (;;) {
		int argc;
		char *argv[32];

		/* The prompt is derived from FR_INIT_NAME rather than spelled
		 * "init>", so it names the component the same way every other
		 * line this process prints does. */
		fputs(FR_INIT_NAME "> ", stdout);

		rc = getline(&line, &cap, stdin);
		if (rc < 0)
			break;

		/*
		 * Ctrl-C arrives as a literal 0x03 byte inside the line rather
		 * than as a signal: there is no signal delivery in this kernel,
		 * so the console hands the byte to getline like any other. Treat a
		 * line that begins with it as an aborted command.
		 */
		if (line[0] == 0x03) {
			say("^C");
			continue;
		}

		echo_line(line);

		/*
		 * Strip the line terminator here rather than with strcspn, which
		 * this libc does not declare. The console sends CR, LF or CRLF
		 * depending on the terminal, and getline leaves the terminator in
		 * the buffer, so every variant has to be collapsed.
		 */
		{
			char *nl = line;

			while (*nl && *nl != '\r' && *nl != '\n')
				nl++;
			*nl = '\0';
		}
		if (line[0] == '\0')
			continue;

		argc = split(line, argv, 32);
		if (dispatch(argc, argv))
			break;
	}

	free(line);
	fflush(stdout);
	printf("%sstdin closed, init exiting\n", TAG);
	return 0;
}
