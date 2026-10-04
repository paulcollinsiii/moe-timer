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

/* Case-insensitive compare of s[0..len) against scheme[0..len), shared by
   every URL-scheme check below so this shape exists once. Scheme compare
   is case-insensitive: "HTTPS://host" is a legal URL that works in a
   browser, so refusing it here would read as a bug. The loop stops at
   the first mismatch, so a short s (including "") fails on its NUL
   rather than running off the end -- '\0' equals no scheme character.
   cppcheck models the loop as always running to completion and so reads
   a short literal at a call site as an overrun; an explicit NUL test
   does not convince it either, so per schedule.c this is suppressed
   rather than contorted. */
static bool scheme_prefix_eq(const char *s, const char *scheme, size_t len) {
    for (size_t i = 0; i < len; i++)
        // cppcheck-suppress arrayIndexOutOfBounds
        if (tolower((unsigned char)s[i]) != scheme[i])
            return false;
    return true;
}

/* The character rule every URL check below shares for what follows the
   scheme: no spaces or control chars (not legal in a URL, and they are
   what would corrupt the request line), and no quote/backslash (they
   would corrupt a hand-built JSON document the value might be
   republished in). */
static bool url_rest_is_clean(const char *rest) {
    for (const char *p = rest; *p != '\0'; p++)
        if ((unsigned char)*p <= ' ' || *p == '"' || *p == '\\')
            return false;
    return true;
}

bool config_is_https_url(const char *s) {
    static const char scheme[] = "https://";
    if (s == NULL)
        return false;
    if (!scheme_prefix_eq(s, scheme, sizeof(scheme) - 1))
        return false;
    const char *rest = s + sizeof(scheme) - 1;
    if (*rest == '\0')
        return false; /* scheme but no host */
    return url_rest_is_clean(rest);
}

/* The host grammar config_mqtt_uri_check() accepts: 1+ of [A-Za-z0-9.-].
   No '@' -- a URI carrying "user:pass@" would have its credential both
   logged (mqtt_ha.c's connect-log line) and echoed back by the form
   page's prefill, so userinfo has to fail the grammar rather than be
   accepted and stripped afterward. */
static bool is_mqtt_host_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
}

config_mqtt_uri_check_t config_mqtt_uri_check(const char *s) {
    static const char scheme_mqtts[] = "mqtts://";
    static const char scheme_mqtt[] = "mqtt://";
    if (s == NULL)
        return CONFIG_MQTT_URI_BAD_SCHEME;

    const char *rest;
    if (scheme_prefix_eq(s, scheme_mqtts, sizeof(scheme_mqtts) - 1))
        rest = s + sizeof(scheme_mqtts) - 1;
    else if (scheme_prefix_eq(s, scheme_mqtt, sizeof(scheme_mqtt) - 1))
        rest = s + sizeof(scheme_mqtt) - 1;
    else
        return CONFIG_MQTT_URI_BAD_SCHEME; /* neither scheme */

    const char *p = rest;
    while (is_mqtt_host_char(*p))
        p++;
    if (p == rest)
        return CONFIG_MQTT_URI_NO_HOST;

    if (*p == ':') {
        const char *port = ++p;
        while (*p >= '0' && *p <= '9')
            p++;
        size_t digits = (size_t)(p - port);
        if (digits == 0 || digits > 5)
            return CONFIG_MQTT_URI_BAD_PORT;
        int value = 0;
        for (size_t i = 0; i < digits; i++)
            value = value * 10 + (port[i] - '0');
        if (value < 1 || value > 65535)
            return CONFIG_MQTT_URI_BAD_PORT;
    }

    if (*p == '/')
        p++;

    return (*p == '\0') ? CONFIG_MQTT_URI_OK : CONFIG_MQTT_URI_BAD_CHAR;
}

bool config_is_mqtt_uri(const char *s) {
    return config_mqtt_uri_check(s) == CONFIG_MQTT_URI_OK;
}

bool config_is_ota_url(const char *s) {
    if (s == NULL)
        return false;
    return (*s == '\0') || config_is_https_url(s);
}

bool config_is_clean_str(const char *s) {
    if (s == NULL)
        return false;
    for (; *s != '\0'; s++)
        if (*s == '"' || *s == '\\' || (unsigned char)*s < 0x20)
            return false;
    return true;
}

bool config_is_valid_chore_free_min(uint16_t chore_free_min, uint16_t alloc_min) {
    /* MINUTES on both sides. The seconds pair (schedule_get_chore_free_sec
       / schedule_get_allocation_sec) answers a different question in a
       different unit; this one is for the config domain, where both values
       are minutes on their way into NVS.

       `<=`, not `<`, and the difference is the whole feature: equal means
       the day's whole allocation is handed over free, which is the
       per-day-type off switch (design 3.3). Tightening this to `<` would
       turn every operator who typed the same number twice into a config
       error and leave no way to disable the gate for one day type without
       inventing another key.

       False does NOT mean "reject" on its own — the two setters that share
       this rule do deliberately different things with it (the chore_free
       setter rejects; the allocation setter clamps the paired chore_free
       down to alloc_min). See the header before wiring a caller, because
       making both reject would block a parent lowering screen time on
       account of a chore setting they never touched.

       No bounds check here, on purpose. CFG_BOUND_CHORE_FREE_* is the
       field's own range and is enforced by whatever parses the field
       (config_apply.c's apply_u16, ha_config.c's number entity); this
       function answers only the cross-field question, so a caller that
       already range-checked does not get a second opinion on the range,
       and the M2 config-error gate can ask about a pair already sitting in
       NVS without a bound it never passed through mattering. Pinned by
       test_chore_free_predicate_does_not_range_check, which fails if a
       "helpful" ceiling check is added here — the (0, 0) case pins the
       same invariant on the alloc side. */
    return chore_free_min <= alloc_min;
}
