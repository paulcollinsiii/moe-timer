/* SNTP clock sync — runs inside an open wifi_session window. */
#include "ntp.h"

#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ntp";

#define SNTP_SYNC_TIMEOUT_MS 15000

esp_err_t ntp_sync_in_session(void) {
    /* TZ is set once at boot in app_main. */
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    /* Immediate clock step (not smooth adjust) — the countdown math relies
       on time(NULL) being corrected in one jump. Runtime call replaces the
       CONFIG_SNTP_TIME_SYNC_METHOD kconfig removed in IDF 6. */
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_init();

    esp_err_t ret = ESP_OK;
    int sntp_wait_ms = 0;
    while (esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
        vTaskDelay(pdMS_TO_TICKS(500));
        sntp_wait_ms += 500;
        if (sntp_wait_ms >= SNTP_SYNC_TIMEOUT_MS) {
            ESP_LOGE(TAG, "SNTP sync timed out after %d ms", sntp_wait_ms);
            ret = ESP_ERR_TIMEOUT;
            break;
        }
    }
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SNTP sync complete");
    }
    esp_sntp_stop();
    return ret;
}
