/*
 * klog.c — structured kernel logging.
 *
 * Output goes to the console, which fans out to serial and the framebuffer. The
 * prefix carries level, CPU and timestamp, because a bare message is close to
 * useless in a multicore system where two subsystems can emit interleaved and
 * the ordering is the interesting part.
 *
 * Formatting is done through kprintf's sink interface into a fixed stack
 * buffer. A heap allocation here would be a bug: klog is called from
 * allocation-free contexts (early boot, panic paths, interrupt handlers), and
 * a message that cannot be printed because the allocator is exhausted is a
 * message lost exactly when it mattered.
 */

#include <klog.h>
#include <console.h>
#include <cpu_features.h>
#include <io.h>
#include <kprintf.h>
#include <kstring.h>
#include <percpu.h>

klog_level_t klog_runtime_level = KLOG_INFO;

static uint64_t klog_counts[5];
static bool klog_initialised;

/* Set in klog_init(); before that, timestamps read as zero. */
static uint64_t boot_tsc;
static uint64_t tsc_khz;

static const char level_names[] = { 'D', 'I', 'W', 'E', 'F' };

/*
 * Emit one formatted record.
 *
 * The buffer is sized for a realistic log line. A longer line is truncated
 * rather than overflowed: truncation is visible and harmless, whereas a stack
 * overflow in a logging routine is neither.
 */
void klog_emit(klog_level_t level, const char *subsystem,
	       const char *fmt, ...)
{
	__builtin_va_list ap;
	char msg[512];
	char line[640];

	if ((int)level < 0 || (int)level > KLOG_FATAL)
		return;
	if (level < klog_runtime_level)
		return;

	__atomic_fetch_add(&klog_counts[level], 1, __ATOMIC_RELAXED);

	__builtin_va_start(ap, fmt);
	kvsnprintf(msg, sizeof(msg), fmt, ap);
	__builtin_va_end(ap);

	uint32_t cpu = percpu_num_cpus() ? this_cpu_id() : 0;
	uint64_t now = 0;

	if (klog_initialised && tsc_khz) {
		/* Convert TSC ticks to milliseconds. Integer arithmetic is used
		 * deliberately: this runs in interrupt context where dividing by a
		 * float would require enabling the FPU. */
		uint64_t ticks = rdtsc() - boot_tsc;
		now = ticks * 1000 / tsc_khz;
	}

	ksize_t n = ksnprintf(line, sizeof(line), "[%5llu.%03llu cpu%u %s/%c] %s",
			      (unsigned long long)(now / 1000),
			      (unsigned long long)(now % 1000),
			      cpu,
			      subsystem ? subsystem : "?",
			      level_names[level],
			      msg);

	(void)n;
	console_write(line);
}

void klog_set_level(klog_level_t level)
{
	klog_runtime_level = level;
}

uint64_t klog_count(klog_level_t level)
{
	if ((int)level < 0 || (int)level > KLOG_FATAL)
		return 0;
	return klog_counts[level];
}

void klog_raw(const char *fmt, ...)
{
	__builtin_va_list ap;
	char buf[512];

	__builtin_va_start(ap, fmt);
	kvsnprintf(buf, sizeof(buf), fmt, ap);
	__builtin_va_end(ap);

	console_write(buf);
}

/*
 * Initialise logging. Called after the TSC frequency is known so timestamps are
 * real rather than raw cycle counts.
 */
void klog_init(void)
{
	boot_tsc = rdtsc();
	tsc_khz = cpu_features.tsc_khz;
	klog_initialised = true;

	for (int i = 0; i < 5; i++)
		klog_counts[i] = 0;
}
