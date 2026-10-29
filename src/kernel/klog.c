/*
 * klog.c — structured kernel logging.
 *
 * Output goes to the console, which fans out to serial and framebuffer. The
 * prefix carries level, CPU and timestamp; ordering is the interesting part
 * in a multicore system.
 *
 * Formatting uses a fixed stack buffer. A heap allocation here would be a bug
 * because klog is called from allocation-free contexts (early boot, panic,
 * interrupt handlers).
 */

#include <klog.h>
#include <console.h>
#include <cpu_features.h>
#include <io.h>
#include <kprintf.h>
#include <kstring.h>
#include <percpu.h>

klog_level_t klog_runtime_level = KLOG_DEBUG;

static uint64_t klog_counts[5];
static bool klog_initialised;

/* Set in klog_init(); before that, timestamps read as zero. */
static uint64_t boot_tsc;
static uint64_t tsc_khz;

static const char level_names[] = { 'D', 'I', 'W', 'E', 'F' };

#define LOG_SGR_RESET             "\033[0m"
#define LOG_SGR_GRAY              "\033[90m"

#define LOG_SGR_TIME_SECONDS      "\033[94m"
#define LOG_SGR_TIME_MILLISECONDS "\033[96m"
#define LOG_SGR_CPU               "\033[95m"
#define LOG_SGR_SUBSYSTEM          "\033[92m"

#define LOG_SGR_LEVEL_DEBUG        "\033[34m"
#define LOG_SGR_LEVEL_INFO         "\033[32m"
#define LOG_SGR_LEVEL_WARN         "\033[33m"
#define LOG_SGR_LEVEL_ERROR        "\033[31m"
#define LOG_SGR_LEVEL_FATAL        "\033[97;41m"
#define LOG_SGR_LEVEL_UNKNOWN      "\033[37m"

#define LOG_SGR_MSG_DEBUG          "\033[38;5;250m"
#define LOG_SGR_MSG_INFO         "\033[37m"
#define LOG_SGR_MSG_WARN           "\033[33m"
#define LOG_SGR_MSG_ERROR          "\033[31m"
#define LOG_SGR_MSG_FATAL          "\033[97;41m"

static const char *log_level_color(klog_level_t level)
{
    switch (level) {
    case KLOG_DEBUG: return LOG_SGR_LEVEL_DEBUG;
    case KLOG_INFO:  return LOG_SGR_LEVEL_INFO;
    case KLOG_WARN:  return LOG_SGR_LEVEL_WARN;
    case KLOG_ERROR: return LOG_SGR_LEVEL_ERROR;
    case KLOG_FATAL: return LOG_SGR_LEVEL_FATAL;
    default:         return LOG_SGR_LEVEL_UNKNOWN;
    }
}

static const char *log_message_color(klog_level_t level)
{
    switch (level) {
    case KLOG_DEBUG:  return LOG_SGR_MSG_DEBUG;
    case KLOG_INFO:  return LOG_SGR_MSG_INFO;
    case KLOG_WARN:  return LOG_SGR_MSG_WARN;
    case KLOG_ERROR: return LOG_SGR_MSG_ERROR;
    case KLOG_FATAL: return LOG_SGR_MSG_FATAL;
    default:         return LOG_SGR_RESET;
    }
}

/* Per-subsystem log level overrides. */
#define KLOG_MAX_SUBSYSTEMS 32
static struct {
    const char *name;
    klog_level_t level;
    bool set;
} klog_overrides[KLOG_MAX_SUBSYSTEMS];

/* Get the effective log level for a subsystem (override or global). */
klog_level_t klog_effective_level(const char *subsystem)
{
    for (int i = 0; i < KLOG_MAX_SUBSYSTEMS; i++) {
        if (klog_overrides[i].set &&
            klog_overrides[i].name && subsystem &&
            !strcmp(klog_overrides[i].name, subsystem))
            return klog_overrides[i].level;
    }
    return klog_runtime_level;
}

void klog_set_subsystem_level(const char *name, klog_level_t level)
{
    if (!name || (int)level < 0 || (int)level > KLOG_FATAL)
        return;

    /* Find existing or free slot. */
    int free = -1;
    for (int i = 0; i < KLOG_MAX_SUBSYSTEMS; i++) {
        if (klog_overrides[i].set) {
            if (!strcmp(klog_overrides[i].name, name)) {
                klog_overrides[i].level = level;
                return;
            }
        } else if (free < 0) {
            free = i;
        }
    }
    if (free >= 0) {
        klog_overrides[free].name = name;
        klog_overrides[free].level = level;
        klog_overrides[free].set = true;
    }
}

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
    ksize_t len;

    if ((int)level < 0 || (int)level > KLOG_FATAL)
        return;
    if (level < klog_runtime_level)
        return;

    __atomic_fetch_add(&klog_counts[level], 1, __ATOMIC_RELAXED);

    __builtin_va_start(ap, fmt);
    kvsnprintf(msg, sizeof(msg), fmt, ap);
    __builtin_va_end(ap);

    /* kvsnprintf() returns nothing, so the length has to be measured. msg is
     * always NUL-terminated when it fits, and the only case that matters is
     * the truncated one, where it is exactly sizeof(msg) - 1. */
    len = 0;
    while (len + 1 < sizeof(msg) && msg[len])
        len++;

    uint32_t cpu = percpu_num_cpus() ? this_cpu_id() : 0;
    uint64_t now = 0;

    /*
     * Read the frequency live rather than from a snapshot. It is filled in by
     * cpu_features_init(), which runs *after* klog_init(), so a cached copy
     * would be zero for the whole boot and every line would read
     * [    0.000 ...] -- a timestamp that looks real and says nothing.
     */
    if (klog_initialised && cpu_features.tsc_khz) {
        /* Convert TSC ticks to milliseconds. Integer arithmetic is used
         * deliberately: this runs in interrupt context where dividing by a
         * float would require enabling the FPU. */
        uint64_t ticks = rdtsc() - boot_tsc;
        now = ticks * 1000 / cpu_features.tsc_khz;
    }

    /*
     * Exactly one trailing newline, always.
     *
     * This used to format the prefix and hand `line` straight to
     * console_write(), with no newline of its own. Every message that
     * happened not to end in one ran straight into the next line's output --
     * "syscall entry installed at 0xffffffff80010cb8[    0.000 cpu0 sched/I]
     * scheduler up on cpu 0" -- and any message long enough to fill the buffer
     * lost its newline to truncation, so the corruption was worst exactly when
     * the message mattered most.
     *
     * The room for the newline is reserved before formatting, not after: a
     * message that fills `line` must still be terminated.
     */
    /* Drop a trailing newline from the message so the one added below is the
     * only one, and so a message cannot push itself into the prefix's room. */
    while (len > 0 && (msg[len - 1] == '\n' || msg[len - 1] == '\r'))
        msg[--len] = '\0';

    const char level_char =
        (unsigned)level < sizeof(level_names)
            ? level_names[(unsigned)level]
            : '?';

    ksize_t n = ksnprintf(line, sizeof(line) - 2,
        LOG_SGR_GRAY "["
        LOG_SGR_TIME_SECONDS "%5llu"
        LOG_SGR_GRAY "."
        LOG_SGR_TIME_MILLISECONDS "%03llu"
        LOG_SGR_GRAY " cpu"
        LOG_SGR_CPU "%-2u"
        LOG_SGR_GRAY " "
        LOG_SGR_SUBSYSTEM "%-10.10s"
        LOG_SGR_GRAY " /"
        "%s%c"
        LOG_SGR_RESET LOG_SGR_GRAY "] "
        "%s%s" LOG_SGR_RESET "\n",
        (unsigned long long)(now / 1000),
        (unsigned long long)(now % 1000),
        cpu,
        subsystem ? subsystem : "?",
        log_level_color(level),
        level_char,
        log_message_color(level),
        msg
    );

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
 *
 * The comment above used to be a lie about the call order: main() calls this
 * *before* cpu_features_init(), so the frequency cached here was always 0 and
 * every timestamp in the log read [    0.000 ...] no matter how good the
 * calibration was. The frequency is now read live in klog_emit() instead of
 * snapshotted, so the order stops mattering -- which is the actual fix, because
 * a cached copy of a value that is filled in later is a race with the
 * initialisation sequence, not a scheduling detail.
 *
 * boot_tsc is still taken here, and that is right: it is the origin, and taking
 * it at the first klog call makes timestamps relative to the start of kernel
 * logging rather than to whenever the frequency happened to be discovered.
 */
void klog_init(void)
{
    boot_tsc = rdtsc();
    tsc_khz = 0;
    klog_initialised = true;

    for (int i = 0; i < 5; i++)
        klog_counts[i] = 0;
}
