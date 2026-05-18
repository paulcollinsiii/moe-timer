#pragma once
#include <stddef.h>
#include <stdint.h>

#include "esp_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nvs_config_init_defaults(void);

esp_err_t nvs_config_get_weekday_min(uint16_t *out);
esp_err_t nvs_config_set_weekday_min(uint16_t val);
esp_err_t nvs_config_get_weekend_min(uint16_t *out);
esp_err_t nvs_config_set_weekend_min(uint16_t val);
esp_err_t nvs_config_get_holiday_min(uint16_t *out);
esp_err_t nvs_config_set_holiday_min(uint16_t val);

esp_err_t nvs_config_get_wifi_ssid(char *buf, size_t len);
esp_err_t nvs_config_set_wifi_ssid(const char *ssid);
esp_err_t nvs_config_get_wifi_pass(char *buf, size_t len);
esp_err_t nvs_config_set_wifi_pass(const char *pass);

esp_err_t nvs_config_get_holidays(char *buf, size_t *len);
esp_err_t nvs_config_set_holidays(const char *blob, size_t len);

esp_err_t nvs_config_get_display_contrast(uint8_t *out);
esp_err_t nvs_config_set_display_contrast(uint8_t val);

#ifdef __cplusplus
}
#endif
