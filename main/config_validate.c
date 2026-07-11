/* Shared config validators — pure, host-tested. */
#include "config_validate.h"

#include <string.h>
#include <time.h>

bool config_is_iso_date(const char *s) {
    if (s == NULL || strlen(s) != 10)
        return false;
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) {
            if (s[i] != '-')
                return false;
        } else if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    int year = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
    int month = (s[5] - '0') * 10 + (s[6] - '0');
    int day = (s[8] - '0') * 10 + (s[9] - '0');
    /* Calendar validity (month lengths, leap years) via a mktime round
       trip: mktime normalizes an impossible date (Feb 31 -> Mar 3), so if
       it changed any field the date was invalid. Noon dodges DST-gap
       midnights; libc owns all the corner cases. */
    struct tm tm = {0};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = 12;
    tm.tm_isdst = -1;
    if (mktime(&tm) == (time_t)-1)
        return false;
    return tm.tm_year == year - 1900 && tm.tm_mon == month - 1 && tm.tm_mday == day;
}
