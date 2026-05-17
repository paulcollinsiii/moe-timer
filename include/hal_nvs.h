#pragma once
#include <stddef.h>
#include <stdint.h>
/* esp_err_t: from esp_err.h in magtag build, from esp_compat.h in native. */

esp_err_t hal_nvs_read_u16(const char *key, uint16_t *out);
esp_err_t hal_nvs_write_u16(const char *key, uint16_t val);
esp_err_t hal_nvs_read_str(const char *key, char *buf, size_t len);
esp_err_t hal_nvs_write_str(const char *key, const char *val);
esp_err_t hal_nvs_read_blob(const char *key, void *buf, size_t *len);
esp_err_t hal_nvs_write_blob(const char *key, const void *buf, size_t len);
