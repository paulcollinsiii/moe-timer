#include <stdint.h>
#include <string.h>
#include <unity.h>

/* Single-TU compilation */
#include "../../main/nvs_config.c"
#include "mock_hal_nvs.c"

void setUp(void) {
    mock_nvs_reset(); /* start each test with blank NVS */
}

void tearDown(void) {}

/* ------------------------------------------------------------------ */
/* nvs_config_init_defaults                                            */
/* ------------------------------------------------------------------ */

void test_init_defaults_writes_weekday_min(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_init_defaults());
    uint16_t val = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_weekday_min(&val));
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_WEEKDAY_MIN, val);
}

void test_init_defaults_writes_weekend_min(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_init_defaults());
    uint16_t val = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_weekend_min(&val));
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_WEEKEND_MIN, val);
}

void test_init_defaults_writes_holiday_min(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_init_defaults());
    uint16_t val = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_holiday_min(&val));
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_HOLIDAY_MIN, val);
}

void test_init_defaults_writes_holiday_blob(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_init_defaults());
    char buf[2048];
    size_t len = sizeof(buf);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_holidays(buf, &len));
    /* Blob must contain at least one YYYY-MM-DD date */
    TEST_ASSERT_TRUE(len >= 10);
    /* Dublin 2026-27 calendar: New Year's Day 2027 (winter break) present;
       Veterans Day 2026 (school in session) must NOT be */
    TEST_ASSERT_NOT_NULL(strstr(buf, "2027-01-01"));
    TEST_ASSERT_NULL(strstr(buf, "2026-11-11"));
}

void test_summer_min_round_trip(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_summer_min(90));
    uint16_t val = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_summer_min(&val));
    TEST_ASSERT_EQUAL_UINT16(90, val);
}

void test_init_defaults_writes_summer_min(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_init_defaults());
    uint16_t val = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_summer_min(&val));
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_SUMMER_MIN, val);
}

void test_init_defaults_is_idempotent(void) {
    /* Call twice — second call must not overwrite user-modified values */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_init_defaults());
    /* Modify a value */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_weekday_min(45));
    /* Call defaults again */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_init_defaults());
    /* Value must NOT revert to default */
    uint16_t val = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_weekday_min(&val));
    TEST_ASSERT_EQUAL_UINT16(45, val);
}

/* ------------------------------------------------------------------ */
/* u16 round-trips                                                      */
/* ------------------------------------------------------------------ */

void test_weekday_min_round_trip(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_weekday_min(45));
    uint16_t val = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_weekday_min(&val));
    TEST_ASSERT_EQUAL_UINT16(45, val);
}

void test_weekend_min_round_trip(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_weekend_min(90));
    uint16_t val = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_weekend_min(&val));
    TEST_ASSERT_EQUAL_UINT16(90, val);
}

void test_holiday_min_round_trip(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_holiday_min(180));
    uint16_t val = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_holiday_min(&val));
    TEST_ASSERT_EQUAL_UINT16(180, val);
}

/* ------------------------------------------------------------------ */
/* String round-trips                                                   */
/* ------------------------------------------------------------------ */

void test_wifi_ssid_round_trip(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_wifi_ssid("MyHomeNetwork"));
    char buf[64] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_wifi_ssid(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("MyHomeNetwork", buf);
}

void test_wifi_pass_round_trip(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_wifi_pass("hunter2"));
    char buf[64] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_wifi_pass(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("hunter2", buf);
}

void test_wifi_ssid_max_length(void) {
    /* SSID max is 32 bytes per 802.11 */
    char long_ssid[33];
    memset(long_ssid, 'A', 32);
    long_ssid[32] = '\0';
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_wifi_ssid(long_ssid));
    char buf[64] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_wifi_ssid(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING(long_ssid, buf);
}

/* ------------------------------------------------------------------ */
/* Blob round-trip                                                      */
/* ------------------------------------------------------------------ */

void test_holidays_blob_round_trip(void) {
    const char *holidays = "2026-01-01\n2026-07-04\n2026-12-25\n";
    size_t write_len = strlen(holidays);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_holidays(holidays, write_len));

    char buf[256] = {0};
    size_t read_len = sizeof(buf);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_holidays(buf, &read_len));
    TEST_ASSERT_EQUAL_UINT32(write_len, read_len);
    TEST_ASSERT_EQUAL_MEMORY(holidays, buf, write_len);
}

/* ------------------------------------------------------------------ */
/* Missing key fallback                                                  */
/* ------------------------------------------------------------------ */

void test_get_weekday_min_missing_returns_default(void) {
    uint16_t val = 0;
    nvs_config_get_weekday_min(&val); /* no write beforehand */
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_WEEKDAY_MIN, val);
}

void test_get_tones_missing_return_defaults_and_roundtrip(void) {
    uint16_t val = 999;
    nvs_config_get_tone_expiry(&val);
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_TONE_EXPIRY, val);
    nvs_config_get_tone_break(&val);
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_TONE_BREAK, val);
    nvs_config_get_tone_bed(&val);
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_TONE_BED, val);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_tone_bed(TONE_CLASSIC));
    nvs_config_get_tone_bed(&val);
    TEST_ASSERT_EQUAL_UINT16(TONE_CLASSIC, val);
}

void test_alert_volume_missing_returns_default_and_roundtrip(void) {
    uint16_t val = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_alert_volume(&val));
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_ALERT_VOLUME, val);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_alert_volume(75));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_alert_volume(&val));
    TEST_ASSERT_EQUAL_UINT16(75, val);
}

/* ------------------------------------------------------------------ */
/* Runner                                                               */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* defaults version stamp                                              */
/* ------------------------------------------------------------------ */

void test_init_defaults_writes_fingerprint_stamp(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_init_defaults());
    uint16_t ver = 0;
    TEST_ASSERT_EQUAL(ESP_OK, hal_nvs_read_u16("defaults_ver", &ver));
    /* Stamp is a fingerprint of the compile-time defaults, so a menuconfig
       change to any allocation reseeds without a manual version bump */
    TEST_ASSERT_EQUAL_UINT16(nvs_config_defaults_fingerprint(), ver);
}

void test_defaults_fingerprint_is_nonzero_and_stable(void) {
    /* 0 would collide with blank NVS; stability keeps reseed idempotent */
    TEST_ASSERT_NOT_EQUAL(0, nvs_config_defaults_fingerprint());
    TEST_ASSERT_EQUAL_UINT16(nvs_config_defaults_fingerprint(), nvs_config_defaults_fingerprint());
}

/* Reference re-implementation of the fingerprint fold, used for property
   tests with arbitrary inputs. The pinned characterization test below
   proves the production registry-driven fold matches this algorithm on
   the real compile-time defaults. */
static uint16_t ref_fingerprint(uint32_t version, uint16_t wd, uint16_t we, uint16_t ho, uint16_t su, const char *ssid,
                                const char *pass, const char *uri, const char *user, const char *mpass) {
    uint32_t fp = version;
    fp = fp * 31u + wd;
    fp = fp * 31u + we;
    fp = fp * 31u + ho;
    fp = fp * 31u + su;
    const char *strs[] = {ssid, pass, uri, user, mpass};
    for (size_t i = 0; i < sizeof(strs) / sizeof(strs[0]); i++) {
        for (const char *s = strs[i]; s != NULL && *s != '\0'; s++) {
            fp = fp * 31u + (unsigned char)*s;
        }
    }
    uint16_t out = (uint16_t)(fp ^ (fp >> 16));
    return (out == 0) ? 1 : out;
}

void test_fingerprint_folds_in_credentials(void) {
    /* Regression: a changed WiFi/MQTT default must change the fingerprint,
       so setting NVS_DEFAULT_MQTT_URI after the first seed actually reseeds
       (the key already exists as "" and init-if-missing would skip it). */
    uint16_t base = ref_fingerprint(3, 60, 120, 120, 120, "ssid", "pass", "", "", "");
    uint16_t with_uri = ref_fingerprint(3, 60, 120, 120, 120, "ssid", "pass", "mqtt://ha:1883", "", "");
    uint16_t other_ssid = ref_fingerprint(3, 60, 120, 120, 120, "other", "pass", "", "", "");
    TEST_ASSERT_NOT_EQUAL(base, with_uri);
    TEST_ASSERT_NOT_EQUAL(base, other_ssid);
    /* deterministic */
    TEST_ASSERT_EQUAL_UINT16(with_uri, ref_fingerprint(3, 60, 120, 120, 120, "ssid", "pass", "mqtt://ha:1883", "", ""));
}

void test_init_defaults_reseeds_on_fingerprint_change(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_init_defaults());
    /* Simulate values seeded by a build with different compile-time defaults */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_weekday_min(99));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_wifi_ssid("old-ssid"));
    TEST_ASSERT_EQUAL(ESP_OK, hal_nvs_write_u16("defaults_ver", (uint16_t)(nvs_config_defaults_fingerprint() - 1)));

    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_init_defaults());

    uint16_t val = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_weekday_min(&val));
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_WEEKDAY_MIN, val);
    char ssid[64];
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_wifi_ssid(ssid, sizeof(ssid)));
    TEST_ASSERT_EQUAL_STRING(NVS_DEFAULT_WIFI_SSID, ssid);
    uint16_t ver = 0;
    TEST_ASSERT_EQUAL(ESP_OK, hal_nvs_read_u16("defaults_ver", &ver));
    TEST_ASSERT_EQUAL_UINT16(nvs_config_defaults_fingerprint(), ver);
}

void test_init_defaults_missing_version_key_reseeds(void) {
    /* Field devices seeded before the stamp existed: no defaults_ver key */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_weekday_min(45));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_init_defaults());
    uint16_t val = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_weekday_min(&val));
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_WEEKDAY_MIN, val);
}

void test_init_defaults_same_version_preserves_values(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_init_defaults());
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_weekday_min(99));

    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_init_defaults());

    uint16_t val = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_weekday_min(&val));
    TEST_ASSERT_EQUAL_UINT16(99, val); /* not clobbered */
}

/* ------------------------------------------------------------------ */
/* MQTT broker settings (HA integration)                               */
/* ------------------------------------------------------------------ */

void test_mqtt_settings_round_trip(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_mqtt_uri("mqtt://ha.local:1883"));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_mqtt_user("magtag"));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_mqtt_pass("hunter2"));
    char buf[96];
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_mqtt_uri(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("mqtt://ha.local:1883", buf);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_mqtt_user(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("magtag", buf);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_mqtt_pass(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("hunter2", buf);
}

void test_mqtt_settings_missing_read_as_empty(void) {
    char buf[96] = "junk";
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_mqtt_uri(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("", buf); /* empty = MQTT disabled */
}

void test_init_defaults_seeds_mqtt_keys(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_init_defaults());
    char buf[96];
    size_t len = sizeof(buf);
    /* Key exists after seeding (value = compile-time default) */
    TEST_ASSERT_EQUAL(ESP_OK, hal_nvs_read_str("mqtt_uri", buf, &len));
}

/* ------------------------------------------------------------------ */
/* HA config-in keys (phase 2)                                         */
/* ------------------------------------------------------------------ */

void test_tz_defaults_and_round_trip(void) {
    char buf[64];
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_tz(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING(NVS_DEFAULT_TZ, buf); /* compile-time default */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_tz("GMT0IST,M3.5.0/1,M10.5.0"));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_tz(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("GMT0IST,M3.5.0/1,M10.5.0", buf);
}

void test_quiet_hours_defaults_and_round_trip(void) {
    uint16_t v = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_quiet_start(&v));
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_QUIET_START, v);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_quiet_start(2100));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_quiet_start(&v));
    TEST_ASSERT_EQUAL_UINT16(2100, v);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_quiet_end(&v));
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_QUIET_END, v);
}

void test_break_settings_defaults_and_round_trip(void) {
    uint16_t v = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_break_interval_min(&v));
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_BREAK_INTERVAL_MIN, v);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_break_interval_min(45));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_break_interval_min(&v));
    TEST_ASSERT_EQUAL_UINT16(45, v);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_break_duration_min(&v));
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_BREAK_DURATION_MIN, v);
}

void test_school_window_defaults_and_round_trip(void) {
    char buf[16];
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_school_start(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING(NVS_DEFAULT_SCHOOL_START, buf);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_school_start("2027-08-19"));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_school_start(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("2027-08-19", buf);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_school_end(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING(NVS_DEFAULT_SCHOOL_END, buf);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_summer_start(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING(NVS_DEFAULT_SUMMER_START, buf);
}

void test_cfg_ver_round_trip(void) {
    char buf[24];
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_cfg_ver(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("", buf); /* never applied */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_cfg_ver("20260708"));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_cfg_ver(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("20260708", buf);
}

void test_timer_defs_blob_round_trip(void) {
    nvs_timer_defs_blob_t defs = {.version = TIMER_DEFS_BLOB_VERSION};
    snprintf(defs.defs[0].name, sizeof(defs.defs[0].name), "Piano");
    defs.defs[0].min = 20;
    defs.defs[0].reload = 1;
    defs.defs[0].break_eligible = 1; /* v2 */
    snprintf(defs.defs[1].name, sizeof(defs.defs[1].name), "Laundry");
    defs.defs[1].min = 30;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_timer_defs(&defs));

    nvs_timer_defs_blob_t out;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&out));
    TEST_ASSERT_EQUAL_STRING("Piano", out.defs[0].name);
    TEST_ASSERT_EQUAL_INT32(20, out.defs[0].min);
    TEST_ASSERT_EQUAL_UINT8(1, out.defs[0].reload);
    TEST_ASSERT_EQUAL_UINT8(1, out.defs[0].break_eligible);
    TEST_ASSERT_EQUAL_STRING("Laundry", out.defs[1].name);
    TEST_ASSERT_EQUAL_UINT8(0, out.defs[1].break_eligible); /* chore: not a break */
    TEST_ASSERT_EQUAL_STRING("", out.defs[2].name);         /* disabled slot */
}

void test_timer_defs_blob_missing_or_stale_version(void) {
    nvs_timer_defs_blob_t out;
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, nvs_config_get_timer_defs(&out));
    nvs_timer_defs_blob_t defs = {.version = 99};
    hal_nvs_write_blob("timer_defs", &defs, sizeof(defs));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_VERSION, nvs_config_get_timer_defs(&out));
}

void test_timer_defs_blob_v1_reads_as_stale(void) {
    /* The v1 layout had no break_eligible byte. Version drift (and, on a
       device that somehow kept the old size, size drift) must read as
       stale so timer_defs_install falls back to the Kconfig table rather
       than reinterpreting the old bytes. */
    nvs_timer_defs_blob_t out;
    nvs_timer_defs_blob_t defs = {.version = 1};
    hal_nvs_write_blob("timer_defs", &defs, sizeof(defs));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_VERSION, nvs_config_get_timer_defs(&out));
}

void test_reseed_clears_cfg_ver(void) {
    /* A Kconfig-fingerprint reseed overwrites HA-managed keys; clearing
       cfg_ver makes the retained HA config re-apply on the next window,
       so HA stays source-of-truth after a reflash. */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_init_defaults());
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_cfg_ver("20260708"));
    /* Force a reseed: corrupt the stored fingerprint stamp */
    hal_nvs_write_u16("defaults_ver", 0x5555);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_init_defaults());
    char buf[24] = "junk";
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_cfg_ver(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("", buf);
}

/* ------------------------------------------------------------------ */
/* OTA keys                                                            */
/* ------------------------------------------------------------------ */

void test_ota_url_defaults_and_round_trip(void) {
    char buf[160] = "junk";
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_ota_url(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING(NVS_DEFAULT_OTA_URL, buf);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_url("https://example.com/ota.json"));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_ota_url(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("https://example.com/ota.json", buf);
    /* Empty is a real stored value (OTA off), not "unset" — it must not
       fall back to the compile-time default. */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_url(""));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_ota_url(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("", buf);
}

void test_ota_on_sync_defaults_and_round_trip(void) {
    uint16_t v = 0xFFFF;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_ota_on_sync(&v));
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_OTA_ON_SYNC, v);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_on_sync(1));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_ota_on_sync(&v));
    TEST_ASSERT_EQUAL_UINT16(1, v);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_on_sync(0));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_ota_on_sync(&v));
    TEST_ASSERT_EQUAL_UINT16(0, v);
}

/* Device-owned state: written by the firmware, read by the stat payload.
   No HA entity, no bulk-document key — just accessors. */
void test_ota_state_keys_default_empty_and_round_trip(void) {
    char buf[40] = "junk";
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_ota_result(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("", buf);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_ota_target(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("", buf);
    uint16_t fails = 0xFFFF;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_ota_fails(&fails));
    TEST_ASSERT_EQUAL_UINT16(0, fails);

    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_result("tls_cert"));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_target("1.6.0"));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_fails(2));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_ota_result(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("tls_cert", buf);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_ota_target(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("1.6.0", buf);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_ota_fails(&fails));
    TEST_ASSERT_EQUAL_UINT16(2, fails);
}

/* The download duration is a u32 and that is the point of it: the
   download budget is CONFIG_MAGTAG_OTA_MAX_SEC (300 s in the shipped
   Kconfig), and a u16 stops counting at 65.5 s — it would saturate on
   exactly the slow transfers the field exists to expose. */
void test_ota_dl_ms_defaults_to_zero_and_holds_a_full_download(void) {
    uint32_t ms = 0xDEADBEEF;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_ota_dl_ms(&ms));
    TEST_ASSERT_EQUAL_UINT32(0, ms);

    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_dl_ms(298000));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_ota_dl_ms(&ms));
    TEST_ASSERT_EQUAL_UINT32(298000, ms);

    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_dl_ms(4294967295u));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_ota_dl_ms(&ms));
    TEST_ASSERT_EQUAL_UINT32(4294967295u, ms);
}

/* The certification token. Absent means "no image is awaiting
   certification", which is the state of every device that has never run
   an OTA — so the default has to be 0 rather than an error, or every
   virgin boot would take the unreadable branch. Normalised to 0/1 like
   ota_on_sync so a stray value cannot read as a third state. */
void test_ota_pend_defaults_to_zero_and_normalises(void) {
    uint16_t pend = 0xFFFF;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_ota_pend(&pend));
    TEST_ASSERT_EQUAL_UINT16(0, pend);

    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_pend(1));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_ota_pend(&pend));
    TEST_ASSERT_EQUAL_UINT16(1, pend);

    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_pend(99));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_ota_pend(&pend));
    TEST_ASSERT_EQUAL_UINT16(1, pend);

    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_pend(0));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_ota_pend(&pend));
    TEST_ASSERT_EQUAL_UINT16(0, pend);
}

/* The OTA keys are deliberately NOT in the seeded-defaults registry, so
   init_defaults never materializes them; the getters supply the
   compile-time default lazily instead. */
void test_init_defaults_does_not_seed_ota_keys(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_init_defaults());
    char buf[160];
    size_t len = sizeof(buf);
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, hal_nvs_read_str("ota_url", buf, &len));
    uint16_t v;
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, hal_nvs_read_u16("ota_on_sync", &v));
}

/* THE regression this exclusion exists for: a menuconfig edit anywhere in
   the allocation defaults changes the fingerprint and reseeds NVS. If the
   OTA keys were fingerprinted (or seeded), that reseed would silently
   revert an HA-set endpoint and check-on-sync flag on the next boot —
   the opposite of what a runtime override is for. */
void test_reseed_does_not_revert_ha_set_ota_values(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_init_defaults());
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_url("https://ha.example/ota.json"));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_on_sync(1));
    /* Force a reseed the way a changed Kconfig default would */
    hal_nvs_write_u16("defaults_ver", 0x5555);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_init_defaults());

    char buf[160];
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_ota_url(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("https://ha.example/ota.json", buf);
    uint16_t v;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_ota_on_sync(&v));
    TEST_ASSERT_EQUAL_UINT16(1, v);
}

/* Guards the "no HA-managed key in the fold" rule from the other side: if
   someone adds an OTA row to NVS_SEEDED_*, the fingerprint changes and
   this pin fails alongside test_defaults_fingerprint_algorithm_pinned. */
void test_fingerprint_ignores_ota_values(void) {
    uint16_t before = nvs_config_defaults_fingerprint();
    nvs_config_set_ota_url("https://elsewhere.example/ota.json");
    nvs_config_set_ota_on_sync(1);
    TEST_ASSERT_EQUAL_UINT16(before, nvs_config_defaults_fingerprint());
}

/* An undersized read buffer is an ERROR that writes nothing — it is not a
   truncating read. get_str_empty_default maps only NOT_FOUND to "", so a
   caller that guesses low keeps whatever junk it started with. This is
   what the declared CFG_BOUND_OTA_* minimums on the getters are for, and
   it is why ota_target must be read at full width: a short read leaves the
   retry-budget comparison matching nothing, so a doomed version is retried
   forever. */
void test_short_read_buffer_errors_and_leaves_the_buffer_untouched(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_target("1.6.0-a-fairly-long-version"));
    char small[8];
    memset(small, 'Z', sizeof(small));
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_INVALID_LENGTH, nvs_config_get_ota_target(small, sizeof(small)));
    TEST_ASSERT_EQUAL_CHAR('Z', small[0]); /* untouched, not truncated */
}

void test_declared_buffer_size_reads_the_whole_value(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_target("1.6.0-a-fairly-long-version"));
    char buf[CFG_BOUND_OTA_TARGET_MAX];
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_ota_target(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("1.6.0-a-fairly-long-version", buf);
}

/* Setters reject rather than truncate: ota_flow.c will be a third writer
   that is not on either validating path. */
void test_ota_string_setters_reject_overlong_values(void) {
    char big[CFG_BOUND_OTA_URL_MAX + 16];
    memset(big, 'u', sizeof(big) - 1);
    memcpy(big, "https://", 8);
    big[sizeof(big) - 1] = '\0';
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, nvs_config_set_ota_url(big));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, nvs_config_set_ota_target(big));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, nvs_config_set_ota_result(big));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, nvs_config_set_ota_url(NULL));
    /* A rejected write must not have stored a partial value */
    char buf[CFG_BOUND_OTA_URL_MAX];
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_ota_url(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING(NVS_DEFAULT_OTA_URL, buf);
}

void test_ota_string_setters_accept_the_declared_maximum(void) {
    char url[CFG_BOUND_OTA_URL_MAX];
    memset(url, 'u', sizeof(url) - 1);
    memcpy(url, "https://", 8);
    url[sizeof(url) - 1] = '\0';
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_url(url));
    char buf[CFG_BOUND_OTA_URL_MAX];
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_ota_url(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING(url, buf);
}

/* cmd_apply.c reads the stored id into a 40-byte buffer and does not check
   the return. A longer id would store fine but never read back, so the
   apply-once compare would fail every window and a retained `grant` would
   re-apply forever. Bound the write instead. */
void test_cmd_id_is_bounded_to_the_dedup_buffer(void) {
    char big[CFG_BOUND_CMD_ID_MAX + 8];
    memset(big, 'i', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, nvs_config_set_cmd_id(big));
    char buf[CFG_BOUND_CMD_ID_MAX];
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_cmd_id(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("", buf); /* nothing stored */
}

void test_cmd_id_at_the_bound_round_trips(void) {
    char id[CFG_BOUND_CMD_ID_MAX];
    memset(id, 'i', sizeof(id) - 1);
    id[sizeof(id) - 1] = '\0';
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_cmd_id(id));
    char buf[CFG_BOUND_CMD_ID_MAX];
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_cmd_id(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING(id, buf);
}

/* ------------------------------------------------------------------ */
/* timer snapshot (crash/reset recovery)                               */
/* ------------------------------------------------------------------ */

void test_timer_snapshot_round_trip(void) {
    timer_snapshot_t snap = {
        .version = TIMER_SNAPSHOT_VERSION,
        .active_slot = 1,
        .slots[0] = {.state = 1, /* TIMER_RUNNING */
                     .remaining_at_pause = 0,
                     .allocation_sec = 3600,
                     .expiry_wall_time = 1767574800},
        .slots[1] = {.state = 2, /* TIMER_PAUSED */
                     .remaining_at_pause = 700,
                     .allocation_sec = 900,
                     .completions = 2},
        .date = "2026-01-05",
    };
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_save_timer_snapshot(&snap));

    timer_snapshot_t out;
    memset(&out, 0, sizeof(out));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_load_timer_snapshot(&out));
    TEST_ASSERT_EQUAL_UINT8(TIMER_SNAPSHOT_VERSION, out.version);
    TEST_ASSERT_EQUAL_UINT8(1, out.active_slot);
    TEST_ASSERT_EQUAL_UINT8(1, out.slots[0].state);
    TEST_ASSERT_EQUAL_INT32(3600, out.slots[0].allocation_sec);
    TEST_ASSERT_EQUAL_INT64(1767574800, out.slots[0].expiry_wall_time);
    TEST_ASSERT_EQUAL_UINT16(2, out.slots[1].completions);
    TEST_ASSERT_EQUAL_STRING("2026-01-05", out.date);
}

void test_timer_snapshot_missing_returns_not_found(void) {
    timer_snapshot_t out;
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, nvs_config_load_timer_snapshot(&out));
}

void test_timer_snapshot_save_propagates_write_failure(void) {
    /* The snapshot is the anti-refund mechanism — a silent save failure
       must at least surface as an error to the caller. */
    timer_snapshot_t snap = {.version = TIMER_SNAPSHOT_VERSION, .slots[0].state = 1, .date = "2026-01-05"};
    mock_nvs_fail_writes(1);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, nvs_config_save_timer_snapshot(&snap));
    /* Store untouched by the failed write */
    timer_snapshot_t out;
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, nvs_config_load_timer_snapshot(&out));
}

void test_set_weekday_min_propagates_write_failure(void) {
    mock_nvs_fail_writes(1);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, nvs_config_set_weekday_min(45));
    /* Store untouched: reads fall back to the compile-time default */
    uint16_t val = 0;
    nvs_config_get_weekday_min(&val);
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_WEEKDAY_MIN, val);
}

void test_timer_snapshot_rejects_wrong_version(void) {
    timer_snapshot_t snap = {.version = 99, .slots[0].state = 1, .date = "2026-01-05"};
    hal_nvs_write_blob("timer_snap", &snap, sizeof(snap));
    timer_snapshot_t out;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_VERSION, nvs_config_load_timer_snapshot(&out));
}

/* Characterization guard: the fingerprint fold ORDER and algorithm are
   load-bearing — a changed value on a deployed device triggers a full
   reseed that reverts every HA-managed key. This re-implements the
   historical fold inline; any registry refactor must keep producing an
   identical value. (Holidays are deliberately NOT folded.) */
void test_defaults_fingerprint_algorithm_pinned(void) {
    uint16_t expect =
        ref_fingerprint(NVS_DEFAULTS_VERSION, NVS_DEFAULT_WEEKDAY_MIN, NVS_DEFAULT_WEEKEND_MIN, NVS_DEFAULT_HOLIDAY_MIN,
                        NVS_DEFAULT_SUMMER_MIN, NVS_DEFAULT_WIFI_SSID, NVS_DEFAULT_WIFI_PASS, NVS_DEFAULT_MQTT_URI,
                        NVS_DEFAULT_MQTT_USER, NVS_DEFAULT_MQTT_PASS);
    TEST_ASSERT_EQUAL_UINT16(expect, nvs_config_defaults_fingerprint());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_defaults_fingerprint_algorithm_pinned);
    RUN_TEST(test_init_defaults_writes_weekday_min);
    RUN_TEST(test_init_defaults_writes_weekend_min);
    RUN_TEST(test_init_defaults_writes_holiday_min);
    RUN_TEST(test_init_defaults_writes_holiday_blob);
    RUN_TEST(test_summer_min_round_trip);
    RUN_TEST(test_init_defaults_writes_summer_min);
    RUN_TEST(test_init_defaults_is_idempotent);
    RUN_TEST(test_weekday_min_round_trip);
    RUN_TEST(test_weekend_min_round_trip);
    RUN_TEST(test_holiday_min_round_trip);
    RUN_TEST(test_wifi_ssid_round_trip);
    RUN_TEST(test_wifi_pass_round_trip);
    RUN_TEST(test_wifi_ssid_max_length);
    RUN_TEST(test_holidays_blob_round_trip);
    RUN_TEST(test_get_weekday_min_missing_returns_default);
    RUN_TEST(test_get_tones_missing_return_defaults_and_roundtrip);
    RUN_TEST(test_alert_volume_missing_returns_default_and_roundtrip);
    RUN_TEST(test_init_defaults_writes_fingerprint_stamp);
    RUN_TEST(test_defaults_fingerprint_is_nonzero_and_stable);
    RUN_TEST(test_fingerprint_folds_in_credentials);
    RUN_TEST(test_init_defaults_reseeds_on_fingerprint_change);
    RUN_TEST(test_init_defaults_missing_version_key_reseeds);
    RUN_TEST(test_init_defaults_same_version_preserves_values);
    RUN_TEST(test_tz_defaults_and_round_trip);
    RUN_TEST(test_quiet_hours_defaults_and_round_trip);
    RUN_TEST(test_break_settings_defaults_and_round_trip);
    RUN_TEST(test_school_window_defaults_and_round_trip);
    RUN_TEST(test_cfg_ver_round_trip);
    RUN_TEST(test_timer_defs_blob_round_trip);
    RUN_TEST(test_timer_defs_blob_missing_or_stale_version);
    RUN_TEST(test_timer_defs_blob_v1_reads_as_stale);
    RUN_TEST(test_reseed_clears_cfg_ver);
    RUN_TEST(test_mqtt_settings_round_trip);
    RUN_TEST(test_mqtt_settings_missing_read_as_empty);
    RUN_TEST(test_init_defaults_seeds_mqtt_keys);
    RUN_TEST(test_ota_url_defaults_and_round_trip);
    RUN_TEST(test_ota_on_sync_defaults_and_round_trip);
    RUN_TEST(test_ota_state_keys_default_empty_and_round_trip);
    RUN_TEST(test_ota_dl_ms_defaults_to_zero_and_holds_a_full_download);
    RUN_TEST(test_ota_pend_defaults_to_zero_and_normalises);
    RUN_TEST(test_init_defaults_does_not_seed_ota_keys);
    RUN_TEST(test_reseed_does_not_revert_ha_set_ota_values);
    RUN_TEST(test_fingerprint_ignores_ota_values);
    RUN_TEST(test_short_read_buffer_errors_and_leaves_the_buffer_untouched);
    RUN_TEST(test_declared_buffer_size_reads_the_whole_value);
    RUN_TEST(test_ota_string_setters_reject_overlong_values);
    RUN_TEST(test_ota_string_setters_accept_the_declared_maximum);
    RUN_TEST(test_cmd_id_is_bounded_to_the_dedup_buffer);
    RUN_TEST(test_cmd_id_at_the_bound_round_trips);
    RUN_TEST(test_timer_snapshot_save_propagates_write_failure);
    RUN_TEST(test_set_weekday_min_propagates_write_failure);
    RUN_TEST(test_timer_snapshot_round_trip);
    RUN_TEST(test_timer_snapshot_missing_returns_not_found);
    RUN_TEST(test_timer_snapshot_rejects_wrong_version);
    return UNITY_END();
}
