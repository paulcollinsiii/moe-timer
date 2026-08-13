#pragma once
#include <stddef.h>
#include <stdint.h>

#include "esp_compat.h"
#include "timer.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nvs_config_init_defaults(void);
/* Stamp derived from NVS_DEFAULTS_VERSION + the compile-time allocation
   defaults; init_defaults reseeds when the stored stamp differs. */
uint16_t nvs_config_defaults_fingerprint(void);

esp_err_t nvs_config_get_weekday_min(uint16_t *out);
esp_err_t nvs_config_set_weekday_min(uint16_t val);
esp_err_t nvs_config_get_weekend_min(uint16_t *out);
esp_err_t nvs_config_set_weekend_min(uint16_t val);
esp_err_t nvs_config_get_holiday_min(uint16_t *out);
esp_err_t nvs_config_set_holiday_min(uint16_t val);
esp_err_t nvs_config_get_summer_min(uint16_t *out);
esp_err_t nvs_config_set_summer_min(uint16_t val);

/* MQTT broker (HA integration). Empty URI = MQTT disabled. */
esp_err_t nvs_config_get_mqtt_uri(char *buf, size_t len);
esp_err_t nvs_config_set_mqtt_uri(const char *uri);
esp_err_t nvs_config_get_mqtt_user(char *buf, size_t len);
esp_err_t nvs_config_set_mqtt_user(const char *user);
esp_err_t nvs_config_get_mqtt_pass(char *buf, size_t len);
esp_err_t nvs_config_set_mqtt_pass(const char *pass);

esp_err_t nvs_config_get_wifi_ssid(char *buf, size_t len);
esp_err_t nvs_config_set_wifi_ssid(const char *ssid);
esp_err_t nvs_config_get_wifi_pass(char *buf, size_t len);
esp_err_t nvs_config_set_wifi_pass(const char *pass);

esp_err_t nvs_config_get_holidays(char *buf, size_t *len);
esp_err_t nvs_config_set_holidays(const char *blob, size_t len);

/* ---- HA config-in keys (phase 2); getters fall back to nvs_defaults ---- */
esp_err_t nvs_config_get_tz(char *buf, size_t len);
esp_err_t nvs_config_set_tz(const char *tz);
esp_err_t nvs_config_get_quiet_start(uint16_t *out);
esp_err_t nvs_config_set_quiet_start(uint16_t hhmm);
esp_err_t nvs_config_get_quiet_end(uint16_t *out);
esp_err_t nvs_config_set_quiet_end(uint16_t hhmm);
esp_err_t nvs_config_get_break_interval_min(uint16_t *out);
esp_err_t nvs_config_set_break_interval_min(uint16_t min);
esp_err_t nvs_config_get_break_duration_min(uint16_t *out);
esp_err_t nvs_config_set_break_duration_min(uint16_t min);
/* Bed Time HHMM (0 = disabled; loader validates via bedtime_hhmm_valid). */
esp_err_t nvs_config_get_bedtime(uint16_t *out);
esp_err_t nvs_config_set_bedtime(uint16_t hhmm);
/* Alert tone selections: tone_id_t indices (audio.c clamps on read). */
esp_err_t nvs_config_get_tone_expiry(uint16_t *out);
esp_err_t nvs_config_set_tone_expiry(uint16_t id);
esp_err_t nvs_config_get_tone_break(uint16_t *out);
esp_err_t nvs_config_set_tone_break(uint16_t id);
esp_err_t nvs_config_get_tone_bed(uint16_t *out);
esp_err_t nvs_config_set_tone_bed(uint16_t id);
/* Alert volume percent (0-TONES_VOLUME_MAX; audio.c clamps on read). */
esp_err_t nvs_config_get_alert_volume(uint16_t *out);
esp_err_t nvs_config_set_alert_volume(uint16_t pct);
esp_err_t nvs_config_get_summer_start(char *buf, size_t len);
esp_err_t nvs_config_set_summer_start(const char *date);
esp_err_t nvs_config_get_school_start(char *buf, size_t len);
esp_err_t nvs_config_set_school_start(const char *date);
esp_err_t nvs_config_get_school_end(char *buf, size_t len);
esp_err_t nvs_config_set_school_end(const char *date);
esp_err_t nvs_config_get_dev_name(char *buf, size_t len); /* "" = use device_id */
esp_err_t nvs_config_set_dev_name(const char *name);
/* Applied HA config version ("" = never applied; cleared by a reseed so
   the retained HA config re-applies after a reflash). */
esp_err_t nvs_config_get_cfg_ver(char *buf, size_t len);
esp_err_t nvs_config_set_cfg_ver(const char *ver);
/* Last applied command id (apply-once dedup for the retained cmd topic). */
esp_err_t nvs_config_get_cmd_id(char *buf, size_t len);
esp_err_t nvs_config_set_cmd_id(const char *id);

/* ---- OTA ------------------------------------------------------------
   None of these are seeded by nvs_config_init_defaults and none are in
   the defaults fingerprint (see nvs_defaults.h) — a menuconfig change
   must not revert an HA-set endpoint. */
/* READER BUFFER SIZES ARE A CONTRACT, not a suggestion. hal_nvs_read_str
   wraps nvs_get_str, which returns ESP_ERR_INVALID_LENGTH on a buffer too
   small and writes NOTHING into it; get_str_*_default maps only
   NOT_FOUND to a default, so a reader that guesses low is left holding an
   UNINITIALISED buffer. Give each getter at least the bound named below.
   The setters reject over-long values rather than truncating, so a
   too-long write fails loudly instead of storing a corrupted value. */

/* Manifest endpoint; "" = OTA disabled. Buffer >= CFG_BOUND_OTA_URL_MAX. */
esp_err_t nvs_config_get_ota_url(char *buf, size_t len);
esp_err_t nvs_config_set_ota_url(const char *url);
/* Also check for an update on every Button D full sync (0/1). */
esp_err_t nvs_config_get_ota_on_sync(uint16_t *out);
esp_err_t nvs_config_set_ota_on_sync(uint16_t on);
/* Device-owned state (no bulk-document key): last attempt's reason code,
   the version the retry budget is counting against, the consecutive-
   failure count for that target, the last download's wall time, and
   whether a committed image is still awaiting certification. The first
   four feed the stat payload (ota_flow_stat reads them at PUBLISH time);
   a different target resets the count.

   Buffer >= CFG_BOUND_OTA_RESULT_MAX. */
esp_err_t nvs_config_get_ota_result(char *buf, size_t len);
esp_err_t nvs_config_set_ota_result(const char *reason);
/* Buffer >= CFG_BOUND_OTA_TARGET_MAX. Reading this one short is not
   cosmetic: the retry-budget comparison would never match its stored
   target, so the device would retry a doomed version forever. */
esp_err_t nvs_config_get_ota_target(char *buf, size_t len);
esp_err_t nvs_config_set_ota_target(const char *ver);
esp_err_t nvs_config_get_ota_fails(uint16_t *out);
esp_err_t nvs_config_set_ota_fails(uint16_t fails);
/* Milliseconds, so u32: a u16 saturates at 65.5 s and the download budget
   is CONFIG_MAGTAG_OTA_MAX_SEC. 0 = no download has ever completed a
   timing. */
esp_err_t nvs_config_get_ota_dl_ms(uint32_t *out);
esp_err_t nvs_config_set_ota_dl_ms(uint32_t ms);
/* 1 = an image was committed and has not yet been certified. The ONLY
   durable trace an OTA reboot leaves behind, and the signal a rollback is
   detected from — see ota_flow.c. Not published; the reason string it
   produces is. */
esp_err_t nvs_config_get_ota_pend(uint16_t *out);
esp_err_t nvs_config_set_ota_pend(uint16_t pending);

/* Extra-timer definitions from HA (timer_defs_install falls back to the
   Kconfig table when absent). Version/size drift reads as stale. */
#define TIMER_DEFS_BLOB_VERSION 2 /* v2: + break_eligible */
typedef struct {
    char name[16]; /* "" = slot disabled */
    int32_t min;
    uint8_t reload;
    uint8_t break_eligible; /* 1 = a genuine break activity (timer_def_t) */
} nvs_timer_def_t;
typedef struct {
    uint8_t version;
    nvs_timer_def_t defs[TIMER_EXTRA_SLOTS];
} nvs_timer_defs_blob_t;
esp_err_t nvs_config_get_timer_defs(nvs_timer_defs_blob_t *out);
esp_err_t nvs_config_set_timer_defs(const nvs_timer_defs_blob_t *defs);

/* Timer crash-recovery snapshot. Load returns ESP_ERR_NVS_NOT_FOUND when
   never saved, ESP_ERR_INVALID_VERSION on size/version drift (stale
   firmware layout); content validation is timer_restore_snapshot's job. */
esp_err_t nvs_config_save_timer_snapshot(const timer_snapshot_t *snap);
esp_err_t nvs_config_load_timer_snapshot(timer_snapshot_t *out);

#ifdef __cplusplus
}
#endif
