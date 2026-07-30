/* test/mocks/esp_compat.h
 * Force-included in native builds via -include build flag.
 * Provides minimal ESP-IDF type aliases for host-based unit tests.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_INVALID_VERSION 0x10A
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_INVALID_NAME 0x1105
#define ESP_ERR_WIFI_NOT_CONNECT 0x3002

/* RTC_DATA_ATTR places data in RTC slow memory on ESP32.
   On native, it's a no-op — g_rtc_state is a regular static. */
#define RTC_DATA_ATTR

/* Reset causes, in ESP-IDF's declaration order so the values match the
   real enum (esp_system.h). Trimmed to the causes the forensics map
   names; IDF's remaining reasons (SDIO, USB, JTAG, ...) exist only on
   device and reach the map's default arm there. */
typedef enum {
    ESP_RST_UNKNOWN = 0,
    ESP_RST_POWERON,
    ESP_RST_EXT,
    ESP_RST_SW,
    ESP_RST_PANIC,
    ESP_RST_INT_WDT,
    ESP_RST_TASK_WDT,
    ESP_RST_WDT,
    ESP_RST_DEEPSLEEP,
    ESP_RST_BROWNOUT,
} esp_reset_reason_t;
