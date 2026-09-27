#pragma once
#include <stddef.h>
#include <stdint.h>

#include "esp_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t hal_nvs_read_u16(const char *key, uint16_t *out);
esp_err_t hal_nvs_write_u16(const char *key, uint16_t val);
/* 32-bit counterpart of the pair above, added for ota_dl_ms. A u16 would
   saturate at 65.5 s, an order of magnitude short of the download's own
   deadline (CONFIG_MAGTAG_OTA_MAX_SEC), i.e. it would clip exactly the
   slow transfers the field exists to make visible. */
esp_err_t hal_nvs_read_u32(const char *key, uint32_t *out);
esp_err_t hal_nvs_write_u32(const char *key, uint32_t val);
esp_err_t hal_nvs_read_str(const char *key, char *buf, size_t *len);
esp_err_t hal_nvs_write_str(const char *key, const char *val);
esp_err_t hal_nvs_read_blob(const char *key, void *buf, size_t *len);
esp_err_t hal_nvs_write_blob(const char *key, const void *buf, size_t len);
/* Release the wake-scoped NVS handle (accessors reopen lazily). Called at
   deep-sleep entry; every write above already committed. */
void hal_nvs_close(void);

#ifdef __cplusplus
}
#endif
