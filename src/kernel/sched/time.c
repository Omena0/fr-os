/*
 * time.c — monotonic and wall-clock time.
 *
 * Wall clock: the MC146818-compatible CMOS RTC. It is the only time-of-day
 * source on a PC, and QEMU programs it from the host clock at machine reset
 * (base=utc by default), so it is a real Unix timestamp rather than an
 * arbitrary epoch. The value is read once, after the boot-time port I/O is
 * safe, and the rest of the kernel works from a monotonic counter.
 *
 * Monotonic: the TSC when the CPU feature probe measured its frequency. The
 * measurement is done against the PIT (see cpu_features), so the timebase is
 * traceable to the PIT's 1.193182 MHz oscillator; the TSC only supplies the
 * resolution. TSC is not advertised invariant on this QEMU, but it is a
 * constant-rate counter under both TCG and KVM, which is what the conversion
 * assumes. If no frequency was measured, PIT ticks (10 ms) remain the source.
 */
#include <types.h>
#include <io.h>
#include <stdbool.h>
#include <cpu_features.h>
#include <sched.h>
#include <klog.h>
#include <ktime.h>

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

/* CMOS register numbers. */
#define RTC_SECONDS  0x00
#define RTC_MINUTES  0x02
#define RTC_HOURS    0x04
#define RTC_DAY      0x07
#define RTC_MONTH    0x08
#define RTC_YEAR     0x09
#define RTC_STATUS_A 0x0A
#define RTC_STATUS_B 0x0B
#define RTC_CENTURY  0x32

struct rtc_fields {
    u8 sec, min, hour, day, month, year, century, status_b;
};

static u64 tsc_base;
static u64 pit_base_ns;
static u64 realtime_base_ns;
static bool rtc_valid;

static u8 cmos_read(u8 reg)
{
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

/* Bit 7 of status A is the update-in-progress flag: the registers must not be
 * read while it is set or a field can be torn between two updates. */
static bool rtc_quiescent(void)
{
    for (unsigned i = 0; i < 100000; i++)
        if (!(cmos_read(RTC_STATUS_A) & 0x80))
            return true;
    return false;
}

static bool rtc_snapshot(struct rtc_fields *f)
{
    if (!rtc_quiescent())
        return false;
    f->sec = cmos_read(RTC_SECONDS);
    f->min = cmos_read(RTC_MINUTES);
    f->hour = cmos_read(RTC_HOURS);
    f->day = cmos_read(RTC_DAY);
    f->month = cmos_read(RTC_MONTH);
    f->year = cmos_read(RTC_YEAR);
    f->century = cmos_read(RTC_CENTURY);
    f->status_b = cmos_read(RTC_STATUS_B);
    return true;
}

static bool rtc_same(const struct rtc_fields *a, const struct rtc_fields *b)
{
    return a->sec == b->sec && a->min == b->min && a->hour == b->hour &&
           a->day == b->day && a->month == b->month && a->year == b->year &&
           a->century == b->century;
}

static u8 bcd_to_bin(u8 v)
{
    return (u8)((v & 0x0F) + ((v >> 4) * 10));
}

/* Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant). */
static s64 days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;

    return (s64)era * 146097 + (s64)doe - 719468;
}

static bool rtc_unix_seconds(u64 *out)
{
    struct rtc_fields a, b;

    /* Two identical reads mean no update happened in between. */
    for (unsigned attempt = 0; attempt < 8; attempt++) {
        if (!rtc_snapshot(&a) || !rtc_snapshot(&b))
            return false;
        if (rtc_same(&a, &b))
            break;
        if (attempt == 7)
            return false;
    }

    bool binary = (a.status_b & 0x04) != 0;
    bool hour24 = (a.status_b & 0x02) != 0;
    u8 sec = a.sec, min = a.min, hour = a.hour;
    u8 day = a.day, month = a.month, year = a.year, century = a.century;

    if (!hour24) {
        bool pm = (hour & 0x80) != 0;
        hour &= 0x7F;
        if (!binary)
            hour = bcd_to_bin(hour);
        if (pm && hour < 12)
            hour = (u8)(hour + 12);
        if (!pm && hour == 12)
            hour = 0;
    } else if (!binary) {
        hour = bcd_to_bin(hour);
    }

    if (!binary) {
        sec = bcd_to_bin(sec);
        min = bcd_to_bin(min);
        day = bcd_to_bin(day);
        month = bcd_to_bin(month);
        year = bcd_to_bin(year);
        if (century != 0x00 && century != 0xFF)
            century = bcd_to_bin(century);
    }

    int full_year;
    if (century != 0x00 && century != 0xFF)
        full_year = (int)century * 100 + year;
    else
        full_year = (year < 70 ? 2000 : 1900) + year;

    if (month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || min > 59 || sec > 60)
        return false;

    u64 days = (u64)days_from_civil(full_year, month, day);
    *out = days * 86400ull + (u64)hour * 3600ull + (u64)min * 60ull + sec;
    return true;
}

void ktime_init(void)
{
    u64 unix_seconds;

    tsc_base = rdtsc();
    pit_base_ns = sched_now_ns();

    if (rtc_unix_seconds(&unix_seconds)) {
        realtime_base_ns = unix_seconds * 1000000000ull;
        rtc_valid = true;
    }

    klog_emit(KLOG_INFO, "time",
              "rtc %llu unix s (%s), clocksource tsc %llu kHz",
              (unsigned long long)unix_seconds, rtc_valid ? "utc" : "unreadable",
              (unsigned long long)cpu_features.tsc_khz);
}

u64 ktime_mono_ns(void)
{
    u64 khz = cpu_features.tsc_khz;

    if (!khz)
        return sched_now_ns() - pit_base_ns;

    u64 cycles = rdtsc() - tsc_base;
    u64 ms = cycles / khz;
    u64 rem = cycles % khz;

    return ms * 1000000ull + (rem * 1000000ull) / khz;
}

u64 ktime_realtime_ns(void)
{
    u64 mono = ktime_mono_ns();

    return rtc_valid ? realtime_base_ns + mono : mono;
}
