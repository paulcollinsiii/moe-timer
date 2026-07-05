#include "nvs_config.h"

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

/* ---- string accessors ---- */

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
    ret = hal_nvs_write_str("wifi_ssid", NVS_DEFAULT_WIFI_SSID);
    if (ret != ESP_OK)
        return ret;
    ret = hal_nvs_write_str("wifi_pass", NVS_DEFAULT_WIFI_PASS);
    if (ret != ESP_OK)
        return ret;
    ret = hal_nvs_write_blob("holidays", NVS_DEFAULT_HOLIDAYS, strlen(NVS_DEFAULT_HOLIDAYS));
    if (ret != ESP_OK)
        return ret;
    /* Stamp last: a power cut mid-reseed re-runs the whole reseed */
    return hal_nvs_write_u16("defaults_ver", NVS_DEFAULTS_VERSION);
}

esp_err_t nvs_config_init_defaults(void) {
    /* Version stamp: when the compile-time defaults change (bump
       NVS_DEFAULTS_VERSION in nvs_defaults.h), overwrite everything —
       no erase-flash needed. A missing stamp also reseeds (covers
       devices seeded before the stamp existed). */
    uint16_t ver = 0;
    esp_err_t vret = hal_nvs_read_u16("defaults_ver", &ver);
    if (vret == ESP_ERR_NVS_NOT_FOUND || (vret == ESP_OK && ver != NVS_DEFAULTS_VERSION)) {
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
