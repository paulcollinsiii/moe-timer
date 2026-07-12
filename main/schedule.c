#include "schedule.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "date_fmt.h"
#include "hal_nvs.h"
#include "nvs_defaults.h"

/* Wake-scoped cache. The device deep-sleeps between wakes, so a boot IS a
   wake: values read once stay valid for the whole awake window. The only
   in-wake config mutation is an HA edit applied during the network window;
   the orchestrator calls schedule_cache_invalidate() after the window so
   later reads see the edit. Sized for the largest consumer (512 B holiday
   blob) — trades a little .bss for one flash read per key per wake. */
#define SCHED_DAY_TYPES 4

static struct {
    bool blob_loaded;
    bool blob_in_nvs;
    size_t blob_len;
    char blob[512];
    bool dates_loaded;
    char summer_start[16];
    char school_start[16];
    char school_end[16];
    bool alloc_loaded[SCHED_DAY_TYPES];
    uint16_t alloc_min[SCHED_DAY_TYPES];
} s_cache;

void schedule_cache_invalidate(void) {
    memset(&s_cache, 0, sizeof(s_cache));
}

bool schedule_is_holiday(const char *date_str, const char *blob, size_t blob_len) {
    size_t date_len = strlen(date_str);
    if (date_len != 10)
        return false;

    const char *p = blob;
    /* One-past-the-end is legal C; cppcheck's portability check flags any
       base+size it can trace to a concrete array — suppress, don't contort. */
    // cppcheck-suppress pointerOutOfBounds
    const char *end = blob + blob_len;
    while (p < end) {
        const char *nl = p;
        while (nl < end && *nl != '\n')
            nl++;
        const char *line_end = nl;
        if (line_end > p && *(line_end - 1) == '\r')
            line_end--;
        size_t line_len = (size_t)(line_end - p);
        if (line_len == date_len && memcmp(p, date_str, date_len) == 0) {
            return true;
        }
        p = nl + 1;
    }
    return false;
}

/* School-year boundary from NVS (HA config-in, phase 2) with the
   compile-time default as fallback — read raw like the holiday blob so
   schedule.c stays dependency-light (host tests link only the mock NVS). */
static void read_date_key(const char *key, const char *def, char *buf, size_t len) {
    size_t rlen = len;
    if (hal_nvs_read_str(key, buf, &rlen) != ESP_OK || buf[0] == '\0') {
        snprintf(buf, len, "%s", def);
    }
}

bool schedule_is_summer(const char *date_str) {
    /* ISO dates compare lexicographically. Summer = the bounded window
       between school years (dates before SUMMER_START belong to the
       previous school year, not summer), plus everything after this
       year's last day (until next year's calendar is loaded). */
    if (!s_cache.dates_loaded) {
        read_date_key(NVS_KEY_SUMMER_START, NVS_DEFAULT_SUMMER_START, s_cache.summer_start,
                      sizeof(s_cache.summer_start));
        read_date_key(NVS_KEY_SCHOOL_START, NVS_DEFAULT_SCHOOL_START, s_cache.school_start,
                      sizeof(s_cache.school_start));
        read_date_key(NVS_KEY_SCHOOL_END, NVS_DEFAULT_SCHOOL_END, s_cache.school_end, sizeof(s_cache.school_end));
        s_cache.dates_loaded = true;
    }
    if (strcmp(date_str, s_cache.school_end) > 0)
        return true;
    return strcmp(date_str, s_cache.summer_start) >= 0 && strcmp(date_str, s_cache.school_start) < 0;
}

day_type_t schedule_get_day_type(time_t now) {
    struct tm tm_local;
    localtime_r(&now, &tm_local);

    char date_str[40];
    date_fmt_iso(date_str, sizeof(date_str), &tm_local);

    if (!s_cache.blob_loaded) {
        s_cache.blob_len = sizeof(s_cache.blob);
        s_cache.blob_in_nvs = hal_nvs_read_blob(NVS_KEY_HOLIDAYS, s_cache.blob, &s_cache.blob_len) == ESP_OK;
        s_cache.blob_loaded = true;
    }
    const char *holiday_data = s_cache.blob_in_nvs ? s_cache.blob : NVS_DEFAULT_HOLIDAYS;
    size_t holiday_len = s_cache.blob_in_nvs ? s_cache.blob_len : strlen(NVS_DEFAULT_HOLIDAYS);
    if (schedule_is_holiday(date_str, holiday_data, holiday_len)) {
        return DAY_HOLIDAY;
    }

    if (tm_local.tm_wday == 0 || tm_local.tm_wday == 6) {
        return DAY_WEEKEND;
    }

    /* Summer upgrades weekdays only (holiday/weekend checked above) */
    if (schedule_is_summer(date_str)) {
        return DAY_SUMMER;
    }

    return DAY_WEEKDAY;
}

uint32_t schedule_get_allocation_sec(day_type_t day_type) {
    const char *key;
    uint16_t default_min;
    unsigned idx;

    switch (day_type) {
        case DAY_WEEKEND:
            key = NVS_KEY_WEEKEND_MIN;
            default_min = NVS_DEFAULT_WEEKEND_MIN;
            idx = DAY_WEEKEND;
            break;
        case DAY_HOLIDAY:
            key = NVS_KEY_HOLIDAY_MIN;
            default_min = NVS_DEFAULT_HOLIDAY_MIN;
            idx = DAY_HOLIDAY;
            break;
        case DAY_SUMMER:
            key = NVS_KEY_SUMMER_MIN;
            default_min = NVS_DEFAULT_SUMMER_MIN;
            idx = DAY_SUMMER;
            break;
        case DAY_WEEKDAY:
        default:
            key = NVS_KEY_WEEKDAY_MIN;
            default_min = NVS_DEFAULT_WEEKDAY_MIN;
            idx = DAY_WEEKDAY;
            break;
    }

    if (!s_cache.alloc_loaded[idx]) {
        uint16_t minutes = default_min;
        hal_nvs_read_u16(key, &minutes);
        s_cache.alloc_min[idx] = minutes;
        s_cache.alloc_loaded[idx] = true;
    }
    return (uint32_t)s_cache.alloc_min[idx] * 60u;
}
