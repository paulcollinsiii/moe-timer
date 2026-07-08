#pragma once
#include <stddef.h>
#include <stdint.h>

#include "esp_compat.h"
#include "timer.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nvs_config_init_defaults(void);
/* Stamp derived from NVS_DEFAULTS_VERSION + the compile-time allocation
   defaults; init_defaults reseeds when the stored stamp differs. */
uint16_t nvs_config_defaults_fingerprint(void);

esp_err_t nvs_config_get_weekday_min(uint16_t *out);
esp_err_t nvs_config_set_weekday_min(uint16_t val);
esp_err_t nvs_config_get_weekend_min(uint16_t *out);
esp_err_t nvs_config_set_weekend_min(uint16_t val);
esp_err_t nvs_config_get_holiday_min(uint16_t *out);
esp_err_t nvs_config_set_holiday_min(uint16_t val);
esp_err_t nvs_config_get_summer_min(uint16_t *out);
esp_err_t nvs_config_set_summer_min(uint16_t val);

/* MQTT broker (HA integration). Empty URI = MQTT disabled. */
esp_err_t nvs_config_get_mqtt_uri(char *buf, size_t len);
esp_err_t nvs_config_set_mqtt_uri(const char *uri);
esp_err_t nvs_config_get_mqtt_user(char *buf, size_t len);
esp_err_t nvs_config_set_mqtt_user(const char *user);
esp_err_t nvs_config_get_mqtt_pass(char *buf, size_t len);
esp_err_t nvs_config_set_mqtt_pass(const char *pass);

esp_err_t nvs_config_get_wifi_ssid(char *buf, size_t len);
esp_err_t nvs_config_set_wifi_ssid(const char *ssid);
esp_err_t nvs_config_get_wifi_pass(char *buf, size_t len);
esp_err_t nvs_config_set_wifi_pass(const char *pass);

esp_err_t nvs_config_get_holidays(char *buf, size_t *len);
esp_err_t nvs_config_set_holidays(const char *blob, size_t len);

/* ---- HA config-in keys (phase 2); getters fall back to nvs_defaults ---- */
esp_err_t nvs_config_get_tz(char *buf, size_t len);
esp_err_t nvs_config_set_tz(const char *tz);
esp_err_t nvs_config_get_quiet_start(uint16_t *out);
esp_err_t nvs_config_set_quiet_start(uint16_t hhmm);
esp_err_t nvs_config_get_quiet_end(uint16_t *out);
esp_err_t nvs_config_set_quiet_end(uint16_t hhmm);
esp_err_t nvs_config_get_break_interval_min(uint16_t *out);
esp_err_t nvs_config_set_break_interval_min(uint16_t min);
esp_err_t nvs_config_get_break_duration_min(uint16_t *out);
esp_err_t nvs_config_set_break_duration_min(uint16_t min);
esp_err_t nvs_config_get_summer_start(char *buf, size_t len);
esp_err_t nvs_config_set_summer_start(const char *date);
esp_err_t nvs_config_get_school_start(char *buf, size_t len);
esp_err_t nvs_config_set_school_start(const char *date);
esp_err_t nvs_config_get_school_end(char *buf, size_t len);
esp_err_t nvs_config_set_school_end(const char *date);
esp_err_t nvs_config_get_dev_name(char *buf, size_t len); /* "" = use device_id */
esp_err_t nvs_config_set_dev_name(const char *name);
/* Applied HA config version ("" = never applied; cleared by a reseed so
   the retained HA config re-applies after a reflash). */
esp_err_t nvs_config_get_cfg_ver(char *buf, size_t len);
esp_err_t nvs_config_set_cfg_ver(const char *ver);
/* Last applied command id (apply-once dedup for the retained cmd topic). */
esp_err_t nvs_config_get_cmd_id(char *buf, size_t len);
esp_err_t nvs_config_set_cmd_id(const char *id);

/* Extra-timer definitions from HA (timer_defs_install falls back to the
   Kconfig table when absent). Version/size drift reads as stale. */
#define TIMER_DEFS_BLOB_VERSION 1
typedef struct {
    char name[16]; /* "" = slot disabled */
    int32_t min;
    uint8_t reload;
} nvs_timer_def_t;
typedef struct {
    uint8_t version;
    nvs_timer_def_t defs[TIMER_EXTRA_SLOTS];
} nvs_timer_defs_blob_t;
esp_err_t nvs_config_get_timer_defs(nvs_timer_defs_blob_t *out);
esp_err_t nvs_config_set_timer_defs(const nvs_timer_defs_blob_t *defs);

/* Timer crash-recovery snapshot. Load returns ESP_ERR_NVS_NOT_FOUND when
   never saved, ESP_ERR_INVALID_VERSION on size/version drift (stale
   firmware layout); content validation is timer_restore_snapshot's job. */
esp_err_t nvs_config_save_timer_snapshot(const timer_snapshot_t *snap);
esp_err_t nvs_config_load_timer_snapshot(timer_snapshot_t *out);

#ifdef __cplusplus
}
#endif
