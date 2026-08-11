#pragma once

/* Canonical NVS key strings (namespace "timer_cfg"). Every module that
   touches a key by name goes through these constants, so a typo is a
   compile error instead of a silently-missing key. NVS limits keys to
   15 chars — keep new ones short. */

#define NVS_KEY_WEEKDAY_MIN "weekday_min"
#define NVS_KEY_WEEKEND_MIN "weekend_min"
#define NVS_KEY_HOLIDAY_MIN "holiday_min"
#define NVS_KEY_SUMMER_MIN "summer_min"
#define NVS_KEY_WIFI_SSID "wifi_ssid"
#define NVS_KEY_WIFI_PASS "wifi_pass"
#define NVS_KEY_MQTT_URI "mqtt_uri"
#define NVS_KEY_MQTT_USER "mqtt_user"
#define NVS_KEY_MQTT_PASS "mqtt_pass"
#define NVS_KEY_HOLIDAYS "holidays"
#define NVS_KEY_TZ "tz"
#define NVS_KEY_QUIET_START "quiet_start"
#define NVS_KEY_QUIET_END "quiet_end"
#define NVS_KEY_BREAK_INT "break_int"
#define NVS_KEY_BREAK_DUR "break_dur"
#define NVS_KEY_SUMMER_START "summer_start"
#define NVS_KEY_SCHOOL_START "school_start"
#define NVS_KEY_SCHOOL_END "school_end"
#define NVS_KEY_DEV_NAME "dev_name"
#define NVS_KEY_CFG_VER "cfg_ver"
#define NVS_KEY_CMD_ID "cmd_id"
#define NVS_KEY_TIMER_DEFS "timer_defs"
#define NVS_KEY_TIMER_SNAP "timer_snap"
#define NVS_KEY_BEDTIME "bedtime"
#define NVS_KEY_TONE_EXPIRY "tone_expiry"
#define NVS_KEY_TONE_BREAK "tone_break"
#define NVS_KEY_TONE_BED "tone_bed"
#define NVS_KEY_ALERT_VOL "alert_vol"
#define NVS_KEY_DEFAULTS_VER "defaults_ver"
#define NVS_KEY_DISC_VER "disc_ver"
/* Stored ha_config_device_hash(): fingerprints the discovery `dev` block's
   mutable fields — the device name AND the firmware version. The key name
   predates the fw leg and is kept as-is deliberately: renaming it would
   read as missing on every deployed device and force one pointless
   discovery republish. */
#define NVS_KEY_DISC_NAME "disc_name"
/* OTA. The first two are settings (HA-editable); the last three are
   device-owned state written by the update flow and read by the stat
   payload — accessors only, no HA entity and no bulk-document key. */
#define NVS_KEY_OTA_URL "ota_url"
#define NVS_KEY_OTA_ON_SYNC "ota_on_sync"
#define NVS_KEY_OTA_RESULT "ota_result"
#define NVS_KEY_OTA_TARGET "ota_target"
#define NVS_KEY_OTA_FAILS "ota_fails"
