#include "hal_nvs.h"

#ifndef NATIVE
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define NVS_NAMESPACE "timer_cfg"
static const char *TAG = "hal_nvs";

static esp_err_t open_nvs(nvs_handle_t *out_handle) {
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, out_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t hal_nvs_read_u16(const char *key, uint16_t *out) {
    nvs_handle_t h;
    esp_err_t ret = open_nvs(&h);
    if (ret != ESP_OK)
        return ret;
    ret = nvs_get_u16(h, key, out);
    nvs_close(h);
    return ret;
}

esp_err_t hal_nvs_write_u16(const char *key, uint16_t val) {
    nvs_handle_t h;
    esp_err_t ret = open_nvs(&h);
    if (ret != ESP_OK)
        return ret;
    ret = nvs_set_u16(h, key, val);
    if (ret == ESP_OK)
        ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

esp_err_t hal_nvs_read_str(const char *key, char *buf, size_t *len) {
    nvs_handle_t h;
    esp_err_t ret = open_nvs(&h);
    if (ret != ESP_OK)
        return ret;
    ret = nvs_get_str(h, key, buf, len);
    nvs_close(h);
    return ret;
}

esp_err_t hal_nvs_write_str(const char *key, const char *val) {
    nvs_handle_t h;
    esp_err_t ret = open_nvs(&h);
    if (ret != ESP_OK)
        return ret;
    ret = nvs_set_str(h, key, val);
    if (ret == ESP_OK)
        ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

esp_err_t hal_nvs_read_blob(const char *key, void *buf, size_t *len) {
    nvs_handle_t h;
    esp_err_t ret = open_nvs(&h);
    if (ret != ESP_OK)
        return ret;
    ret = nvs_get_blob(h, key, buf, len);
    nvs_close(h);
    return ret;
}

esp_err_t hal_nvs_write_blob(const char *key, const void *buf, size_t len) {
    nvs_handle_t h;
    esp_err_t ret = open_nvs(&h);
    if (ret != ESP_OK)
        return ret;
    ret = nvs_set_blob(h, key, buf, len);
    if (ret == ESP_OK)
        ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

#endif /* NATIVE */
