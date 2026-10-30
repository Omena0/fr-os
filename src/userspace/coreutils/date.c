#include <stdio.h>
#include <time.h>

/* Days since 1970-01-01 to civil date, after Howard Hinnant's algorithm. */
static void civil_from_days(long z, int *yy, int *mm, int *dd)
{
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp + (mp < 10 ? 3 : -9);

    *yy = (int)(y + (m <= 2));
    *mm = (int)m;
    *dd = (int)d;
}

int main(void)
{
    struct timespec ts;
    long secs;
    int y, m, d;

    if (clock_gettime(CLOCK_REALTIME, &ts) < 0) {
        perror("date");
        return 1;
    }
    secs = (long)ts.tv_sec;
    civil_from_days(secs / 86400, &y, &m, &d);
    printf("%04d-%02d-%02d %02d:%02d:%02d UTC\n", y, m, d,
           (int)((secs / 3600) % 24), (int)((secs / 60) % 60),
           (int)(secs % 60));
    return 0;
}
