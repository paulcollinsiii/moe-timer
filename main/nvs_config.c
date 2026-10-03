#include "nvs_config.h"

#include <stdio.h>
#include <string.h>

#include "config_validate.h" /* CFG_BOUND_OTA_* — setter length caps */
#include "hal_nvs.h"
#ifndef NATIVE
#include "nvs.h"
#endif
#include "nvs_defaults.h"

/* An owner whose credentials.local.h still defines one of the old
   WiFi/MQTT fallbacks gets a build-time nudge: these macros are no longer
   read by anything (nvs_defaults.h no longer seeds them). Plain #warning
   becomes a hard error under this build's -Werror (GCC promotes #warning
   via -Werror=cpp), so this uses #pragma message instead, which -Werror
   does not elevate. This check lives here as plain C, not in
   nvs_defaults.h as part of the header: that header is included by every
   TU that touches a default, so a #pragma in it fires once per including
   TU. Putting the check in this .c file instead means it fires once per
   build, this being the one TU nvs_defaults.h's own include of
   credentials.local.h is guaranteed to have already run in, by the time
   the preprocessor reaches this line. */
#if defined(NVS_DEFAULT_WIFI_SSID) || defined(NVS_DEFAULT_WIFI_PASS) || defined(NVS_DEFAULT_MQTT_URI) || \
    defined(NVS_DEFAULT_MQTT_USER) || defined(NVS_DEFAULT_MQTT_PASS)
#pragma message \
    "credentials.local.h still defines a WiFi/MQTT NVS_DEFAULT_* macro; it is no longer read. Enter WiFi and MQTT on the device in setup mode instead."
#endif

/* ---- u16 helpers ---- */

static esp_err_t get_u16_with_default(const char *key, uint16_t *out, uint16_t default_val) {
    *out = default_val; /* safe value on any error path */
    esp_err_t ret = hal_nvs_read_u16(key, out);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        *out = default_val;
        return ESP_OK;
    }
    return ret;
}

static esp_err_t get_u32_with_default(const char *key, uint32_t *out, uint32_t default_val) {
    *out = default_val; /* safe value on any error path */
    esp_err_t ret = hal_nvs_read_u32(key, out);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        *out = default_val;
        return ESP_OK;
    }
    return ret;
}

static esp_err_t init_u16_if_missing(const char *key, uint16_t default_val) {
    uint16_t tmp;
    esp_err_t ret = hal_nvs_read_u16(key, &tmp);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return hal_nvs_write_u16(key, default_val);
    }
    return ESP_OK; /* already set — do not overwrite */
}

/* ---- u16 accessors ---- */

esp_err_t nvs_config_get_weekday_min(uint16_t *out) {
    return get_u16_with_default(NVS_KEY_WEEKDAY_MIN, out, NVS_DEFAULT_WEEKDAY_MIN);
}

esp_err_t nvs_config_set_weekday_min(uint16_t val) {
    return hal_nvs_write_u16(NVS_KEY_WEEKDAY_MIN, val);
}

esp_err_t nvs_config_get_weekend_min(uint16_t *out) {
    return get_u16_with_default(NVS_KEY_WEEKEND_MIN, out, NVS_DEFAULT_WEEKEND_MIN);
}

esp_err_t nvs_config_set_weekend_min(uint16_t val) {
    return hal_nvs_write_u16(NVS_KEY_WEEKEND_MIN, val);
}

esp_err_t nvs_config_get_holiday_min(uint16_t *out) {
    return get_u16_with_default(NVS_KEY_HOLIDAY_MIN, out, NVS_DEFAULT_HOLIDAY_MIN);
}

esp_err_t nvs_config_set_holiday_min(uint16_t val) {
    return hal_nvs_write_u16(NVS_KEY_HOLIDAY_MIN, val);
}

esp_err_t nvs_config_get_summer_min(uint16_t *out) {
    return get_u16_with_default(NVS_KEY_SUMMER_MIN, out, NVS_DEFAULT_SUMMER_MIN);
}

esp_err_t nvs_config_set_summer_min(uint16_t val) {
    return hal_nvs_write_u16(NVS_KEY_SUMMER_MIN, val);
}

/* The four chore_free_* minute keys, the paired sibling of the four
   allocations above. Nothing seeds these — an absent key reads its
   compile-time 0 through get_u16_with_default, exactly as
   schedule_get_chore_free_sec() already relies on. */
esp_err_t nvs_config_get_chore_free_wd(uint16_t *out) {
    return get_u16_with_default(NVS_KEY_CHORE_FREE_WD, out, NVS_DEFAULT_CHORE_FREE_WD);
}

esp_err_t nvs_config_set_chore_free_wd(uint16_t val) {
    return hal_nvs_write_u16(NVS_KEY_CHORE_FREE_WD, val);
}

esp_err_t nvs_config_get_chore_free_we(uint16_t *out) {
    return get_u16_with_default(NVS_KEY_CHORE_FREE_WE, out, NVS_DEFAULT_CHORE_FREE_WE);
}

esp_err_t nvs_config_set_chore_free_we(uint16_t val) {
    return hal_nvs_write_u16(NVS_KEY_CHORE_FREE_WE, val);
}

esp_err_t nvs_config_get_chore_free_hol(uint16_t *out) {
    return get_u16_with_default(NVS_KEY_CHORE_FREE_HOL, out, NVS_DEFAULT_CHORE_FREE_HOL);
}

esp_err_t nvs_config_set_chore_free_hol(uint16_t val) {
    return hal_nvs_write_u16(NVS_KEY_CHORE_FREE_HOL, val);
}

esp_err_t nvs_config_get_chore_free_sum(uint16_t *out) {
    return get_u16_with_default(NVS_KEY_CHORE_FREE_SUM, out, NVS_DEFAULT_CHORE_FREE_SUM);
}

esp_err_t nvs_config_set_chore_free_sum(uint16_t val) {
    return hal_nvs_write_u16(NVS_KEY_CHORE_FREE_SUM, val);
}

/* ---- string accessors ---- */

static esp_err_t get_str_empty_default(const char *key, char *buf, size_t len) {
    esp_err_t ret = hal_nvs_read_str(key, buf, &len);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        buf[0] = '\0';
        return ESP_OK;
    }
    return ret;
}

static esp_err_t get_str_with_default(const char *key, char *buf, size_t len, const char *def) {
    size_t rlen = len;
    esp_err_t ret = hal_nvs_read_str(key, buf, &rlen);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        snprintf(buf, len, "%s", def);
        return ESP_OK;
    }
    return ret;
}

/* Reject rather than truncate. Both current callers validate length first
   (ha_config_set against the field's `hi`, config_apply against the same
   bound), but ota_flow.c will be the third writer of the state keys and
   is not on that path — and a silently truncated URL or target version is
   worse than a failed write: a half-written ota_target never matches, so
   the retry budget would never converge. */
static esp_err_t write_str_bounded(const char *key, const char *val, size_t cap) {
    if (val == NULL || strlen(val) >= cap)
        return ESP_ERR_INVALID_SIZE;
    return hal_nvs_write_str(key, val);
}

esp_err_t nvs_config_get_mqtt_uri(char *buf, size_t len) {
    return get_str_empty_default(NVS_KEY_MQTT_URI, buf, len);
}

esp_err_t nvs_config_set_mqtt_uri(const char *uri) {
    return hal_nvs_write_str(NVS_KEY_MQTT_URI, uri);
}

esp_err_t nvs_config_get_mqtt_user(char *buf, size_t len) {
    return get_str_empty_default(NVS_KEY_MQTT_USER, buf, len);
}

esp_err_t nvs_config_set_mqtt_user(const char *user) {
    return hal_nvs_write_str(NVS_KEY_MQTT_USER, user);
}

esp_err_t nvs_config_get_mqtt_pass(char *buf, size_t len) {
    return get_str_empty_default(NVS_KEY_MQTT_PASS, buf, len);
}

esp_err_t nvs_config_set_mqtt_pass(const char *pass) {
    return hal_nvs_write_str(NVS_KEY_MQTT_PASS, pass);
}

esp_err_t nvs_config_get_wifi_ssid(char *buf, size_t len) {
    return get_str_empty_default(NVS_KEY_WIFI_SSID, buf, len);
}

esp_err_t nvs_config_set_wifi_ssid(const char *ssid) {
    return hal_nvs_write_str(NVS_KEY_WIFI_SSID, ssid);
}

esp_err_t nvs_config_get_wifi_pass(char *buf, size_t len) {
    return get_str_empty_default(NVS_KEY_WIFI_PASS, buf, len);
}

esp_err_t nvs_config_set_wifi_pass(const char *pass) {
    return hal_nvs_write_str(NVS_KEY_WIFI_PASS, pass);
}

/* ---- blob accessor ---- */

/* buf is NOT null-terminated; callers must use *len and null-terminate before
 * string operations (e.g. buf[*len] = '\0'). */
esp_err_t nvs_config_get_holidays(char *buf, size_t *len) {
    return hal_nvs_read_blob(NVS_KEY_HOLIDAYS, buf, len);
}

esp_err_t nvs_config_set_holidays(const char *blob, size_t len) {
    return hal_nvs_write_blob(NVS_KEY_HOLIDAYS, blob, len);
}

/* ---- HA config-in keys (phase 2) ---- */

esp_err_t nvs_config_get_tz(char *buf, size_t len) {
    return get_str_with_default(NVS_KEY_TZ, buf, len, NVS_DEFAULT_TZ);
}
esp_err_t nvs_config_set_tz(const char *tz) {
    return hal_nvs_write_str(NVS_KEY_TZ, tz);
}

esp_err_t nvs_config_get_quiet_start(uint16_t *out) {
    return get_u16_with_default(NVS_KEY_QUIET_START, out, NVS_DEFAULT_QUIET_START);
}
esp_err_t nvs_config_set_quiet_start(uint16_t hhmm) {
    return hal_nvs_write_u16(NVS_KEY_QUIET_START, hhmm);
}
esp_err_t nvs_config_get_quiet_end(uint16_t *out) {
    return get_u16_with_default(NVS_KEY_QUIET_END, out, NVS_DEFAULT_QUIET_END);
}
esp_err_t nvs_config_set_quiet_end(uint16_t hhmm) {
    return hal_nvs_write_u16(NVS_KEY_QUIET_END, hhmm);
}

esp_err_t nvs_config_get_break_interval_min(uint16_t *out) {
    return get_u16_with_default(NVS_KEY_BREAK_INT, out, NVS_DEFAULT_BREAK_INTERVAL_MIN);
}
esp_err_t nvs_config_set_break_interval_min(uint16_t min) {
    return hal_nvs_write_u16(NVS_KEY_BREAK_INT, min);
}
esp_err_t nvs_config_get_break_duration_min(uint16_t *out) {
    return get_u16_with_default(NVS_KEY_BREAK_DUR, out, NVS_DEFAULT_BREAK_DURATION_MIN);
}
esp_err_t nvs_config_set_break_duration_min(uint16_t min) {
    return hal_nvs_write_u16(NVS_KEY_BREAK_DUR, min);
}

esp_err_t nvs_config_get_bedtime(uint16_t *out) {
    return get_u16_with_default(NVS_KEY_BEDTIME, out, NVS_DEFAULT_BEDTIME);
}
esp_err_t nvs_config_set_bedtime(uint16_t hhmm) {
    return hal_nvs_write_u16(NVS_KEY_BEDTIME, hhmm);
}

esp_err_t nvs_config_get_tone_expiry(uint16_t *out) {
    return get_u16_with_default(NVS_KEY_TONE_EXPIRY, out, NVS_DEFAULT_TONE_EXPIRY);
}
esp_err_t nvs_config_set_tone_expiry(uint16_t id) {
    return hal_nvs_write_u16(NVS_KEY_TONE_EXPIRY, id);
}
esp_err_t nvs_config_get_tone_break(uint16_t *out) {
    return get_u16_with_default(NVS_KEY_TONE_BREAK, out, NVS_DEFAULT_TONE_BREAK);
}
esp_err_t nvs_config_set_tone_break(uint16_t id) {
    return hal_nvs_write_u16(NVS_KEY_TONE_BREAK, id);
}
esp_err_t nvs_config_get_tone_bed(uint16_t *out) {
    return get_u16_with_default(NVS_KEY_TONE_BED, out, NVS_DEFAULT_TONE_BED);
}
esp_err_t nvs_config_set_tone_bed(uint16_t id) {
    return hal_nvs_write_u16(NVS_KEY_TONE_BED, id);
}

esp_err_t nvs_config_get_alert_volume(uint16_t *out) {
    return get_u16_with_default(NVS_KEY_ALERT_VOL, out, NVS_DEFAULT_ALERT_VOLUME);
}
esp_err_t nvs_config_set_alert_volume(uint16_t pct) {
    return hal_nvs_write_u16(NVS_KEY_ALERT_VOL, pct);
}

esp_err_t nvs_config_get_summer_start(char *buf, size_t len) {
    return get_str_with_default(NVS_KEY_SUMMER_START, buf, len, NVS_DEFAULT_SUMMER_START);
}
esp_err_t nvs_config_set_summer_start(const char *date) {
    return hal_nvs_write_str(NVS_KEY_SUMMER_START, date);
}
esp_err_t nvs_config_get_school_start(char *buf, size_t len) {
    return get_str_with_default(NVS_KEY_SCHOOL_START, buf, len, NVS_DEFAULT_SCHOOL_START);
}
esp_err_t nvs_config_set_school_start(const char *date) {
    return hal_nvs_write_str(NVS_KEY_SCHOOL_START, date);
}
esp_err_t nvs_config_get_school_end(char *buf, size_t len) {
    return get_str_with_default(NVS_KEY_SCHOOL_END, buf, len, NVS_DEFAULT_SCHOOL_END);
}
esp_err_t nvs_config_set_school_end(const char *date) {
    return hal_nvs_write_str(NVS_KEY_SCHOOL_END, date);
}

esp_err_t nvs_config_get_dev_name(char *buf, size_t len) {
    return get_str_empty_default(NVS_KEY_DEV_NAME, buf, len);
}
esp_err_t nvs_config_set_dev_name(const char *name) {
    return hal_nvs_write_str(NVS_KEY_DEV_NAME, name);
}

esp_err_t nvs_config_get_cfg_ver(char *buf, size_t len) {
    return get_str_empty_default(NVS_KEY_CFG_VER, buf, len);
}
esp_err_t nvs_config_set_cfg_ver(const char *ver) {
    return hal_nvs_write_str(NVS_KEY_CFG_VER, ver);
}

esp_err_t nvs_config_get_cmd_id(char *buf, size_t len) {
    return get_str_empty_default(NVS_KEY_CMD_ID, buf, len);
}
esp_err_t nvs_config_set_cmd_id(const char *id) {
    /* Bounded to cmd_apply.c's dedup buffer. An id longer than that stores
       fine but can never be read back, so the apply-once compare fails
       every window and a retained `grant` re-applies forever. */
    return write_str_bounded(NVS_KEY_CMD_ID, id, CFG_BOUND_CMD_ID_MAX);
}

/* ---- OTA ----
   Lazy defaults, no registry row: these must survive a defaults reseed
   (see the NVS_DEFAULT_OTA_URL comment in nvs_defaults.h). */

esp_err_t nvs_config_get_ota_url(char *buf, size_t len) {
    return get_str_with_default(NVS_KEY_OTA_URL, buf, len, NVS_DEFAULT_OTA_URL);
}
esp_err_t nvs_config_set_ota_url(const char *url) {
    return write_str_bounded(NVS_KEY_OTA_URL, url, CFG_BOUND_OTA_URL_MAX);
}
esp_err_t nvs_config_get_ota_on_sync(uint16_t *out) {
    return get_u16_with_default(NVS_KEY_OTA_ON_SYNC, out, NVS_DEFAULT_OTA_ON_SYNC);
}
esp_err_t nvs_config_set_ota_on_sync(uint16_t on) {
    return hal_nvs_write_u16(NVS_KEY_OTA_ON_SYNC, on ? 1 : 0);
}

esp_err_t nvs_config_get_ota_result(char *buf, size_t len) {
    return get_str_empty_default(NVS_KEY_OTA_RESULT, buf, len);
}
esp_err_t nvs_config_set_ota_result(const char *reason) {
    return write_str_bounded(NVS_KEY_OTA_RESULT, reason, CFG_BOUND_OTA_RESULT_MAX);
}
esp_err_t nvs_config_get_ota_target(char *buf, size_t len) {
    return get_str_empty_default(NVS_KEY_OTA_TARGET, buf, len);
}
esp_err_t nvs_config_set_ota_target(const char *ver) {
    return write_str_bounded(NVS_KEY_OTA_TARGET, ver, CFG_BOUND_OTA_TARGET_MAX);
}
esp_err_t nvs_config_get_ota_fails(uint16_t *out) {
    return get_u16_with_default(NVS_KEY_OTA_FAILS, out, 0);
}
esp_err_t nvs_config_set_ota_fails(uint16_t fails) {
    return hal_nvs_write_u16(NVS_KEY_OTA_FAILS, fails);
}
esp_err_t nvs_config_get_ota_dl_ms(uint32_t *out) {
    return get_u32_with_default(NVS_KEY_OTA_DL_MS, out, 0);
}
esp_err_t nvs_config_set_ota_dl_ms(uint32_t ms) {
    return hal_nvs_write_u32(NVS_KEY_OTA_DL_MS, ms);
}
esp_err_t nvs_config_get_ota_pend_ver(char *buf, size_t len) {
    return get_str_empty_default(NVS_KEY_OTA_PEND_VER, buf, len);
}
esp_err_t nvs_config_set_ota_pend_ver(const char *ver) {
    return write_str_bounded(NVS_KEY_OTA_PEND_VER, ver, CFG_BOUND_OTA_TARGET_MAX);
}

esp_err_t nvs_config_get_timer_defs(nvs_timer_defs_blob_t *out) {
    size_t len = sizeof(*out);
    esp_err_t ret = hal_nvs_read_blob(NVS_KEY_TIMER_DEFS, out, &len);
    if (ret != ESP_OK)
        return ret;
    if (len != sizeof(*out) || out->version != TIMER_DEFS_BLOB_VERSION)
        return ESP_ERR_INVALID_VERSION;
    return ESP_OK;
}
esp_err_t nvs_config_set_timer_defs(const nvs_timer_defs_blob_t *defs) {
    return hal_nvs_write_blob(NVS_KEY_TIMER_DEFS, defs, sizeof(*defs));
}

/* ---- timer snapshot (crash recovery) ---- */

esp_err_t nvs_config_save_timer_snapshot(const timer_snapshot_t *snap) {
    return hal_nvs_write_blob(NVS_KEY_TIMER_SNAP, snap, sizeof(*snap));
}

esp_err_t nvs_config_load_timer_snapshot(timer_snapshot_t *out) {
    size_t len = sizeof(*out);
    esp_err_t ret = hal_nvs_read_blob(NVS_KEY_TIMER_SNAP, out, &len);
    if (ret != ESP_OK)
        return ret;
    /* Size or version drift after a firmware update = stale layout */
    if (len != sizeof(*out) || out->version != TIMER_SNAPSHOT_VERSION)
        return ESP_ERR_INVALID_VERSION;
    return ESP_OK;
}

/* ---- init defaults ---- */

/* Only the u16 allocations are folded now — wifi_ssid/wifi_pass/mqtt_uri/
   mqtt_user/mqtt_pass were the only string rows, and build-time
   credentials are gone (see the on-device setup mode). Fold order =
   registry row order (see nvs_defaults.h — order is load-bearing). Never
   returns 0 (would collide with blank NVS). */
uint16_t nvs_config_defaults_fingerprint(void) {
    uint32_t fp = NVS_DEFAULTS_VERSION;
#define FOLD_U16(key, def) fp = fp * 31u + (def);
    /* registry macro from nvs_defaults.h — cppcheck runs without -I */
    // cppcheck-suppress unknownMacro
    NVS_SEEDED_U16S(FOLD_U16)
#undef FOLD_U16
    uint16_t out = (uint16_t)(fp ^ (fp >> 16));
    return (out == 0) ? 1 : out;
}

static esp_err_t reseed_all_defaults(void) {
    esp_err_t ret;

#define SEED_U16(key, def)                               \
    if ((ret = hal_nvs_write_u16(key, (def))) != ESP_OK) \
        return ret;
    /* registry macro from nvs_defaults.h — cppcheck runs without -I */
    // cppcheck-suppress unknownMacro
    NVS_SEEDED_U16S(SEED_U16)
#undef SEED_U16
    ret = hal_nvs_write_blob(NVS_KEY_HOLIDAYS, NVS_DEFAULT_HOLIDAYS, strlen(NVS_DEFAULT_HOLIDAYS));
    if (ret != ESP_OK)
        return ret;
    /* A reseed reverts every HA-managed key to the Kconfig default, so the
       applied HA config version no longer describes what's stored: clear
       it and the retained HA config re-applies on the next window (HA stays
       source-of-truth across a reflash that bumps the fingerprint). */
    ret = hal_nvs_write_str(NVS_KEY_CFG_VER, "");
    if (ret != ESP_OK)
        return ret;
    /* Stamp last: a power cut mid-reseed re-runs the whole reseed */
    return hal_nvs_write_u16(NVS_KEY_DEFAULTS_VER, nvs_config_defaults_fingerprint());
}

esp_err_t nvs_config_init_defaults(void) {
    /* Fingerprint stamp: when the compile-time defaults change (menuconfig
       allocation values or an NVS_DEFAULTS_VERSION bump), overwrite
       everything — no erase-flash needed. A missing stamp also reseeds
       (covers devices seeded before the stamp existed). */
    uint16_t ver = 0;
    esp_err_t vret = hal_nvs_read_u16(NVS_KEY_DEFAULTS_VER, &ver);
    if (vret == ESP_ERR_NVS_NOT_FOUND || (vret == ESP_OK && ver != nvs_config_defaults_fingerprint())) {
        return reseed_all_defaults();
    }
    if (vret != ESP_OK)
        return vret;

    /* Stamp current: fill in only missing keys (repairs partial state
       without touching runtime-set values). Same registry as the reseed. */
    esp_err_t ret;

#define INIT_U16(key, def)                                 \
    if ((ret = init_u16_if_missing(key, (def))) != ESP_OK) \
        return ret;
    /* registry macro from nvs_defaults.h — cppcheck runs without -I */
    // cppcheck-suppress unknownMacro
    NVS_SEEDED_U16S(INIT_U16)
#undef INIT_U16

    /* Holiday blob: write only if missing */
    {
        uint8_t blob_check[1];
        size_t blob_len = sizeof(blob_check);
        if (hal_nvs_read_blob(NVS_KEY_HOLIDAYS, blob_check, &blob_len) == ESP_ERR_NVS_NOT_FOUND) {
            const char *defaults = NVS_DEFAULT_HOLIDAYS;
            size_t def_len = strlen(defaults);
            ret = hal_nvs_write_blob(NVS_KEY_HOLIDAYS, defaults, def_len);
            if (ret != ESP_OK)
                return ret;
        }
    }

    return ESP_OK;
}
