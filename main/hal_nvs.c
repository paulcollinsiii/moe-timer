#include "hal_nvs.h"

#ifndef NATIVE
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define NVS_NAMESPACE "timer_cfg"
static const char *TAG = "hal_nvs";

/* One handle per wake: opened lazily on first access, closed by
   hal_nvs_close() at sleep entry. A network-window wake issues dozens of
   accessor calls; per-call open/close was pure flash-walk overhead. The
   lazy open is not lock-guarded: the first access is app_main's
   nvs_config_init_defaults(), which completes before the wake handlers
   run — the LED task's first pixel command (quiet-hours read) and the
   network task's spawn both happen after that, so by the time any other
   task calls in, s_open is already true. The NVS API itself is internally
   synchronized for the concurrent reads/writes that follow. */
static nvs_handle_t s_handle;
static bool s_open;

static esp_err_t open_nvs(nvs_handle_t *out_handle) {
    if (!s_open) {
        esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(ret));
            return ret;
        }
        s_open = true;
    }
    *out_handle = s_handle;
    return ESP_OK;
}

void hal_nvs_close(void) {
    if (s_open) {
        nvs_close(s_handle);
        s_open = false;
    }
}

esp_err_t hal_nvs_read_u16(const char *key, uint16_t *out) {
    nvs_handle_t h;
    esp_err_t ret = open_nvs(&h);
    if (ret != ESP_OK)
        return ret;
    return nvs_get_u16(h, key, out);
}

esp_err_t hal_nvs_write_u16(const char *key, uint16_t val) {
    nvs_handle_t h;
    esp_err_t ret = open_nvs(&h);
    if (ret != ESP_OK)
        return ret;
    ret = nvs_set_u16(h, key, val);
    if (ret == ESP_OK)
        ret = nvs_commit(h);
    return ret;
}

esp_err_t hal_nvs_read_str(const char *key, char *buf, size_t *len) {
    nvs_handle_t h;
    esp_err_t ret = open_nvs(&h);
    if (ret != ESP_OK)
        return ret;
    return nvs_get_str(h, key, buf, len);
}

esp_err_t hal_nvs_write_str(const char *key, const char *val) {
    nvs_handle_t h;
    esp_err_t ret = open_nvs(&h);
    if (ret != ESP_OK)
        return ret;
    ret = nvs_set_str(h, key, val);
    if (ret == ESP_OK)
        ret = nvs_commit(h);
    return ret;
}

esp_err_t hal_nvs_read_blob(const char *key, void *buf, size_t *len) {
    nvs_handle_t h;
    esp_err_t ret = open_nvs(&h);
    if (ret != ESP_OK)
        return ret;
    return nvs_get_blob(h, key, buf, len);
}

esp_err_t hal_nvs_write_blob(const char *key, const void *buf, size_t len) {
    nvs_handle_t h;
    esp_err_t ret = open_nvs(&h);
    if (ret != ESP_OK)
        return ret;
    ret = nvs_set_blob(h, key, buf, len);
    if (ret == ESP_OK)
        ret = nvs_commit(h);
    return ret;
}

#endif /* NATIVE */
