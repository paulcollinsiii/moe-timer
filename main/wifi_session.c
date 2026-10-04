#include "wifi_session.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_config.h"

static const char *TAG = "wifi_session";

#define WIFI_CONNECT_TIMEOUT_MS 15000

/* On warm wakes the driver fast-reconnects from NVS-stored channel/BSSID;
   that first attempt routinely bounces once before a clean association, so
   a single disconnect must not be treated as failure (IDF station-example
   pattern). */
#define WIFI_CONNECT_MAX_RETRY 5

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_STOPPED_BIT BIT2

static volatile int s_retry_num;
static volatile bool s_retry_enabled; /* false during teardown: our own
                                         esp_wifi_disconnect() also fires
                                         STA_DISCONNECTED */
static esp_event_handler_instance_t s_inst_wifi, s_inst_ip;

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *dis = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "WiFi disconnected (reason %d)", dis ? dis->reason : -1);
        if (s_retry_enabled && s_retry_num < WIFI_CONNECT_MAX_RETRY) {
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying connect (%d/%d)", s_retry_num, WIFI_CONNECT_MAX_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_STOP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_STOPPED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Got IP address");
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void cleanup_events(void) {
    if (s_inst_wifi) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_inst_wifi);
        s_inst_wifi = NULL;
    }
    if (s_inst_ip) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_inst_ip);
        s_inst_ip = NULL;
    }
    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }
}

/* Returns the boot-global default STA netif, creating it on first call.
   See wifi_session.h's own doc comment: this is now the ONE place that
   calls esp_netif_create_default_wifi_sta(), shared with
   setup_session_idf.c, so the two can no longer race to create a second
   netif behind the same "WIFI_STA_DEF" if_key. */
esp_netif_t *wifi_session_sta_netif(void) {
    static esp_netif_t *s_sta_netif;
    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }
    return s_sta_netif;
}

esp_err_t wifi_session_begin(void) {
    char ssid[NVS_CONFIG_WIFI_SSID_BUF] = {0};
    char pass[NVS_CONFIG_WIFI_PASS_BUF] = {0};
    nvs_config_get_wifi_ssid(ssid, sizeof(ssid));
    nvs_config_get_wifi_pass(pass, sizeof(pass));

    if (ssid[0] == '\0') {
        ESP_LOGE(TAG, "WiFi SSID not configured in NVS");
        return ESP_ERR_INVALID_STATE;
    }

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

    /* Shared with setup_session_idf.c (wifi_session.h) — created once per
       boot, never twice. Creating it per session leaks a netif +
       duplicate default handlers (two sessions per wake is a routine
       path: day rollover + mandatory start sync). */
    (void)wifi_session_sta_netif();

    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        ESP_LOGE(TAG, "xEventGroupCreate failed");
        return ESP_ERR_NO_MEM;
    }

    ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &s_inst_wifi);
    if (ret != ESP_OK)
        goto fail_events;
    ret = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, &s_inst_ip);
    if (ret != ESP_OK)
        goto fail_events;

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&wifi_init_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(ret));
        goto fail_events;
    }

    /* RAM, not the driver's default flash-backed store: this app's own
       NVS keys (above) are the one copy of these credentials that
       persists, and esp_wifi_set_config below must not write a second,
       independent copy that a future join could read instead and
       silently disagree with. This is also what makes setup_session's
       D3 clear (network_prov_mgr_reset_wifi_provisioning, run once after
       a provisioning success) mean something: without this, the very
       next normal window's esp_wifi_set_config would repopulate the
       driver's flash store from these same NVS keys anyway, so the
       clear would be erasing a copy that was about to be overwritten
       for free. */
    ret = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_storage: %s", esp_err_to_name(ret));
        goto fail_wifi;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK)
        goto fail_wifi;

    wifi_config_t wifi_cfg = {0};
    /* memcpy, not strncpy: wifi_config_t's ssid[32]/password[64] are not
       NUL-terminated strings by contract, and a full-width SSID or a
       64-character hex PSK has no room left for strncpy's own NUL (it
       would silently drop the last byte of either). wifi_cfg is already
       zeroed above, so bytes past each strnlen() stay zero either way. */
    size_t ssid_len = strnlen(ssid, sizeof(wifi_cfg.sta.ssid));
    size_t pass_len = strnlen(pass, sizeof(wifi_cfg.sta.password));
    memcpy(wifi_cfg.sta.ssid, ssid, ssid_len);
    memcpy(wifi_cfg.sta.password, pass, pass_len);
    /* An open network has no password to threshold against; WPA2_PSK
       would refuse to join one that provisioned and verified fine
       moments earlier. */
    wifi_cfg.sta.threshold.authmode = (pass_len == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (ret != ESP_OK)
        goto fail_wifi;

    ret = esp_wifi_start();
    if (ret != ESP_OK)
        goto fail_started;

    s_retry_num = 0;
    s_retry_enabled = true;
    ret = esp_wifi_connect();
    if (ret != ESP_OK)
        goto fail_started;

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "WiFi connection failed or timed out");
        ret = ESP_ERR_WIFI_NOT_CONNECT;
        goto fail_started;
    }

    return ESP_OK;

fail_started:
    s_retry_enabled = false;
    esp_wifi_disconnect();
    esp_wifi_stop();
    xEventGroupWaitBits(s_wifi_event_group, WIFI_STOPPED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));

fail_wifi:
    esp_wifi_deinit();

fail_events:
    cleanup_events();
    return ret;
}

void wifi_session_end(void) {
    s_retry_enabled = false;
    esp_wifi_disconnect();
    esp_wifi_stop();
    /* Stop is asynchronous; deinit before STA_STOP lands fails with
       ESP_ERR_WIFI_STOP_STATE (0x3014). Bounded wait, then deinit anyway —
       deep sleep reboots us, so a leaked driver cannot accumulate. */
    if (s_wifi_event_group) {
        xEventGroupWaitBits(s_wifi_event_group, WIFI_STOPPED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
    }
    esp_wifi_deinit();
    cleanup_events();
}
