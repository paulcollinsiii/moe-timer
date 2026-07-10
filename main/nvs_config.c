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
    return get_u16_with_default("weekday_min", out, NVS_DEFAULT_WEEKDAY_MIN);
}

esp_err_t nvs_config_set_weekday_min(uint16_t val) {
    return hal_nvs_write_u16("weekday_min", val);
}

esp_err_t nvs_config_get_weekend_min(uint16_t *out) {
    return get_u16_with_default("weekend_min", out, NVS_DEFAULT_WEEKEND_MIN);
}

esp_err_t nvs_config_set_weekend_min(uint16_t val) {
    return hal_nvs_write_u16("weekend_min", val);
}

esp_err_t nvs_config_get_holiday_min(uint16_t *out) {
    return get_u16_with_default("holiday_min", out, NVS_DEFAULT_HOLIDAY_MIN);
}

esp_err_t nvs_config_set_holiday_min(uint16_t val) {
    return hal_nvs_write_u16("holiday_min", val);
}

esp_err_t nvs_config_get_summer_min(uint16_t *out) {
    return get_u16_with_default("summer_min", out, NVS_DEFAULT_SUMMER_MIN);
}

esp_err_t nvs_config_set_summer_min(uint16_t val) {
    return hal_nvs_write_u16("summer_min", val);
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
    return get_str_empty_default("mqtt_uri", buf, len);
}

esp_err_t nvs_config_set_mqtt_uri(const char *uri) {
    return hal_nvs_write_str("mqtt_uri", uri);
}

esp_err_t nvs_config_get_mqtt_user(char *buf, size_t len) {
    return get_str_empty_default("mqtt_user", buf, len);
}

esp_err_t nvs_config_set_mqtt_user(const char *user) {
    return hal_nvs_write_str("mqtt_user", user);
}

esp_err_t nvs_config_get_mqtt_pass(char *buf, size_t len) {
    return get_str_empty_default("mqtt_pass", buf, len);
}

esp_err_t nvs_config_set_mqtt_pass(const char *pass) {
    return hal_nvs_write_str("mqtt_pass", pass);
}

esp_err_t nvs_config_get_wifi_ssid(char *buf, size_t len) {
    esp_err_t ret = hal_nvs_read_str("wifi_ssid", buf, &len);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        buf[0] = '\0';
        return ESP_OK;
    }
    return ret;
}

esp_err_t nvs_config_set_wifi_ssid(const char *ssid) {
    return hal_nvs_write_str("wifi_ssid", ssid);
}

esp_err_t nvs_config_get_wifi_pass(char *buf, size_t len) {
    esp_err_t ret = hal_nvs_read_str("wifi_pass", buf, &len);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        buf[0] = '\0';
        return ESP_OK;
    }
    return ret;
}

esp_err_t nvs_config_set_wifi_pass(const char *pass) {
    return hal_nvs_write_str("wifi_pass", pass);
}

/* ---- blob accessor ---- */

/* buf is NOT null-terminated; callers must use *len and null-terminate before
 * string operations (e.g. buf[*len] = '\0'). */
esp_err_t nvs_config_get_holidays(char *buf, size_t *len) {
    return hal_nvs_read_blob("holidays", buf, len);
}

esp_err_t nvs_config_set_holidays(const char *blob, size_t len) {
    return hal_nvs_write_blob("holidays", blob, len);
}

/* ---- HA config-in keys (phase 2) ---- */

esp_err_t nvs_config_get_tz(char *buf, size_t len) {
    return get_str_with_default("tz", buf, len, NVS_DEFAULT_TZ);
}
esp_err_t nvs_config_set_tz(const char *tz) {
    return hal_nvs_write_str("tz", tz);
}

esp_err_t nvs_config_get_quiet_start(uint16_t *out) {
    return get_u16_with_default("quiet_start", out, NVS_DEFAULT_QUIET_START);
}
esp_err_t nvs_config_set_quiet_start(uint16_t hhmm) {
    return hal_nvs_write_u16("quiet_start", hhmm);
}
esp_err_t nvs_config_get_quiet_end(uint16_t *out) {
    return get_u16_with_default("quiet_end", out, NVS_DEFAULT_QUIET_END);
}
esp_err_t nvs_config_set_quiet_end(uint16_t hhmm) {
    return hal_nvs_write_u16("quiet_end", hhmm);
}

esp_err_t nvs_config_get_break_interval_min(uint16_t *out) {
    return get_u16_with_default("break_int", out, NVS_DEFAULT_BREAK_INTERVAL_MIN);
}
esp_err_t nvs_config_set_break_interval_min(uint16_t min) {
    return hal_nvs_write_u16("break_int", min);
}
esp_err_t nvs_config_get_break_duration_min(uint16_t *out) {
    return get_u16_with_default("break_dur", out, NVS_DEFAULT_BREAK_DURATION_MIN);
}
esp_err_t nvs_config_set_break_duration_min(uint16_t min) {
    return hal_nvs_write_u16("break_dur", min);
}

esp_err_t nvs_config_get_summer_start(char *buf, size_t len) {
    return get_str_with_default("summer_start", buf, len, NVS_DEFAULT_SUMMER_START);
}
esp_err_t nvs_config_set_summer_start(const char *date) {
    return hal_nvs_write_str("summer_start", date);
}
esp_err_t nvs_config_get_school_start(char *buf, size_t len) {
    return get_str_with_default("school_start", buf, len, NVS_DEFAULT_SCHOOL_START);
}
esp_err_t nvs_config_set_school_start(const char *date) {
    return hal_nvs_write_str("school_start", date);
}
esp_err_t nvs_config_get_school_end(char *buf, size_t len) {
    return get_str_with_default("school_end", buf, len, NVS_DEFAULT_SCHOOL_END);
}
esp_err_t nvs_config_set_school_end(const char *date) {
    return hal_nvs_write_str("school_end", date);
}

esp_err_t nvs_config_get_dev_name(char *buf, size_t len) {
    return get_str_empty_default("dev_name", buf, len);
}
esp_err_t nvs_config_set_dev_name(const char *name) {
    return hal_nvs_write_str("dev_name", name);
}

esp_err_t nvs_config_get_cfg_ver(char *buf, size_t len) {
    return get_str_empty_default("cfg_ver", buf, len);
}
esp_err_t nvs_config_set_cfg_ver(const char *ver) {
    return hal_nvs_write_str("cfg_ver", ver);
}

esp_err_t nvs_config_get_cmd_id(char *buf, size_t len) {
    return get_str_empty_default("cmd_id", buf, len);
}
esp_err_t nvs_config_set_cmd_id(const char *id) {
    return hal_nvs_write_str("cmd_id", id);
}

esp_err_t nvs_config_get_timer_defs(nvs_timer_defs_blob_t *out) {
    size_t len = sizeof(*out);
    esp_err_t ret = hal_nvs_read_blob("timer_defs", out, &len);
    if (ret != ESP_OK)
        return ret;
    if (len != sizeof(*out) || out->version != TIMER_DEFS_BLOB_VERSION)
        return ESP_ERR_INVALID_VERSION;
    return ESP_OK;
}
esp_err_t nvs_config_set_timer_defs(const nvs_timer_defs_blob_t *defs) {
    return hal_nvs_write_blob("timer_defs", defs, sizeof(*defs));
}

/* ---- timer snapshot (crash recovery) ---- */

esp_err_t nvs_config_save_timer_snapshot(const timer_snapshot_t *snap) {
    return hal_nvs_write_blob("timer_snap", snap, sizeof(*snap));
}

esp_err_t nvs_config_load_timer_snapshot(timer_snapshot_t *out) {
    size_t len = sizeof(*out);
    esp_err_t ret = hal_nvs_read_blob("timer_snap", out, &len);
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

/* Pure, host-testable core of the fingerprint (the public function feeds it
   the compile-time defaults). Every seeded default is mixed in — the
   credential strings too, or setting a WiFi/MQTT default after the first
   seed silently never takes (the key already exists as "" and
   init-if-missing skips it). Never returns 0 (would collide with blank NVS). */
static uint16_t fingerprint_compute(uint32_t version, uint16_t wd, uint16_t we, uint16_t ho, uint16_t su,
                                    const char *ssid, const char *pass, const char *mqtt_uri, const char *mqtt_user,
                                    const char *mqtt_pass) {
    uint32_t fp = version;
    fp = fp * 31u + wd;
    fp = fp * 31u + we;
    fp = fp * 31u + ho;
    fp = fp * 31u + su;
    fp = fold_str(fp, ssid);
    fp = fold_str(fp, pass);
    fp = fold_str(fp, mqtt_uri);
    fp = fold_str(fp, mqtt_user);
    fp = fold_str(fp, mqtt_pass);
    uint16_t out = (uint16_t)(fp ^ (fp >> 16));
    return (out == 0) ? 1 : out;
}

uint16_t nvs_config_defaults_fingerprint(void) {
    return fingerprint_compute(NVS_DEFAULTS_VERSION, NVS_DEFAULT_WEEKDAY_MIN, NVS_DEFAULT_WEEKEND_MIN,
                               NVS_DEFAULT_HOLIDAY_MIN, NVS_DEFAULT_SUMMER_MIN, NVS_DEFAULT_WIFI_SSID,
                               NVS_DEFAULT_WIFI_PASS, NVS_DEFAULT_MQTT_URI, NVS_DEFAULT_MQTT_USER,
                               NVS_DEFAULT_MQTT_PASS);
}

static esp_err_t reseed_all_defaults(void) {
    esp_err_t ret;

    ret = hal_nvs_write_u16("weekday_min", NVS_DEFAULT_WEEKDAY_MIN);
    if (ret != ESP_OK)
        return ret;
    ret = hal_nvs_write_u16("weekend_min", NVS_DEFAULT_WEEKEND_MIN);
    if (ret != ESP_OK)
        return ret;
    ret = hal_nvs_write_u16("holiday_min", NVS_DEFAULT_HOLIDAY_MIN);
    if (ret != ESP_OK)
        return ret;
    ret = hal_nvs_write_u16("summer_min", NVS_DEFAULT_SUMMER_MIN);
    if (ret != ESP_OK)
        return ret;
    ret = hal_nvs_write_str("wifi_ssid", NVS_DEFAULT_WIFI_SSID);
    if (ret != ESP_OK)
        return ret;
    ret = hal_nvs_write_str("wifi_pass", NVS_DEFAULT_WIFI_PASS);
    if (ret != ESP_OK)
        return ret;
    ret = hal_nvs_write_str("mqtt_uri", NVS_DEFAULT_MQTT_URI);
    if (ret != ESP_OK)
        return ret;
    ret = hal_nvs_write_str("mqtt_user", NVS_DEFAULT_MQTT_USER);
    if (ret != ESP_OK)
        return ret;
    ret = hal_nvs_write_str("mqtt_pass", NVS_DEFAULT_MQTT_PASS);
    if (ret != ESP_OK)
        return ret;
    ret = hal_nvs_write_blob("holidays", NVS_DEFAULT_HOLIDAYS, strlen(NVS_DEFAULT_HOLIDAYS));
    if (ret != ESP_OK)
        return ret;
    /* A reseed reverts every HA-managed key to the Kconfig default, so the
       applied HA config version no longer describes what's stored: clear
       it and the retained HA config re-applies on the next window (HA stays
       source-of-truth across a reflash that bumps the fingerprint). */
    ret = hal_nvs_write_str("cfg_ver", "");
    if (ret != ESP_OK)
        return ret;
    /* Stamp last: a power cut mid-reseed re-runs the whole reseed */
    return hal_nvs_write_u16("defaults_ver", nvs_config_defaults_fingerprint());
}

esp_err_t nvs_config_init_defaults(void) {
    /* Fingerprint stamp: when the compile-time defaults change (menuconfig
       allocation values or an NVS_DEFAULTS_VERSION bump), overwrite
       everything — no erase-flash needed. A missing stamp also reseeds
       (covers devices seeded before the stamp existed). */
    uint16_t ver = 0;
    esp_err_t vret = hal_nvs_read_u16("defaults_ver", &ver);
    if (vret == ESP_ERR_NVS_NOT_FOUND || (vret == ESP_OK && ver != nvs_config_defaults_fingerprint())) {
        return reseed_all_defaults();
    }
    if (vret != ESP_OK)
        return vret;

    /* Stamp current: fill in only missing keys (repairs partial state
       without touching runtime-set values). */
    esp_err_t ret;

    ret = init_u16_if_missing("weekday_min", NVS_DEFAULT_WEEKDAY_MIN);
    if (ret != ESP_OK)
        return ret;

    ret = init_u16_if_missing("weekend_min", NVS_DEFAULT_WEEKEND_MIN);
    if (ret != ESP_OK)
        return ret;

    ret = init_u16_if_missing("holiday_min", NVS_DEFAULT_HOLIDAY_MIN);
    if (ret != ESP_OK)
        return ret;

    ret = init_u16_if_missing("summer_min", NVS_DEFAULT_SUMMER_MIN);
    if (ret != ESP_OK)
        return ret;

    /* Strings: write only if missing */
    char tmp[64];
    size_t tmp_len = sizeof(tmp);
    if (hal_nvs_read_str("wifi_ssid", tmp, &tmp_len) == ESP_ERR_NVS_NOT_FOUND) {
        ret = hal_nvs_write_str("wifi_ssid", NVS_DEFAULT_WIFI_SSID);
        if (ret != ESP_OK)
            return ret;
    }
    tmp_len = sizeof(tmp);
    if (hal_nvs_read_str("wifi_pass", tmp, &tmp_len) == ESP_ERR_NVS_NOT_FOUND) {
        ret = hal_nvs_write_str("wifi_pass", NVS_DEFAULT_WIFI_PASS);
        if (ret != ESP_OK)
            return ret;
    }
    static const struct {
        const char *key;
        const char *def;
    } MQTT_DEFAULTS[] = {
        {"mqtt_uri", NVS_DEFAULT_MQTT_URI},
        {"mqtt_user", NVS_DEFAULT_MQTT_USER},
        {"mqtt_pass", NVS_DEFAULT_MQTT_PASS},
    };
    for (size_t i = 0; i < sizeof(MQTT_DEFAULTS) / sizeof(MQTT_DEFAULTS[0]); i++) {
        tmp_len = sizeof(tmp);
        if (hal_nvs_read_str(MQTT_DEFAULTS[i].key, tmp, &tmp_len) == ESP_ERR_NVS_NOT_FOUND) {
            ret = hal_nvs_write_str(MQTT_DEFAULTS[i].key, MQTT_DEFAULTS[i].def);
            if (ret != ESP_OK)
                return ret;
        }
    }

    /* Holiday blob: write only if missing */
    {
        uint8_t blob_check[1];
        size_t blob_len = sizeof(blob_check);
        if (hal_nvs_read_blob("holidays", blob_check, &blob_len) == ESP_ERR_NVS_NOT_FOUND) {
            const char *defaults = NVS_DEFAULT_HOLIDAYS;
            size_t def_len = strlen(defaults);
            ret = hal_nvs_write_blob("holidays", defaults, def_len);
            if (ret != ESP_OK)
                return ret;
        }
    }

    return ESP_OK;
}
