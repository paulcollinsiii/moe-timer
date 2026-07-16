#include "nvs_config.h"

#include <stdio.h>
#include <string.h>

#include "hal_nvs.h"
#ifndef NATIVE
#include "nvs.h"
#endif
#include "nvs_defaults.h"

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
    return hal_nvs_write_str(NVS_KEY_CMD_ID, id);
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

static uint32_t fold_str(uint32_t fp, const char *s) {
    for (; s != NULL && *s != '\0'; s++)
        fp = fp * 31u + (unsigned char)*s;
    return fp;
}

/* Every seeded default is mixed in — the credential strings too, or
   setting a WiFi/MQTT default after the first seed silently never takes
   (the key already exists as "" and init-if-missing skips it). Fold order
   = registry row order (see nvs_defaults.h — order is load-bearing).
   Never returns 0 (would collide with blank NVS). */
uint16_t nvs_config_defaults_fingerprint(void) {
    uint32_t fp = NVS_DEFAULTS_VERSION;
#define FOLD_U16(key, def) fp = fp * 31u + (def);
    /* registry macro from nvs_defaults.h — cppcheck runs without -I */
    // cppcheck-suppress unknownMacro
    NVS_SEEDED_U16S(FOLD_U16)
#undef FOLD_U16
#define FOLD_STR(key, def) fp = fold_str(fp, (def));
    /* registry macro from nvs_defaults.h — cppcheck runs without -I */
    // cppcheck-suppress unknownMacro
    NVS_SEEDED_STRS(FOLD_STR)
#undef FOLD_STR
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
#define SEED_STR(key, def)                               \
    if ((ret = hal_nvs_write_str(key, (def))) != ESP_OK) \
        return ret;
    /* registry macro from nvs_defaults.h — cppcheck runs without -I */
    // cppcheck-suppress unknownMacro
    NVS_SEEDED_STRS(SEED_STR)
#undef SEED_STR
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
    char tmp[64];
    size_t tmp_len;

#define INIT_U16(key, def)                                 \
    if ((ret = init_u16_if_missing(key, (def))) != ESP_OK) \
        return ret;
    /* registry macro from nvs_defaults.h — cppcheck runs without -I */
    // cppcheck-suppress unknownMacro
    NVS_SEEDED_U16S(INIT_U16)
#undef INIT_U16
#define INIT_STR(key, def)                                               \
    tmp_len = sizeof(tmp);                                               \
    if (hal_nvs_read_str(key, tmp, &tmp_len) == ESP_ERR_NVS_NOT_FOUND) { \
        if ((ret = hal_nvs_write_str(key, (def))) != ESP_OK)             \
            return ret;                                                  \
    }
    /* registry macro from nvs_defaults.h — cppcheck runs without -I */
    // cppcheck-suppress unknownMacro
    NVS_SEEDED_STRS(INIT_STR)
#undef INIT_STR

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
