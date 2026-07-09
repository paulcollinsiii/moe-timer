/* Apply an HA config document to NVS. Pure over nvs_config; host-tested. */
#include "config_apply.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "nvs_config.h"
#include "timer.h" /* TIMER_EXTRA_SLOTS */

/* ---- error accumulator: builds the ack "errors" list ---- */

typedef struct {
    char errors[192];
    int count;
} err_acc_t;

static void err_add(err_acc_t *e, const char *field) {
    int n = snprintf(e->errors + strlen(e->errors), sizeof(e->errors) - strlen(e->errors), "%s\"%s\"",
                     e->count ? "," : "", field);
    if (n > 0)
        e->count++;
}

/* ---- validators ---- */

static bool is_iso_date(const char *s) {
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

/* Apply a bounded integer field to a u16 setter; records the field name on
   an out-of-range or wrong-typed value. Absent field = no-op. */
static void apply_u16(const cJSON *root, const char *field, int lo, int hi, esp_err_t (*setter)(uint16_t),
                      err_acc_t *e) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, field);
    if (item == NULL)
        return;
    if (!cJSON_IsNumber(item) || item->valuedouble < lo || item->valuedouble > hi) {
        err_add(e, field);
        return;
    }
    setter((uint16_t)item->valueint);
}

static void apply_date(const cJSON *root, const char *field, esp_err_t (*setter)(const char *), err_acc_t *e) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, field);
    if (item == NULL)
        return;
    if (!cJSON_IsString(item) || !is_iso_date(item->valuestring)) {
        err_add(e, field);
        return;
    }
    setter(item->valuestring);
}

static void apply_str(const cJSON *root, const char *field, size_t maxlen, esp_err_t (*setter)(const char *),
                      err_acc_t *e) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, field);
    if (item == NULL)
        return;
    if (!cJSON_IsString(item) || strlen(item->valuestring) >= maxlen) {
        err_add(e, field);
        return;
    }
    setter(item->valuestring);
}

#define HOLIDAY_BLOB_CAP 512

static void apply_holidays(const cJSON *root, err_acc_t *e) {
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "holidays");
    if (arr == NULL)
        return;
    if (!cJSON_IsArray(arr)) {
        err_add(e, "holidays");
        return;
    }
    char blob[HOLIDAY_BLOB_CAP];
    size_t pos = 0;
    bool had_bad = false;
    const cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        if (!cJSON_IsString(item) || !is_iso_date(item->valuestring)) {
            had_bad = true;
            continue;
        }
        if (pos + 11 > sizeof(blob))
            break; /* cap: whole dates only, never a partial trailing line */
        memcpy(blob + pos, item->valuestring, 10);
        blob[pos + 10] = '\n';
        pos += 11;
    }
    if (had_bad)
        err_add(e, "holidays");
    nvs_config_set_holidays(blob, pos);
}

static void apply_timers(const cJSON *root, err_acc_t *e) {
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "timers");
    if (arr == NULL)
        return;
    if (!cJSON_IsArray(arr)) {
        err_add(e, "timers");
        return;
    }
    nvs_timer_defs_blob_t defs;
    memset(&defs, 0, sizeof(defs));
    defs.version = TIMER_DEFS_BLOB_VERSION;
    int slot = 0;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, arr) {
        if (slot >= TIMER_EXTRA_SLOTS)
            break;
        if (!cJSON_IsObject(entry)) {
            err_add(e, "timers");
            return;
        }
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(entry, "name");
        const cJSON *min = cJSON_GetObjectItemCaseSensitive(entry, "min");
        const cJSON *reload = cJSON_GetObjectItemCaseSensitive(entry, "reload");
        if (name == NULL) {
            slot++; /* {} = disabled slot */
            continue;
        }
        if (!cJSON_IsString(name) || strlen(name->valuestring) >= sizeof(defs.defs[slot].name)) {
            err_add(e, "timers");
            return; /* whole array rejected — a half-written table is worse */
        }
        if (min == NULL || !cJSON_IsNumber(min) || min->valuedouble < 1 || min->valuedouble > 1440) {
            err_add(e, "timers");
            return;
        }
        snprintf(defs.defs[slot].name, sizeof(defs.defs[slot].name), "%s", name->valuestring);
        defs.defs[slot].min = min->valueint;
        defs.defs[slot].reload = (reload != NULL && cJSON_IsTrue(reload)) ? 1 : 0;
        slot++;
    }
    nvs_config_set_timer_defs(&defs);
}

config_result_t config_apply(const char *json, char *ack, size_t ack_len) {
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        snprintf(ack, ack_len, "{\"ok\":false,\"err\":\"parse\"}");
        return CONFIG_INVALID;
    }

    const cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "ver");
    /* ver may arrive as a JSON number or string; normalize to text */
    char ver_str[24] = {0};
    if (cJSON_IsString(ver) && ver->valuestring[0] != '\0') {
        snprintf(ver_str, sizeof(ver_str), "%s", ver->valuestring);
    } else if (cJSON_IsNumber(ver)) {
        snprintf(ver_str, sizeof(ver_str), "%lld", (long long)ver->valuedouble);
    } else {
        snprintf(ack, ack_len, "{\"ok\":false,\"err\":\"no_ver\"}");
        cJSON_Delete(root);
        return CONFIG_INVALID;
    }

    char stored[24];
    nvs_config_get_cfg_ver(stored, sizeof(stored));
    if (strcmp(stored, ver_str) == 0) {
        snprintf(ack, ack_len, "{\"ver\":\"%s\",\"ok\":true,\"skipped\":true}", ver_str);
        cJSON_Delete(root);
        return CONFIG_SKIPPED;
    }

    err_acc_t e = {.errors = {0}, .count = 0};

    apply_str(root, "name", 32, nvs_config_set_dev_name, &e);
    apply_str(root, "tz", 48, nvs_config_set_tz, &e);
    apply_u16(root, "weekday_min", 1, 1440, nvs_config_set_weekday_min, &e);
    apply_u16(root, "weekend_min", 1, 1440, nvs_config_set_weekend_min, &e);
    apply_u16(root, "holiday_min", 1, 1440, nvs_config_set_holiday_min, &e);
    apply_u16(root, "summer_min", 1, 1440, nvs_config_set_summer_min, &e);
    apply_u16(root, "quiet_start", 0, 2359, nvs_config_set_quiet_start, &e);
    apply_u16(root, "quiet_end", 0, 2359, nvs_config_set_quiet_end, &e);
    apply_u16(root, "break_interval_min", 0, 480, nvs_config_set_break_interval_min, &e);
    apply_u16(root, "break_duration_min", 1, 120, nvs_config_set_break_duration_min, &e);
    apply_date(root, "summer_start", nvs_config_set_summer_start, &e);
    apply_date(root, "school_start", nvs_config_set_school_start, &e);
    apply_date(root, "school_end", nvs_config_set_school_end, &e);
    apply_holidays(root, &e);
    apply_timers(root, &e);

    /* Record the version last: a power cut mid-apply leaves cfg_ver stale,
       so the (idempotent) document simply re-applies next window. */
    nvs_config_set_cfg_ver(ver_str);

    if (e.count > 0) {
        snprintf(ack, ack_len, "{\"ver\":\"%s\",\"ok\":false,\"errors\":[%s]}", ver_str, e.errors);
    } else {
        snprintf(ack, ack_len, "{\"ver\":\"%s\",\"ok\":true}", ver_str);
    }
    cJSON_Delete(root);
    return CONFIG_APPLIED;
}
