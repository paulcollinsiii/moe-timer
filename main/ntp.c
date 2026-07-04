#include "ntp.h"

#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_config.h"

static const char *TAG = "ntp";

/* Maximum time to wait for WiFi connection and SNTP sync */
#define WIFI_CONNECT_TIMEOUT_MS 15000
#define SNTP_SYNC_TIMEOUT_MS 15000

static EventGroupHandle_t s_wifi_event_group = NULL;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

/* ---- Event handler ---- */

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected");
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Got IP address");
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ---- ntp_sync ---- */

esp_err_t ntp_sync(void) {
    /* Read credentials from NVS */
    char ssid[64] = {0};
    char pass[64] = {0};
    nvs_config_get_wifi_ssid(ssid, sizeof(ssid));
    nvs_config_get_wifi_pass(pass, sizeof(pass));

    if (ssid[0] == '\0') {
        ESP_LOGE(TAG, "WiFi SSID not configured in NVS");
        return ESP_ERR_INVALID_STATE;
    }

    /* ---- Init ---- */
    esp_err_t ret;

    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Create the default STA netif once per boot — creating it per sync
       leaks a netif + duplicate default handlers (two syncs per wake is a
       routine path: day rollover + mandatory start sync). */
    static esp_netif_t *s_sta_netif;
    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }

    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        ESP_LOGE(TAG, "xEventGroupCreate failed");
        return ESP_ERR_NO_MEM;
    }

    esp_event_handler_instance_t inst_wifi = NULL, inst_ip = NULL;
    ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &inst_wifi);
    if (ret != ESP_OK)
        goto cleanup_events;
    ret = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, &inst_ip);
    if (ret != ESP_OK)
        goto cleanup_events;

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&wifi_init_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(ret));
        goto cleanup_events;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK)
        goto cleanup_wifi;

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (ret != ESP_OK)
        goto cleanup_wifi;

    ret = esp_wifi_start();
    if (ret != ESP_OK)
        goto cleanup_wifi_started;

    ret = esp_wifi_connect();
    if (ret != ESP_OK)
        goto cleanup_wifi_started;

    /* ---- Wait for connection ---- */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "WiFi connection failed or timed out");
        ret = ESP_ERR_WIFI_NOT_CONNECT;
        goto cleanup_wifi_started;
    }

    /* ---- SNTP sync ---- */
    /* TZ is set once at boot in app_main (MAGTAG_TZ). */
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    /* Immediate clock step (not smooth adjust) — the countdown math relies
       on time(NULL) being corrected in one jump. Runtime call replaces the
       CONFIG_SNTP_TIME_SYNC_METHOD kconfig removed in IDF 6. */
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_init();

    int sntp_wait_ms = 0;
    while (esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
        vTaskDelay(pdMS_TO_TICKS(500));
        sntp_wait_ms += 500;
        if (sntp_wait_ms >= SNTP_SYNC_TIMEOUT_MS) {
            ESP_LOGE(TAG, "SNTP sync timed out after %d ms", sntp_wait_ms);
            ret = ESP_ERR_TIMEOUT;
            goto cleanup_sntp;
        }
    }

    ESP_LOGI(TAG, "SNTP sync complete");
    ret = ESP_OK;

cleanup_sntp:
    esp_sntp_stop();

cleanup_wifi_started:
    esp_wifi_disconnect();
    esp_wifi_stop();

cleanup_wifi:
    esp_wifi_deinit();

cleanup_events:
    if (inst_wifi)
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, inst_wifi);
    if (inst_ip)
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, inst_ip);
    vEventGroupDelete(s_wifi_event_group);
    s_wifi_event_group = NULL;

    return ret;
}
