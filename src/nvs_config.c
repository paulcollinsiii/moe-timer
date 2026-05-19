#include "nvs_config.h"

#include <string.h>

#include "hal_nvs.h"
#include "nvs.h"
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

/* ---- display contrast accessor ---- */

esp_err_t nvs_config_get_display_contrast(uint8_t *out) {
    uint16_t tmp;
    esp_err_t ret = get_u16_with_default("disp_contrast", &tmp, NVS_DEFAULT_CONTRAST);
    *out = (uint8_t)tmp;
    return ret;
}

esp_err_t nvs_config_set_display_contrast(uint8_t val) {
    return hal_nvs_write_u16("disp_contrast", (uint16_t)val);
}

/* ---- init defaults ---- */

esp_err_t nvs_config_init_defaults(void) {
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

    ret = init_u16_if_missing("disp_contrast", NVS_DEFAULT_CONTRAST);
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
