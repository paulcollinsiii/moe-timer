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
#define ESP_ERR_NVS_INVALID_LENGTH 0x110C
#define ESP_ERR_WIFI_NOT_CONNECT 0x3002

/* app_update (esp_ota_ops.h) and bootloader_support
   (esp_image_format.h), trimmed to the codes ota_facts.c classifies.
   Real values, so the host suite is asserting against what the device
   will actually be handed. */
#define ESP_ERR_OTA_BASE 0x1500
#define ESP_ERR_OTA_PARTITION_CONFLICT (ESP_ERR_OTA_BASE + 0x01)
#define ESP_ERR_OTA_SELECT_INFO_INVALID (ESP_ERR_OTA_BASE + 0x02)
#define ESP_ERR_OTA_VALIDATE_FAILED (ESP_ERR_OTA_BASE + 0x03)
#define ESP_ERR_OTA_SMALL_SEC_VER (ESP_ERR_OTA_BASE + 0x04)
#define ESP_ERR_OTA_ROLLBACK_FAILED (ESP_ERR_OTA_BASE + 0x05)
#define ESP_ERR_OTA_ROLLBACK_INVALID_STATE (ESP_ERR_OTA_BASE + 0x06)

#define ESP_ERR_IMAGE_BASE 0x2000
#define ESP_ERR_IMAGE_FLASH_FAIL (ESP_ERR_IMAGE_BASE + 1)
#define ESP_ERR_IMAGE_INVALID (ESP_ERR_IMAGE_BASE + 2)

/* esp-tls (esp_tls_errors.h), in IDF's declaration order and with IDF's
   values. The WHOLE range is mirrored rather than just the codes
   ota_facts.c names, because the classification it performs is a range
   test with a carve-out: a mirror missing the middle would let a host
   test pass against a gap the device does not have.
   ota_facts.c carries a _Static_assert tying the last of these to
   ESP_ERR_ESP_TLS_BASE, so a drift here fails the host build. */
#define ESP_ERR_ESP_TLS_BASE 0x8000
#define ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME (ESP_ERR_ESP_TLS_BASE + 0x01)
#define ESP_ERR_ESP_TLS_CANNOT_CREATE_SOCKET (ESP_ERR_ESP_TLS_BASE + 0x02)
#define ESP_ERR_ESP_TLS_UNSUPPORTED_PROTOCOL_FAMILY (ESP_ERR_ESP_TLS_BASE + 0x03)
#define ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST (ESP_ERR_ESP_TLS_BASE + 0x04)
#define ESP_ERR_ESP_TLS_SOCKET_SETOPT_FAILED (ESP_ERR_ESP_TLS_BASE + 0x05)
#define ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT (ESP_ERR_ESP_TLS_BASE + 0x06)
#define ESP_ERR_ESP_TLS_SE_FAILED (ESP_ERR_ESP_TLS_BASE + 0x07)
#define ESP_ERR_ESP_TLS_TCP_CLOSED_FIN (ESP_ERR_ESP_TLS_BASE + 0x08)
#define ESP_ERR_ESP_TLS_SERVER_HANDSHAKE_TIMEOUT (ESP_ERR_ESP_TLS_BASE + 0x09)
#define ESP_ERR_MBEDTLS_CERT_PARTLY_OK (ESP_ERR_ESP_TLS_BASE + 0x10)
#define ESP_ERR_MBEDTLS_CTR_DRBG_SEED_FAILED (ESP_ERR_ESP_TLS_BASE + 0x11)
#define ESP_ERR_MBEDTLS_SSL_SET_HOSTNAME_FAILED (ESP_ERR_ESP_TLS_BASE + 0x12)
#define ESP_ERR_MBEDTLS_SSL_CONFIG_DEFAULTS_FAILED (ESP_ERR_ESP_TLS_BASE + 0x13)
#define ESP_ERR_MBEDTLS_SSL_CONF_ALPN_PROTOCOLS_FAILED (ESP_ERR_ESP_TLS_BASE + 0x14)
#define ESP_ERR_MBEDTLS_X509_CRT_PARSE_FAILED (ESP_ERR_ESP_TLS_BASE + 0x15)
#define ESP_ERR_MBEDTLS_SSL_CONF_OWN_CERT_FAILED (ESP_ERR_ESP_TLS_BASE + 0x16)
#define ESP_ERR_MBEDTLS_SSL_SETUP_FAILED (ESP_ERR_ESP_TLS_BASE + 0x17)
#define ESP_ERR_MBEDTLS_SSL_WRITE_FAILED (ESP_ERR_ESP_TLS_BASE + 0x18)
#define ESP_ERR_MBEDTLS_PK_PARSE_KEY_FAILED (ESP_ERR_ESP_TLS_BASE + 0x19)
#define ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED (ESP_ERR_ESP_TLS_BASE + 0x1A)
#define ESP_ERR_MBEDTLS_SSL_CONF_PSK_FAILED (ESP_ERR_ESP_TLS_BASE + 0x1B)
#define ESP_ERR_MBEDTLS_SSL_TICKET_SETUP_FAILED (ESP_ERR_ESP_TLS_BASE + 0x1C)
#define ESP_ERR_MBEDTLS_SSL_READ_FAILED (ESP_ERR_ESP_TLS_BASE + 0x1D)

/* RTC_DATA_ATTR places data in RTC slow memory on ESP32.
   On native, it's a no-op — g_rtc_state is a regular static. */
#define RTC_DATA_ATTR

/* Reset causes, in ESP-IDF's declaration order so the values match the
   real enum (esp_system.h). Trimmed to the causes the forensics map
   names, plus USB, which the setup trigger classifies as a cold boot;
   IDF's remaining reasons (SDIO, JTAG, ...) exist only on device and
   reach the maps' default arm there. USB sits after BROWNOUT here, so its
   value differs from the real enum's (SDIO comes between them there):
   nothing on host depends on the numeric value. */
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
    ESP_RST_USB,
} esp_reset_reason_t;

/* The app description ESP-IDF builds into the image (esp_app_desc.h).
   Trimmed to the one field any host-built TU reads — `version`, which the
   stat snapshot publishes — for the same reason the enum above is
   trimmed: the fields nobody names would be dead weight the suite has to
   keep in step for nothing. Nothing crosses a real ABI on host, so the
   layout does not have to match; `version` is a char array in IDF too, so
   the decay-to-pointer that the snapshot relies on behaves identically in
   both builds. A TU that starts reading project_name or idf_ver adds it
   here and gets a compile error on device if it guessed the name wrong. */
typedef struct {
    char version[32];
} esp_app_desc_t;
