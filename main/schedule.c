#include "schedule.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hal_nvs.h"
#include "nvs_defaults.h"

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
    char summer_start[16], school_start[16], school_end[16];
    read_date_key("summer_start", NVS_DEFAULT_SUMMER_START, summer_start, sizeof(summer_start));
    read_date_key("school_start", NVS_DEFAULT_SCHOOL_START, school_start, sizeof(school_start));
    read_date_key("school_end", NVS_DEFAULT_SCHOOL_END, school_end, sizeof(school_end));
    if (strcmp(date_str, school_end) > 0)
        return true;
    return strcmp(date_str, summer_start) >= 0 && strcmp(date_str, school_start) < 0;
}

day_type_t schedule_get_day_type(time_t now) {
    struct tm tm_local;
    localtime_r(&now, &tm_local);

    char date_str[40];
    snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d", tm_local.tm_year + 1900, tm_local.tm_mon + 1,
             tm_local.tm_mday);

    char blob[512];
    size_t blob_len = sizeof(blob);
    esp_err_t ret = hal_nvs_read_blob("holidays", blob, &blob_len);
    const char *holiday_data = (ret == ESP_OK) ? blob : NVS_DEFAULT_HOLIDAYS;
    size_t holiday_len = (ret == ESP_OK) ? blob_len : strlen(NVS_DEFAULT_HOLIDAYS);
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

    switch (day_type) {
        case DAY_WEEKEND:
            key = "weekend_min";
            default_min = NVS_DEFAULT_WEEKEND_MIN;
            break;
        case DAY_HOLIDAY:
            key = "holiday_min";
            default_min = NVS_DEFAULT_HOLIDAY_MIN;
            break;
        case DAY_SUMMER:
            key = "summer_min";
            default_min = NVS_DEFAULT_SUMMER_MIN;
            break;
        case DAY_WEEKDAY:
        default:
            key = "weekday_min";
            default_min = NVS_DEFAULT_WEEKDAY_MIN;
            break;
    }

    uint16_t minutes = default_min;
    hal_nvs_read_u16(key, &minutes);
    return (uint32_t)minutes * 60u;
}
