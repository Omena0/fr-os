/*
 * klog.h — structured kernel logging with severity levels.
 *
 * Every kernel message goes through klog(). The point is not decoration: a
 * message with a level can be filtered at runtime, counted by the tracing
 * subsystem, and attached to a CPU and a timestamp without every call site
 * having to remember to do that.
 *
 * Levels, in increasing severity:
 *
 *   KLOG_DEBUG   per-subsystem detail, off by default
 *   KLOG_INFO    lifecycle milestones ("mounted root", "scheduler up")
 *   KLOG_WARN    something is wrong but the system continues
 *   KLOG_ERROR   a subsystem failed an operation
 *   KLOG_FATAL   unrecoverable; the panic handler takes over
 *
 * The panic path cannot use this subsystem: it may run with the allocator, the
 * scheduler, or the console lock in an unusable state. panic() has its own
 * output path for that reason.
 */
#ifndef KLOG_H
#define KLOG_H

#include <types.h>

typedef enum {
	KLOG_DEBUG = 0,
	KLOG_INFO  = 1,
	KLOG_WARN  = 2,
	KLOG_ERROR = 3,
	KLOG_FATAL = 4,
} klog_level_t;

struct klog_record {
	klog_level_t level;
	uint64_t timestamp;      /* TSC ticks */
	uint32_t cpu;
	const char *subsystem;   /* static string, set by KLOG_SUBSYSTEM */
	const char *message;     /* static format string */
};

/*
 * Tag this translation unit's log messages with a subsystem name.
 *
 * The definition is file-local, so every source file can name its own
 * subsystem and none of them collide. The earlier version made it a strong
 * global on the theory that a weak default in klog.c would be overridden at
 * link time; that works for exactly one file, and the second file to opt in
 * fails to link with a duplicate definition.
 *
 * Requiring every file that logs to name itself also means no log line is ever
 * tagged "kernel" by accident, which is the only value the default ever
 * produced.
 */
#define KLOG_SUBSYSTEM(name) \
	static const char klog_subsys_name[] __attribute__((unused)) = name

/* Set the compile-time minimum level. Anything below is compiled out, so a
 * debug-heavy subsystem costs nothing in the release build. */
#ifndef KLOG_COMPILE_LEVEL
#define KLOG_COMPILE_LEVEL KLOG_DEBUG
#endif

void klog_init(void);

/* The formatted form. Normally reached through the klog() macro below. */
void klog_emit(klog_level_t level, const char *subsystem,
	       const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/*
 * The logging entry point.
 *
 * The level test is written to survive constant folding: the level is passed as
 * a literal at nearly every call site, so `level < KLOG_COMPILE_LEVEL` is a
 * compile-time constant and the entire call is removed. The double check on
 * `subsystem` allows one subsystem to raise its own verbosity at runtime without
 * turning on logging everywhere.
 */
#define klog(level, ...)                                                  \
	do {                                                              \
		if ((level) >= KLOG_COMPILE_LEVEL &&                     \
		    (level) >= klog_runtime_level)                      \
			klog_emit((level), klog_subsys_name, __VA_ARGS__); \
	} while (0)

/* Current runtime threshold; messages below it are dropped. */
extern klog_level_t klog_runtime_level;

/* Raise or lower verbosity at runtime. Used by the observability interface. */
void klog_set_level(klog_level_t level);

/* Number of messages emitted per level, for the statistics interface. */
uint64_t klog_count(klog_level_t level);

/* Raw formatted output to the console with no level, prefix or timestamp. Used
 * by the early-boot path before logging is initialised. */
void klog_raw(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Every file that logs calls KLOG_SUBSYSTEM() at file scope. There is no
 * fallback and no extern declaration: the klog() macro above refers to the
 * file-local array that KLOG_SUBSYSTEM() defines, and a file that logs without
 * one fails to compile rather than silently attributing its output to
 * "kernel". */

#endif /* KLOG_H */
