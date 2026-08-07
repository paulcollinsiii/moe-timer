/* Apply an HA config document to NVS. Pure over nvs_config; host-tested. */
#include "config_apply.h"

#include <stdio.h>
#include <string.h>

#include "bedtime.h" /* bedtime_hhmm_valid */
#include "cJSON.h"
#include "config_validate.h" /* config_is_iso_date */
#include "nvs_config.h"
#include "quiet_hours.h" /* quiet_hhmm_valid */
#include "timer.h"       /* TIMER_EXTRA_SLOTS */
#include "tones.h"       /* tones_names for the tone selects */

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

/* HHMM time-of-day field: real-time validity (hour<=23, minute<=59) via
   the field's `valid` rule, not a plain numeric range. Absent = no-op. */
static void apply_hhmm(const cJSON *root, const char *field, bool (*valid)(int), esp_err_t (*setter)(uint16_t),
                       err_acc_t *e) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, field);
    if (item == NULL)
        return;
    if (!cJSON_IsNumber(item) || !valid(item->valueint)) {
        err_add(e, field);
        return;
    }
    setter((uint16_t)item->valueint);
}

static void apply_date(const cJSON *root, const char *field, esp_err_t (*setter)(const char *), err_acc_t *e) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, field);
    if (item == NULL)
        return;
    if (!cJSON_IsString(item) || !config_is_iso_date(item->valuestring)) {
        err_add(e, field);
        return;
    }
    setter(item->valuestring);
}

/* Enum-as-option-string field (mirrors the HA select): the string must
   match one of `options` exactly; stored as its index. Absent = no-op. */
static void apply_enum(const cJSON *root, const char *field, const char *const *options, int n_options,
                       esp_err_t (*setter)(uint16_t), err_acc_t *e) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, field);
    if (item == NULL)
        return;
    if (cJSON_IsString(item)) {
        for (int i = 0; i < n_options; i++) {
            if (strcmp(item->valuestring, options[i]) == 0) {
                setter((uint16_t)i);
                return;
            }
        }
    }
    err_add(e, field);
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
        if (!cJSON_IsString(item) || !config_is_iso_date(item->valuestring)) {
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
    /* The stored table, for the optional-key rule below. A read failure
       (never written, or version drift) just means "no slot existed", which
       is the same answer as an empty table — so no error path is needed. */
    nvs_timer_defs_blob_t prev;
    bool have_prev = (nvs_config_get_timer_defs(&prev) == ESP_OK);

    nvs_timer_defs_blob_t defs;
    memset(&defs, 0, sizeof(defs));
    defs.version = TIMER_DEFS_BLOB_VERSION;
    int slot = 0;
    const cJSON *entry;
    /* One bad entry rejects the whole array, so the error has to name the
       entry: "timers" alone reads as "none of your timers applied" with
       no clue which one to fix, and an over-long name is the easy way to
       land here. */
    char where[16];
    cJSON_ArrayForEach(entry, arr) {
        if (slot >= TIMER_EXTRA_SLOTS)
            break;
        snprintf(where, sizeof(where), "timers[%d]", slot);
        if (!cJSON_IsObject(entry)) {
            err_add(e, where);
            return;
        }
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(entry, "name");
        const cJSON *min = cJSON_GetObjectItemCaseSensitive(entry, "min");
        const cJSON *reload = cJSON_GetObjectItemCaseSensitive(entry, "reload");
        const cJSON *brk = cJSON_GetObjectItemCaseSensitive(entry, "break");
        if (name == NULL) {
            slot++; /* {} = disabled slot */
            continue;
        }
        if (!cJSON_IsString(name) || strlen(name->valuestring) >= sizeof(defs.defs[slot].name)) {
            err_add(e, where);
            return; /* whole array rejected — a half-written table is worse */
        }
        if (min == NULL || !cJSON_IsNumber(min) || min->valuedouble < CFG_BOUND_TIMER_MIN_LO ||
            min->valuedouble > CFG_BOUND_TIMER_MIN_HI) {
            err_add(e, where);
            return;
        }
        snprintf(defs.defs[slot].name, sizeof(defs.defs[slot].name), "%s", name->valuestring);
        defs.defs[slot].min = min->valueint;
        /* Optional keys: ABSENT MEANS UNCHANGED for a slot that already has
           a definition, and false for one this document is defining for the
           first time.

           It used to mean false unconditionally, on the argument that a
           wrong `break: true` would let a screen activity run during (and
           drain) a break. That argument is right for a NEW slot and is kept
           for one — but it does not justify overriding a value the operator
           already set, and doing so was a live defect: `break` is settable
           from HA's per-timer switch yet is absent from the documented
           `timers` schema, so every application of a documentation-shaped
           document silently cleared it. The retained set/ command that would
           have restored it is consumed on apply (mqtt_ha.c), so nothing
           healed it. See BUG-6 in docs/planning/refactor.bugdiscoveries.md.

           Caveat worth knowing: "already has a definition" is by SLOT, not
           by name, so renaming a slot in the document carries its flags
           over. Repurposing slot 3 from a chore to a screen activity must
           therefore say `"break": false` explicitly rather than rely on
           omission. Stated here because the safe direction for this field is
           false, and this rule does not always pick it. */
        bool existed = have_prev && prev.defs[slot].name[0] != '\0';
        defs.defs[slot].reload = (reload != NULL) ? (cJSON_IsTrue(reload) ? 1 : 0)
                                 : existed        ? prev.defs[slot].reload
                                                  : 0;
        defs.defs[slot].break_eligible = (brk != NULL) ? (cJSON_IsTrue(brk) ? 1 : 0)
                                         : existed     ? prev.defs[slot].break_eligible
                                                       : 0;
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

    apply_str(root, "name", CFG_BOUND_NAME_MAX, nvs_config_set_dev_name, &e);
    apply_str(root, "tz", CFG_BOUND_TZ_MAX, nvs_config_set_tz, &e);
    apply_u16(root, "weekday_min", CFG_BOUND_ALLOC_LO, CFG_BOUND_ALLOC_HI, nvs_config_set_weekday_min, &e);
    apply_u16(root, "weekend_min", CFG_BOUND_ALLOC_LO, CFG_BOUND_ALLOC_HI, nvs_config_set_weekend_min, &e);
    apply_u16(root, "holiday_min", CFG_BOUND_ALLOC_LO, CFG_BOUND_ALLOC_HI, nvs_config_set_holiday_min, &e);
    apply_u16(root, "summer_min", CFG_BOUND_ALLOC_LO, CFG_BOUND_ALLOC_HI, nvs_config_set_summer_min, &e);
    apply_hhmm(root, "quiet_start", quiet_hhmm_valid, nvs_config_set_quiet_start, &e);
    apply_hhmm(root, "quiet_end", quiet_hhmm_valid, nvs_config_set_quiet_end, &e);
    apply_hhmm(root, "bedtime", bedtime_hhmm_valid, nvs_config_set_bedtime, &e);
    apply_u16(root, "break_interval_min", CFG_BOUND_BREAK_INT_LO, CFG_BOUND_BREAK_INT_HI,
              nvs_config_set_break_interval_min, &e);
    apply_u16(root, "break_duration_min", CFG_BOUND_BREAK_DUR_LO, CFG_BOUND_BREAK_DUR_HI,
              nvs_config_set_break_duration_min, &e);
    apply_enum(root, "tone_expiry", tones_names, TONE_COUNT, nvs_config_set_tone_expiry, &e);
    apply_enum(root, "tone_break", tones_names, TONE_COUNT, nvs_config_set_tone_break, &e);
    apply_enum(root, "tone_bed", tones_names, TONE_COUNT, nvs_config_set_tone_bed, &e);
    apply_u16(root, "alert_volume", 0, TONES_VOLUME_MAX, nvs_config_set_alert_volume, &e);
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
