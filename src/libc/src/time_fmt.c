/*
 * time_fmt.c — UTC calendar conversion and formatting.
 *
 * The kernel gives Unix seconds. This file turns them into broken-down UTC
 * and back, and formats them. There is no timezone database, so localtime()
 * is gmtime() and tm_isdst is always 0.
 */
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static const char *const weekday_name[7] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};
static const char *const weekday_long[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
    "Saturday"
};
static const char *const month_name[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};
static const char *const month_long[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

/* Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant). */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;

    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

/* Inverse of days_from_civil. */
static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int year = (int)yoe + (int)era * 400;
    unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    unsigned mp = (5u * doy + 2u) / 153u;
    unsigned day = doy - (153u * mp + 2u) / 5u + 1u;
    unsigned month = (mp < 10u) ? mp + 3u : mp - 9u;

    *y = year + (month <= 2u);
    *m = month;
    *d = day;
}

struct tm *gmtime_r(const time_t *t, struct tm *out)
{
    if (!t || !out)
        return NULL;

    int64_t secs = (int64_t)*t;
    int64_t days = secs / 86400;
    int64_t rem = secs % 86400;

    if (rem < 0) {
        rem += 86400;
        days--;
    }

    int year;
    unsigned month, day;

    civil_from_days(days, &year, &month, &day);

    out->tm_year = year - 1900;
    out->tm_mon = (int)month - 1;
    out->tm_mday = (int)day;
    out->tm_hour = (int)(rem / 3600);
    out->tm_min = (int)((rem % 3600) / 60);
    out->tm_sec = (int)(rem % 60);
    out->tm_wday = (int)((days + 4) % 7);
    if (out->tm_wday < 0)
        out->tm_wday += 7;
    out->tm_yday = (int)(days - days_from_civil(year, 1, 1));
    out->tm_isdst = 0;
    return out;
}

struct tm *localtime_r(const time_t *t, struct tm *out)
{
    /* No timezone database: local time is UTC. */
    return gmtime_r(t, out);
}

static struct tm static_tm;

struct tm *gmtime(const time_t *t)
{
    return gmtime_r(t, &static_tm);
}

struct tm *localtime(const time_t *t)
{
    return localtime_r(t, &static_tm);
}

time_t mktime(struct tm *tm)
{
    int year = tm->tm_year + 1900;
    int mon = tm->tm_mon;

    year += mon / 12;
    mon %= 12;
    if (mon < 0) {
        mon += 12;
        year--;
    }

    int64_t days = days_from_civil(year, (unsigned)mon + 1, 1) +
                   (tm->tm_mday - 1);
    int64_t secs = days * 86400 + (int64_t)tm->tm_hour * 3600 +
                   (int64_t)tm->tm_min * 60 + tm->tm_sec;
    time_t t = (time_t)secs;
    struct tm norm;

    /* Normalise the caller's structure, as mktime is allowed to. */
    if (gmtime_r(&t, &norm))
        *tm = norm;
    return t;
}

double difftime(time_t t1, time_t t0)
{
    return (double)t1 - (double)t0;
}

char *asctime_r(const struct tm *tm, char *buf)
{
    if (!tm || !buf)
        return NULL;

    snprintf(buf, 26, "%s %s %2d %02d:%02d:%02d %4d\n",
             weekday_name[tm->tm_wday % 7],
             month_name[((tm->tm_mon % 12) + 12) % 12],
             tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec,
             tm->tm_year + 1900);
    return buf;
}

static char static_asctime[32];

char *asctime(const struct tm *tm)
{
    return asctime_r(tm, static_asctime);
}

char *ctime_r(const time_t *t, char *buf)
{
    struct tm tm;

    if (!t || !buf || !gmtime_r(t, &tm))
        return NULL;
    return asctime_r(&tm, buf);
}

static char static_ctime[32];

char *ctime(const time_t *t)
{
    return ctime_r(t, static_ctime);
}

struct out {
    char *s;
    size_t max;
    size_t len;
    int ok;
};

static void out_put(struct out *o, const char *s, size_t n)
{
    if (!o->ok)
        return;
    if (o->len + n >= o->max) {
        o->ok = 0;
        return;
    }
    memcpy(o->s + o->len, s, n);
    o->len += n;
}

static void out_str(struct out *o, const char *s)
{
    out_put(o, s, strlen(s));
}

static void out_num(struct out *o, int value, int width, char pad)
{
    char tmp[16];
    int n = snprintf(tmp, sizeof(tmp), "%d", value);

    if (n <= 0)
        return;

    if (n < width) {
        char padding[16];
        int count = width - n;

        if (count > (int)sizeof(padding))
            count = (int)sizeof(padding);
        memset(padding, pad, (size_t)count);
        out_put(o, padding, (size_t)count);
    }
    out_put(o, tmp, (size_t)n);
}

size_t strftime(char *s, size_t max, const char *format, const struct tm *tm)
{
    struct out o = { s, max, 0, 1 };

    if (!s || max == 0 || !format || !tm)
        return 0;

    for (const char *p = format; *p; p++) {
        if (*p != '%') {
            out_put(&o, p, 1);
            continue;
        }
        p++;
        if (!*p)
            break;

        int hour12 = tm->tm_hour % 12;

        if (hour12 == 0)
            hour12 = 12;

        switch (*p) {
        case '%': out_put(&o, "%", 1); break;
        case 'a': out_str(&o, weekday_name[tm->tm_wday % 7]); break;
        case 'A': out_str(&o, weekday_long[tm->tm_wday % 7]); break;
        case 'b':
        case 'h': out_str(&o, month_name[((tm->tm_mon % 12) + 12) % 12]); break;
        case 'B': out_str(&o, month_long[((tm->tm_mon % 12) + 12) % 12]); break;
        case 'C': out_num(&o, (tm->tm_year + 1900) / 100, 2, '0'); break;
        case 'c':
            out_str(&o, weekday_name[tm->tm_wday % 7]);
            out_put(&o, " ", 1);
            out_str(&o, month_name[((tm->tm_mon % 12) + 12) % 12]);
            out_put(&o, " ", 1);
            out_num(&o, tm->tm_mday, 2, ' ');
            out_put(&o, " ", 1);
            out_num(&o, tm->tm_hour, 2, '0');
            out_put(&o, ":", 1);
            out_num(&o, tm->tm_min, 2, '0');
            out_put(&o, ":", 1);
            out_num(&o, tm->tm_sec, 2, '0');
            out_put(&o, " ", 1);
            out_num(&o, tm->tm_year + 1900, 4, '0');
            break;
        case 'd': out_num(&o, tm->tm_mday, 2, '0'); break;
        case 'e': out_num(&o, tm->tm_mday, 2, ' '); break;
        case 'F':
            out_num(&o, tm->tm_year + 1900, 4, '0');
            out_put(&o, "-", 1);
            out_num(&o, tm->tm_mon + 1, 2, '0');
            out_put(&o, "-", 1);
            out_num(&o, tm->tm_mday, 2, '0');
            break;
        case 'H': out_num(&o, tm->tm_hour, 2, '0'); break;
        case 'I': out_num(&o, hour12, 2, '0'); break;
        case 'j': out_num(&o, tm->tm_yday + 1, 3, '0'); break;
        case 'm': out_num(&o, tm->tm_mon + 1, 2, '0'); break;
        case 'M': out_num(&o, tm->tm_min, 2, '0'); break;
        case 'n': out_put(&o, "\n", 1); break;
        case 'p': out_str(&o, tm->tm_hour < 12 ? "AM" : "PM"); break;
        case 'r':
            out_num(&o, hour12, 2, '0');
            out_put(&o, ":", 1);
            out_num(&o, tm->tm_min, 2, '0');
            out_put(&o, ":", 1);
            out_num(&o, tm->tm_sec, 2, '0');
            out_put(&o, " ", 1);
            out_str(&o, tm->tm_hour < 12 ? "AM" : "PM");
            break;
        case 'R':
            out_num(&o, tm->tm_hour, 2, '0');
            out_put(&o, ":", 1);
            out_num(&o, tm->tm_min, 2, '0');
            break;
        case 'S': out_num(&o, tm->tm_sec, 2, '0'); break;
        case 't': out_put(&o, "\t", 1); break;
        case 'T':
            out_num(&o, tm->tm_hour, 2, '0');
            out_put(&o, ":", 1);
            out_num(&o, tm->tm_min, 2, '0');
            out_put(&o, ":", 1);
            out_num(&o, tm->tm_sec, 2, '0');
            break;
        case 'u': out_num(&o, tm->tm_wday == 0 ? 7 : tm->tm_wday, 1, '0'); break;
        case 'w': out_num(&o, tm->tm_wday % 7, 1, '0'); break;
        case 'x':
            out_num(&o, tm->tm_mon + 1, 2, '0');
            out_put(&o, "/", 1);
            out_num(&o, tm->tm_mday, 2, '0');
            out_put(&o, "/", 1);
            out_num(&o, (tm->tm_year + 1900) % 100, 2, '0');
            break;
        case 'X':
            out_num(&o, tm->tm_hour, 2, '0');
            out_put(&o, ":", 1);
            out_num(&o, tm->tm_min, 2, '0');
            out_put(&o, ":", 1);
            out_num(&o, tm->tm_sec, 2, '0');
            break;
        case 'y': out_num(&o, (tm->tm_year + 1900) % 100, 2, '0'); break;
        case 'Y': out_num(&o, tm->tm_year + 1900, 4, '0'); break;
        case 'z': out_str(&o, "+0000"); break;
        case 'Z': out_str(&o, "UTC"); break;
        default:
            /* Unknown conversion: emit it literally, as several libcs do. */
            out_put(&o, "%", 1);
            out_put(&o, p, 1);
            break;
        }
    }

    if (!o.ok)
        return 0;
    s[o.len] = '\0';
    return o.len;
}
