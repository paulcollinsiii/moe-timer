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

void test_fingerprint_folds_in_credentials(void) {
    /* Regression: a changed WiFi/MQTT default must change the fingerprint,
       so setting NVS_DEFAULT_MQTT_URI after the first seed actually reseeds
       (the key already exists as "" and init-if-missing would skip it). */
    uint16_t base = fingerprint_compute(3, 60, 120, 120, 120, "ssid", "pass", "", "", "");
    uint16_t with_uri = fingerprint_compute(3, 60, 120, 120, 120, "ssid", "pass", "mqtt://ha:1883", "", "");
    uint16_t other_ssid = fingerprint_compute(3, 60, 120, 120, 120, "other", "pass", "", "", "");
    TEST_ASSERT_NOT_EQUAL(base, with_uri);
    TEST_ASSERT_NOT_EQUAL(base, other_ssid);
    /* deterministic */
    TEST_ASSERT_EQUAL_UINT16(with_uri,
                             fingerprint_compute(3, 60, 120, 120, 120, "ssid", "pass", "mqtt://ha:1883", "", ""));
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
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_timer_defs(&defs));

    nvs_timer_defs_blob_t out;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&out));
    TEST_ASSERT_EQUAL_STRING("Piano", out.defs[0].name);
    TEST_ASSERT_EQUAL_INT32(20, out.defs[0].min);
    TEST_ASSERT_EQUAL_UINT8(1, out.defs[0].reload);
    TEST_ASSERT_EQUAL_STRING("", out.defs[1].name); /* disabled slot */
}

void test_timer_defs_blob_missing_or_stale_version(void) {
    nvs_timer_defs_blob_t out;
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, nvs_config_get_timer_defs(&out));
    nvs_timer_defs_blob_t defs = {.version = 99};
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

int main(void) {
    UNITY_BEGIN();
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
    RUN_TEST(test_reseed_clears_cfg_ver);
    RUN_TEST(test_mqtt_settings_round_trip);
    RUN_TEST(test_mqtt_settings_missing_read_as_empty);
    RUN_TEST(test_init_defaults_seeds_mqtt_keys);
    RUN_TEST(test_timer_snapshot_save_propagates_write_failure);
    RUN_TEST(test_set_weekday_min_propagates_write_failure);
    RUN_TEST(test_timer_snapshot_round_trip);
    RUN_TEST(test_timer_snapshot_missing_returns_not_found);
    RUN_TEST(test_timer_snapshot_rejects_wrong_version);
    return UNITY_END();
}
