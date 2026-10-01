# Stream 3c — WiFi / NTP Module Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement `ntp.c` — a single `ntp_sync()` function that performs the full WiFi+SNTP lifecycle, corrects the ESP32 system clock, and returns. It never reads or writes `expiry_wall_time`.

**Architecture:** `ntp_sync()` is a blocking synchronous function. It: initialises `esp_netif`, starts `esp_wifi` in station mode, connects using credentials from NVS, triggers SNTP sync via `esp_sntp`, polls `sntp_get_sync_status()` until synced or timeout, then tears everything down. The compile gate (`pio run`) is the validation; no hardware flashing in this stream.

**Tech Stack:** ESP-IDF `esp_wifi`, `esp_netif`, `esp_sntp` (lwIP SNTP), `nvs_config.h`

**Prerequisite:** Stream 1 (`feature/test-harness`) merged to `integration`.

**Critical invariant:** `ntp_sync()` MUST NOT touch `expiry_wall_time`. After `ntp_sync()` returns `ESP_OK`, the caller reads `time(NULL)` which now reflects corrected UTC. The `remaining = expiry_wall_time - time(NULL)` subtraction self-corrects.

---

## Files

| Action | Path |
|---|---|
| Modify | `src/ntp.c` |
| Modify | `sdkconfig.defaults` |

`include/ntp.h` is created in Stream 0 — do not modify its signature.

---

## Task 1: Create branch

- [ ] **Step 1: Branch from integration**

```bash
git fetch origin
git checkout integration
git checkout -b feature/wifi-ntp
```

---

## Task 2: Update sdkconfig.defaults

**Files:** Modify `sdkconfig.defaults`

- [ ] **Step 1: Add WiFi and SNTP Kconfig entries**

Replace the full content of `sdkconfig.defaults`:

```
# Force immediate SNTP sync instead of smooth adjustment
CONFIG_SNTP_TIME_SYNC_METHOD_IMMED=y

# Use WPA2 station mode — no enterprise auth needed
CONFIG_ESP_WIFI_AUTH_OPEN=n

# Timezone configured in code via setenv("TZ", ...)
# POSIX TZ string is defined as TZ_STRING in ntp.c

# LWIP SNTP: use pool.ntp.org
CONFIG_LWIP_SNTP_MAX_SERVERS=1

# Increase WiFi task stack if connection drops
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=10
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=32
```

- [ ] **Step 2: Commit**

```bash
git add sdkconfig.defaults
git commit -m "build: add WiFi and SNTP sdkconfig defaults"
```

---

## Task 3: Implement ntp.c

**Files:** Replace `src/ntp.c`

- [ ] **Step 1: Implement the full lifecycle**

```c
#include "ntp.h"
#include "nvs_config.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <time.h>

static const char *TAG = "ntp";

/* POSIX TZ string — compile-time timezone.
   Format: POSIX TZ rule, e.g. "EST5EDT,M3.2.0,M11.1.0" for US Eastern.
   Change for your locale. */
#define TZ_STRING "EST5EDT,M3.2.0,M11.1.0"

/* Maximum time to wait for WiFi connection and SNTP sync */
#define WIFI_CONNECT_TIMEOUT_MS  15000
#define SNTP_SYNC_TIMEOUT_MS     15000

static EventGroupHandle_t s_wifi_event_group = NULL;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

/* ---- Event handler ---- */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected");
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Got IP address");
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ---- ntp_sync ---- */

esp_err_t ntp_sync(void)
{
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

    esp_netif_create_default_wifi_sta();

    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        ESP_LOGE(TAG, "xEventGroupCreate failed");
        return ESP_ERR_NO_MEM;
    }

    esp_event_handler_instance_t inst_wifi, inst_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_event_handler, NULL, &inst_wifi);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        wifi_event_handler, NULL, &inst_ip);

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&wifi_init_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(ret));
        goto cleanup_events;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) goto cleanup_wifi;

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (ret != ESP_OK) goto cleanup_wifi;

    ret = esp_wifi_start();
    if (ret != ESP_OK) goto cleanup_wifi;

    ret = esp_wifi_connect();
    if (ret != ESP_OK) goto cleanup_wifi_started;

    /* ---- Wait for connection ---- */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "WiFi connection failed or timed out");
        ret = ESP_ERR_WIFI_NOT_CONNECT;
        goto cleanup_wifi_started;
    }

    /* ---- SNTP sync ---- */
    setenv("TZ", TZ_STRING, 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
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
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, inst_wifi);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, inst_ip);
    vEventGroupDelete(s_wifi_event_group);
    s_wifi_event_group = NULL;

    return ret;
}
```

- [ ] **Step 2: Commit**

```bash
git add src/ntp.c
git commit -m "feat(ntp): implement WiFi+SNTP sync lifecycle via esp_wifi and esp_sntp"
```

---

## Task 4: Update CMakeLists.txt for WiFi components

**Files:** Modify `src/CMakeLists.txt`

- [ ] **Step 1: Add WiFi and SNTP components to REQUIRES**

The `esp_wifi`, `esp_netif`, and `lwip` (which provides `esp_sntp`) components must be listed. If `src/CMakeLists.txt` from Stream 0 already has them, skip this step.

Verify the `REQUIRES` section of `src/CMakeLists.txt` includes:
```cmake
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        lwip
        esp_timer
        driver
        log
        led_strip
```

Add any missing entries. `lwip` provides `esp_sntp.h`.

- [ ] **Step 2: Commit if changed**

```bash
git add src/CMakeLists.txt
git commit -m "build: add esp_event and lwip to component REQUIRES for WiFi+SNTP"
```

---

## Task 5: Compile gate — verify pio run succeeds

- [ ] **Step 1: Run the build**

```bash
pio run
```

Expected: `[SUCCESS]`.

**Troubleshooting:**

- `esp_sntp.h` not found: in ESP-IDF v5+, the header is `esp_sntp.h` and the component is `lwip`. If using ESP-IDF v4.x, the header may be at `apps/sntp/sntp.h`. Check IDF version: `cat $IDF_PATH/components/esp_idf_support/include/esp_idf_version.h`.
- `esp_sntp_setoperatingmode` undefined: in older ESP-IDF, this was `sntp_setoperatingmode(SNTP_OPMODE_POLL)` (no `esp_` prefix). Adjust if needed.
- `WIFI_AUTH_WPA2_PSK` vs `WIFI_AUTH_WPA_WPA2_PSK`: use `WIFI_AUTH_WPA_WPA2_PSK` for broader compatibility with home routers.
- Event group leaks on error path: verify all `goto` labels clean up the event group and unregister handlers. The `inst_wifi` and `inst_ip` variables must be in scope for the `goto` labels — check that they're declared before the first `goto` target.
- `esp_sntp_get_sync_status` returns `SNTP_SYNC_STATUS_IN_PROGRESS` indefinitely: this can happen if the TZ string is wrong or DNS fails. The timeout guard handles it.

- [ ] **Step 2: Commit any fixes**

```bash
git add -u
git commit -m "fix(ntp): resolve ESP-IDF API compatibility issues"
```

---

## Task 6: Code review

- [ ] **Step 1: Run adversarial code review**

Use `superpowers:requesting-code-review` skill. Focus areas:
- **`expiry_wall_time` invariant**: search `src/ntp.c` for any reference to `expiry_wall_time` or `g_rtc_state`. There must be zero. If any appear, this is a hard block.
- **WiFi stays on after error**: trace all error paths through the `goto` labels. Every path that calls `esp_wifi_start()` must call `esp_wifi_stop()` before returning. Every path that calls `esp_wifi_init()` must call `esp_wifi_deinit()`.
- **Event group leak**: `vEventGroupDelete` called on all paths? Yes — it's in `cleanup_events` which all paths reach.
- **`esp_netif_init()` idempotency**: returns `ESP_ERR_INVALID_STATE` if called twice (e.g., on second NTP sync). The code handles this; confirm.
- **`esp_event_loop_create_default()` idempotency**: same pattern — confirm `ESP_ERR_INVALID_STATE` is handled.
- **Stack size**: `ntp_sync()` is called from `app_main` task (8 KB stack). WiFi event handler is called from the WiFi task, not app_main. FreeRTOS event groups cross task boundaries correctly here.
- **Password in log**: `ssid` is logged at INFO level; `pass` is NOT logged. Confirm.
- **Timeout calculation**: `WIFI_CONNECT_TIMEOUT_MS = 15000`. If the user's router is slow, this may need to be longer. Configurable via `#define` is sufficient.

Fix any issues before merging.

---

## Task 7: Merge to integration

- [ ] **Step 1: Push and merge**

```bash
git push -u origin feature/wifi-ntp
git checkout integration
git merge --no-ff feature/wifi-ntp -m "feat(ntp): WiFi+SNTP sync lifecycle using esp_wifi and esp_sntp"
git push origin integration
```
