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

/* ---- is_iso_date: real calendar validity (mktime round-trip) ---- */

void test_is_iso_date_accepts_valid(void) {
    TEST_ASSERT_TRUE(is_iso_date("2026-08-20"));
    TEST_ASSERT_TRUE(is_iso_date("2026-01-01"));
    TEST_ASSERT_TRUE(is_iso_date("2026-12-31"));
    TEST_ASSERT_TRUE(is_iso_date("2028-02-29")); /* leap year */
}

void test_is_iso_date_rejects_bad_shape(void) {
    TEST_ASSERT_FALSE(is_iso_date(NULL));
    TEST_ASSERT_FALSE(is_iso_date(""));
    TEST_ASSERT_FALSE(is_iso_date("2026-8-20"));  /* not zero-padded */
    TEST_ASSERT_FALSE(is_iso_date("2026/08/20")); /* wrong separators */
    TEST_ASSERT_FALSE(is_iso_date("not-a-date"));
    TEST_ASSERT_FALSE(is_iso_date("2026-08-2x"));
}

void test_is_iso_date_rejects_impossible_calendar_dates(void) {
    TEST_ASSERT_FALSE(is_iso_date("2026-13-45")); /* month & day out of range */
    TEST_ASSERT_FALSE(is_iso_date("2026-00-10")); /* month 0 */
    TEST_ASSERT_FALSE(is_iso_date("2026-02-00")); /* day 0 */
    TEST_ASSERT_FALSE(is_iso_date("2026-02-31")); /* Feb has no 31st */
    TEST_ASSERT_FALSE(is_iso_date("2026-04-31")); /* April has 30 days */
    TEST_ASSERT_FALSE(is_iso_date("2026-02-29")); /* 2026 is not a leap year */
    TEST_ASSERT_FALSE(is_iso_date("2100-02-29")); /* century non-leap */
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

void test_valid_dates_applied(void) {
    char ack[256];
    apply(
        "{\"ver\":\"1\",\"summer_start\":\"2027-06-01\",\"school_start\":\"2027-08-18\","
        "\"school_end\":\"2028-05-26\"}",
        ack, sizeof(ack));
    char s[16];
    nvs_config_get_summer_start(s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("2027-06-01", s);
    nvs_config_get_school_start(s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("2027-08-18", s);
    nvs_config_get_school_end(s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("2028-05-26", s);
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"ok\":true"));
}

void test_impossible_calendar_date_rejected(void) {
    /* Correct shape, impossible month/day — must be rejected, not stored */
    char ack[256];
    apply("{\"ver\":\"1\",\"school_start\":\"2026-13-45\"}", ack, sizeof(ack));
    char s[16];
    nvs_config_get_school_start(s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING(NVS_DEFAULT_SCHOOL_START, s);
    TEST_ASSERT_NOT_NULL(strstr(ack, "school_start"));
}

void test_numeric_ver_accepted(void) {
    /* ver may arrive as a JSON number (now().timestamp()|int in HA) */
    char ack[256];
    TEST_ASSERT_EQUAL(CONFIG_APPLIED, apply("{\"ver\":20260708,\"weekday_min\":50}", ack, sizeof(ack)));
    char s[24];
    nvs_config_get_cfg_ver(s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("20260708", s);
    /* same numeric ver is then idempotent */
    TEST_ASSERT_EQUAL(CONFIG_SKIPPED, apply("{\"ver\":20260708,\"weekday_min\":77}", ack, sizeof(ack)));
}

void test_legal_edge_values_applied(void) {
    char ack[256];
    /* quiet_start 0 (midnight) and break_interval_min 0 (breaks disabled)
       are both in range and must apply, not error */
    apply("{\"ver\":\"1\",\"quiet_start\":0,\"break_interval_min\":0}", ack, sizeof(ack));
    uint16_t v;
    nvs_config_get_quiet_start(&v);
    TEST_ASSERT_EQUAL_UINT16(0, v);
    nvs_config_get_break_interval_min(&v);
    TEST_ASSERT_EQUAL_UINT16(0, v);
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"ok\":true"));
}

void test_short_timers_array_disables_trailing_slots(void) {
    char ack[256];
    apply("{\"ver\":\"1\",\"timers\":[{\"name\":\"Piano\",\"min\":15}]}", ack, sizeof(ack));
    nvs_timer_defs_blob_t defs;
    nvs_config_get_timer_defs(&defs);
    TEST_ASSERT_EQUAL_STRING("Piano", defs.defs[0].name);
    TEST_ASSERT_EQUAL_STRING("", defs.defs[1].name); /* absent entries = disabled */
    TEST_ASSERT_EQUAL_STRING("", defs.defs[2].name);
    TEST_ASSERT_EQUAL_STRING("", defs.defs[3].name);
}

void test_timers_wrong_type_rejected(void) {
    char ack[256];
    apply("{\"ver\":\"1\",\"timers\":5}", ack, sizeof(ack));
    TEST_ASSERT_NOT_NULL(strstr(ack, "timers"));
    nvs_timer_defs_blob_t defs;
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, nvs_config_get_timer_defs(&defs));
}

void test_holidays_non_string_element_rejected_keeps_good(void) {
    char ack[256];
    apply("{\"ver\":\"1\",\"holidays\":[\"2026-10-16\",42,\"2026-12-25\"]}", ack, sizeof(ack));
    char blob[512];
    size_t len = sizeof(blob);
    nvs_config_get_holidays(blob, &len);
    blob[len] = '\0';
    TEST_ASSERT_EQUAL_STRING("2026-10-16\n2026-12-25\n", blob);
    TEST_ASSERT_NOT_NULL(strstr(ack, "holidays"));
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
    RUN_TEST(test_is_iso_date_accepts_valid);
    RUN_TEST(test_is_iso_date_rejects_bad_shape);
    RUN_TEST(test_is_iso_date_rejects_impossible_calendar_dates);
    RUN_TEST(test_full_document_applies_and_stores_ver);
    RUN_TEST(test_same_ver_skips_without_writing);
    RUN_TEST(test_new_ver_reapplies);
    RUN_TEST(test_partial_document_touches_only_present_fields);
    RUN_TEST(test_invalid_field_reported_but_others_apply);
    RUN_TEST(test_bad_date_shape_rejected);
    RUN_TEST(test_valid_dates_applied);
    RUN_TEST(test_impossible_calendar_date_rejected);
    RUN_TEST(test_numeric_ver_accepted);
    RUN_TEST(test_legal_edge_values_applied);
    RUN_TEST(test_short_timers_array_disables_trailing_slots);
    RUN_TEST(test_timers_wrong_type_rejected);
    RUN_TEST(test_holidays_non_string_element_rejected_keeps_good);
    RUN_TEST(test_missing_ver_is_invalid);
    RUN_TEST(test_malformed_json_is_invalid_no_crash);
    RUN_TEST(test_holidays_array_becomes_newline_blob);
    RUN_TEST(test_holidays_reject_bad_entries_keep_good);
    RUN_TEST(test_holidays_cap_enforced);
    RUN_TEST(test_timers_array_maps_to_blob);
    RUN_TEST(test_timers_overlong_name_rejected);
    return UNITY_END();
}
