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
/* Stored ha_config_discovery_hash(): fingerprints discovery's mutable
   inputs — the `dev` block's device name AND firmware version
   (ha_config_device_hash), the extra-timer slot names and enablement, and
   the chore list. The key name predates every leg but the name and is
   kept as-is deliberately: renaming it would
   read as missing on every deployed device and force one pointless
   discovery republish. */
#define NVS_KEY_DISC_NAME "disc_name"
/* OTA. The first two are settings (HA-editable); the rest are
   device-owned state written by the update flow and read by the stat
   payload — accessors only, no bulk-document key. */
#define NVS_KEY_OTA_URL "ota_url"
#define NVS_KEY_OTA_ON_SYNC "ota_on_sync"
#define NVS_KEY_OTA_RESULT "ota_result"
#define NVS_KEY_OTA_TARGET "ota_target"
#define NVS_KEY_OTA_FAILS "ota_fails"
/* Last download's wall time, ms. In NVS rather than RAM because the
   download runs in the SECOND window, after MQTT has closed, and its
   success path ends in esp_restart() — so a RAM copy is discarded before
   anything can publish it and reads back 0 forever. See ota_flow.c's
   stop_clock(). Published one wake later, like ota_result. */
#define NVS_KEY_OTA_DL_MS "ota_dl_ms"
/* The version that was COMMITTED and has not yet been certified — empty
   when there is nothing outstanding. Written just before the OTA reboot,
   cleared either by the certification that follows it or by the boot that
   finds the device running something else, which is what a rollback looks
   like from inside the firmware. Consumed on read, so it reports one
   revert once and cannot latch.

   One key rather than a flag plus a version, because the two would be one
   fact stored twice: "armed with no version" and "a version with nothing
   armed" are both unrepresentable here, and the second of those was a
   real defect — the detector used to borrow NVS_KEY_OTA_TARGET, which is
   the RETRY BUDGET's key and is re-pointed by charge_the_attempt() before
   any download, so an attempt made between a commit and its revert moved
   the record of what had been committed. See ota_flow.c. */
#define NVS_KEY_OTA_PEND_VER "ota_pend_ver"
/* Panic forensics (panic_diag.c). Device-owned bookkeeping written and
   read through hal_nvs directly rather than through an nvs_config
   accessor pair — the same arrangement mqtt_ha.c already uses for
   NVS_KEY_DISC_VER, and for the same reason: these are not settings.
   They are never seeded, never defaulted, never editable from Home
   Assistant, and nvs_config's registry (nvs_defaults.h) would have to
   grow rows that exist only to leave them alone.

   panic_cnt is monotonic and is never reset by this firmware: HA reads
   the rate as the difference between two publishes, and a counter the
   device can zero cannot be differenced. panic_rec is one
   panic_diag_rec_t blob — the breadcrumb latched out of RTC memory on
   the boot after a panic, kept here because RTC memory survives to the
   NEXT boot only and this device does not open a network window on
   every wake. */
#define NVS_KEY_PANIC_CNT "panic_cnt"
#define NVS_KEY_PANIC_REC "panic_rec"
/* The chore checklist (chore_store.c). Two blobs, on opposite sides of the
   settings/state line drawn above: `chores` is the name list, written by
   config_apply from the HA config document (document-only — no accessor
   pair, no registry row, no HA entity of its own), while `chore_ack` is
   the day-stamped ack record, device-owned state like ota_result — written
   on every ack toggle, never editable from Home Assistant, never
   published. Both go through hal_nvs directly for the same reason
   panic_cnt does: nvs_config's registry would have to grow rows that exist
   only to leave them alone.
   6 and 9 chars — inside the 15-char NVS cap noted at the top. */
#define NVS_KEY_CHORES "chores"
#define NVS_KEY_CHORE_ACK "chore_ack"
/* The chore gate's free slice, one key per day type, mirroring the four
   allocation keys at the top of this file. Minutes, like those — the
   seconds conversion happens in schedule.c at the same point the
   allocation's does. 13-14 chars, inside the 15-char cap noted above.

   Settings, HA-editable, but DELIBERATELY absent from the seeded-defaults
   registry — see the NVS_DEFAULT_CHORE_FREE_WD comment in nvs_defaults.h
   for why adding them there would reseed every deployed device. */
#define NVS_KEY_CHORE_FREE_WD "chore_free_wd"
#define NVS_KEY_CHORE_FREE_WE "chore_free_we"
#define NVS_KEY_CHORE_FREE_HOL "chore_free_hol"
#define NVS_KEY_CHORE_FREE_SUM "chore_free_sum"
