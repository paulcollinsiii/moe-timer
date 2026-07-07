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

esp_err_t nvs_config_get_wifi_ssid(char *buf, size_t len);
esp_err_t nvs_config_set_wifi_ssid(const char *ssid);
esp_err_t nvs_config_get_wifi_pass(char *buf, size_t len);
esp_err_t nvs_config_set_wifi_pass(const char *pass);

esp_err_t nvs_config_get_holidays(char *buf, size_t *len);
esp_err_t nvs_config_set_holidays(const char *blob, size_t len);

/* Timer crash-recovery snapshot. Load returns ESP_ERR_NVS_NOT_FOUND when
   never saved, ESP_ERR_INVALID_VERSION on size/version drift (stale
   firmware layout); content validation is timer_restore_snapshot's job. */
esp_err_t nvs_config_save_timer_snapshot(const timer_snapshot_t *snap);
esp_err_t nvs_config_load_timer_snapshot(timer_snapshot_t *out);

#ifdef __cplusplus
}
#endif
