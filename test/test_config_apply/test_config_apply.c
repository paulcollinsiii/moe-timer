#include <string.h>
#include <unity.h>

/* Single-TU: cJSON + the config applier over the mock NVS + real
   nvs_config accessors (so validation and persistence are exercised
   end-to-end without hardware). */
// clang-format off
#include "cJSON.h"
#include "mock_hal_nvs.c"
#include "../../main/nvs_config.c"
#include "../../main/config_apply.c"
// clang-format on

void setUp(void) {
    mock_nvs_reset();
}
void tearDown(void) {}

static config_result_t apply(const char *json, char *ack, size_t acklen) {
    return config_apply(json, ack, acklen);
}

/* ---- version gating ---- */

void test_full_document_applies_and_stores_ver(void) {
    char ack[256];
    const char *doc =
        "{\"ver\":\"20260708\",\"name\":\"Kitchen\",\"tz\":\"GMT0\","
        "\"weekday_min\":45,\"weekend_min\":90,\"holiday_min\":100,\"summer_min\":110,"
        "\"quiet_start\":2100,\"quiet_end\":700,\"break_interval_min\":40,\"break_duration_min\":10}";
    TEST_ASSERT_EQUAL(CONFIG_APPLIED, apply(doc, ack, sizeof(ack)));

    uint16_t v;
    nvs_config_get_weekday_min(&v);
    TEST_ASSERT_EQUAL_UINT16(45, v);
    nvs_config_get_break_interval_min(&v);
    TEST_ASSERT_EQUAL_UINT16(40, v);
    char s[64];
    nvs_config_get_tz(s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("GMT0", s);
    nvs_config_get_dev_name(s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("Kitchen", s);
    nvs_config_get_cfg_ver(s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("20260708", s);
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"ok\":true"));
}

void test_same_ver_skips_without_writing(void) {
    char ack[256];
    apply("{\"ver\":\"1\",\"weekday_min\":45}", ack, sizeof(ack));
    /* Second document, same ver, different value — must be ignored */
    TEST_ASSERT_EQUAL(CONFIG_SKIPPED, apply("{\"ver\":\"1\",\"weekday_min\":99}", ack, sizeof(ack)));
    uint16_t v;
    nvs_config_get_weekday_min(&v);
    TEST_ASSERT_EQUAL_UINT16(45, v);
}

void test_new_ver_reapplies(void) {
    char ack[256];
    apply("{\"ver\":\"1\",\"weekday_min\":45}", ack, sizeof(ack));
    TEST_ASSERT_EQUAL(CONFIG_APPLIED, apply("{\"ver\":\"2\",\"weekday_min\":99}", ack, sizeof(ack)));
    uint16_t v;
    nvs_config_get_weekday_min(&v);
    TEST_ASSERT_EQUAL_UINT16(99, v);
}

/* ---- partial + invalid fields ---- */

void test_partial_document_touches_only_present_fields(void) {
    char ack[256];
    apply("{\"ver\":\"1\",\"weekday_min\":45,\"weekend_min\":90}", ack, sizeof(ack));
    apply("{\"ver\":\"2\",\"weekday_min\":30}", ack, sizeof(ack));
    uint16_t v;
    nvs_config_get_weekday_min(&v);
    TEST_ASSERT_EQUAL_UINT16(30, v);
    nvs_config_get_weekend_min(&v); /* untouched by the second doc */
    TEST_ASSERT_EQUAL_UINT16(90, v);
}

void test_invalid_field_reported_but_others_apply(void) {
    char ack[256];
    /* weekday_min out of range (>1440); weekend_min valid */
    config_result_t r = apply("{\"ver\":\"1\",\"weekday_min\":5000,\"weekend_min\":90}", ack, sizeof(ack));
    TEST_ASSERT_EQUAL(CONFIG_APPLIED, r); /* applied with errors */
    uint16_t v;
    nvs_config_get_weekend_min(&v);
    TEST_ASSERT_EQUAL_UINT16(90, v);
    nvs_config_get_weekday_min(&v); /* rejected: still the default */
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_WEEKDAY_MIN, v);
    TEST_ASSERT_NOT_NULL(strstr(ack, "weekday_min")); /* named in the error list */
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"ok\":false"));
}

void test_bad_date_shape_rejected(void) {
    char ack[256];
    apply("{\"ver\":\"1\",\"school_start\":\"not-a-date\"}", ack, sizeof(ack));
    char s[16];
    nvs_config_get_school_start(s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING(NVS_DEFAULT_SCHOOL_START, s); /* unchanged */
    TEST_ASSERT_NOT_NULL(strstr(ack, "school_start"));
}

/* ---- malformed / missing ver ---- */

void test_missing_ver_is_invalid(void) {
    char ack[256];
    TEST_ASSERT_EQUAL(CONFIG_INVALID, apply("{\"weekday_min\":45}", ack, sizeof(ack)));
    uint16_t v;
    nvs_config_get_weekday_min(&v);
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_WEEKDAY_MIN, v);
}

void test_malformed_json_is_invalid_no_crash(void) {
    char ack[256];
    TEST_ASSERT_EQUAL(CONFIG_INVALID, apply("{\"ver\":\"1\", not json", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(CONFIG_INVALID, apply("", ack, sizeof(ack)));
}

/* ---- holidays array -> newline blob ---- */

void test_holidays_array_becomes_newline_blob(void) {
    char ack[256];
    apply("{\"ver\":\"1\",\"holidays\":[\"2026-10-16\",\"2026-11-03\",\"2026-12-25\"]}", ack, sizeof(ack));
    char blob[512];
    size_t len = sizeof(blob);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_holidays(blob, &len));
    blob[len] = '\0';
    TEST_ASSERT_EQUAL_STRING("2026-10-16\n2026-11-03\n2026-12-25\n", blob);
}

void test_holidays_reject_bad_entries_keep_good(void) {
    char ack[256];
    apply("{\"ver\":\"1\",\"holidays\":[\"2026-10-16\",\"garbage\",\"2026-12-25\"]}", ack, sizeof(ack));
    char blob[512];
    size_t len = sizeof(blob);
    nvs_config_get_holidays(blob, &len);
    blob[len] = '\0';
    TEST_ASSERT_EQUAL_STRING("2026-10-16\n2026-12-25\n", blob);
    TEST_ASSERT_NOT_NULL(strstr(ack, "holidays"));
}

void test_holidays_cap_enforced(void) {
    /* Build 60 dates (60*11 = 660 bytes) — must cap at <=512 without
       overflowing and without a partial trailing date. */
    char doc[1200];
    int pos = snprintf(doc, sizeof(doc), "{\"ver\":\"1\",\"holidays\":[");
    for (int i = 0; i < 60; i++) {
        pos += snprintf(doc + pos, sizeof(doc) - pos, "%s\"2026-01-%02d\"", i ? "," : "", (i % 28) + 1);
    }
    snprintf(doc + pos, sizeof(doc) - pos, "]}");
    char ack[256];
    apply(doc, ack, sizeof(ack));
    char blob[512];
    size_t len = sizeof(blob);
    nvs_config_get_holidays(blob, &len);
    TEST_ASSERT_LESS_OR_EQUAL_UINT(512, len);
    /* Every stored line is a complete 10-char date + newline */
    TEST_ASSERT_EQUAL_UINT(0, len % 11);
}

/* ---- timers array -> blob ---- */

void test_timers_array_maps_to_blob(void) {
    char ack[256];
    apply(
        "{\"ver\":\"1\",\"timers\":[{\"name\":\"Piano\",\"min\":20,\"reload\":true},{},"
        "{\"name\":\"Meditation\",\"min\":10,\"reload\":true},{}]}",
        ack, sizeof(ack));
    nvs_timer_defs_blob_t defs;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&defs));
    TEST_ASSERT_EQUAL_STRING("Piano", defs.defs[0].name);
    TEST_ASSERT_EQUAL_INT32(20, defs.defs[0].min);
    TEST_ASSERT_EQUAL_UINT8(1, defs.defs[0].reload);
    TEST_ASSERT_EQUAL_STRING("", defs.defs[1].name); /* {} = disabled */
    TEST_ASSERT_EQUAL_STRING("Meditation", defs.defs[2].name);
    TEST_ASSERT_EQUAL_UINT8(1, defs.defs[2].reload); /* reload: true */
    TEST_ASSERT_EQUAL_STRING("", defs.defs[3].name); /* {} = disabled */
}

void test_timers_overlong_name_rejected(void) {
    char ack[256];
    /* 20-char name won't fit the 15-char field */
    apply("{\"ver\":\"1\",\"timers\":[{\"name\":\"ThisNameIsWayTooLong\",\"min\":20}]}", ack, sizeof(ack));
    TEST_ASSERT_NOT_NULL(strstr(ack, "timers"));
    /* Blob not written (whole array rejected on a bad entry) */
    nvs_timer_defs_blob_t defs;
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, nvs_config_get_timer_defs(&defs));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_full_document_applies_and_stores_ver);
    RUN_TEST(test_same_ver_skips_without_writing);
    RUN_TEST(test_new_ver_reapplies);
    RUN_TEST(test_partial_document_touches_only_present_fields);
    RUN_TEST(test_invalid_field_reported_but_others_apply);
    RUN_TEST(test_bad_date_shape_rejected);
    RUN_TEST(test_missing_ver_is_invalid);
    RUN_TEST(test_malformed_json_is_invalid_no_crash);
    RUN_TEST(test_holidays_array_becomes_newline_blob);
    RUN_TEST(test_holidays_reject_bad_entries_keep_good);
    RUN_TEST(test_holidays_cap_enforced);
    RUN_TEST(test_timers_array_maps_to_blob);
    RUN_TEST(test_timers_overlong_name_rejected);
    return UNITY_END();
}
