/* Shared config validators — pure, host-tested. */
#include "config_validate.h"

#include <ctype.h>
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

bool config_is_https_url(const char *s) {
    static const char scheme[] = "https://";
    if (s == NULL)
        return false;
    /* Scheme compare is case-insensitive: "HTTPS://host" is a legal URL
       that works in a browser, so refusing it here would read as a bug.
       The loop stops at the first mismatch, so a short string (including
       "") fails on its NUL rather than running off the end -- '\0' equals
       no scheme character. cppcheck models the loop as always running to
       completion and so reads a short literal at a call site as an overrun;
       an explicit NUL test does not convince it either, so per schedule.c
       this is suppressed rather than contorted. */
    for (size_t i = 0; i < sizeof(scheme) - 1; i++)
        // cppcheck-suppress arrayIndexOutOfBounds
        if (tolower((unsigned char)s[i]) != scheme[i])
            return false;
    const char *rest = s + sizeof(scheme) - 1;
    if (*rest == '\0')
        return false; /* scheme but no host */
    /* No spaces or control chars (not legal in a URL, and they are what
       would corrupt the request line), and no quote/backslash (they would
       corrupt the cfg-state JSON this value is republished in). */
    for (const char *p = rest; *p != '\0'; p++)
        if ((unsigned char)*p <= ' ' || *p == '"' || *p == '\\')
            return false;
    return true;
}

bool config_is_ota_url(const char *s) {
    if (s == NULL)
        return false;
    return (*s == '\0') || config_is_https_url(s);
}
