#include <string.h>
#include <unity.h>

/* Single-TU: cJSON + the config applier over the mock NVS + real
   nvs_config accessors (so validation and persistence are exercised
   end-to-end without hardware). */
// clang-format off
#include "cJSON.h"
#include "mock_hal_nvs.c"
#include "../../main/nvs_config.c"
#include "../../main/quiet_hours.c"
#include "../../main/bedtime.c"
#include "../../main/config_validate.c"
#include "../../main/tones.c"
#include "../../main/config_apply.c"
// clang-format on

void setUp(void) {
    mock_nvs_reset();
}
void tearDown(void) {}

static config_result_t apply(const char *json, char *ack, size_t acklen) {
    return config_apply(json, ack, acklen);
}

/* is_iso_date moved to config_validate.c (test_config_validate); the
   date-rejection behaviour is still exercised end-to-end below via the
   school_start / holidays fields. */

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

void test_quiet_hhmm_out_of_range_minute_rejected(void) {
    char ack[256];
    /* 2260 passes a naive 0..2359 range but minute 60 is not a real time */
    apply("{\"ver\":\"1\",\"quiet_start\":2260,\"quiet_end\":830}", ack, sizeof(ack));
    uint16_t v;
    nvs_config_get_quiet_start(&v);
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_QUIET_START, v); /* rejected */
    nvs_config_get_quiet_end(&v);
    TEST_ASSERT_EQUAL_UINT16(830, v); /* valid sibling still applied */
    TEST_ASSERT_NOT_NULL(strstr(ack, "quiet_start"));
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

void test_bedtime_applies_evening_and_zero_rejects_daytime(void) {
    char ack[256];
    uint16_t v;
    apply("{\"ver\":\"1\",\"bedtime\":2130}", ack, sizeof(ack));
    nvs_config_get_bedtime(&v);
    TEST_ASSERT_EQUAL_UINT16(2130, v);
    apply("{\"ver\":\"2\",\"bedtime\":0}", ack, sizeof(ack));
    nvs_config_get_bedtime(&v);
    TEST_ASSERT_EQUAL_UINT16(0, v); /* disabled */
    apply("{\"ver\":\"3\",\"bedtime\":900}", ack, sizeof(ack));
    nvs_config_get_bedtime(&v);
    TEST_ASSERT_EQUAL_UINT16(0, v); /* daytime rejected, value kept */
    TEST_ASSERT_NOT_NULL(strstr(ack, "bedtime"));
}

void test_tone_enum_applied_by_option_string(void) {
    char ack[256];
    apply("{\"ver\":\"1\",\"tone_expiry\":\"Gran Vals\",\"tone_bed\":\"Classic beep\"}", ack, sizeof(ack));
    uint16_t v;
    nvs_config_get_tone_expiry(&v);
    TEST_ASSERT_EQUAL_UINT16(TONE_GRANVALS, v);
    nvs_config_get_tone_bed(&v);
    TEST_ASSERT_EQUAL_UINT16(TONE_CLASSIC, v);
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"ok\":true"));
}

void test_tone_enum_unknown_or_nonstring_rejected(void) {
    char ack[256];
    apply("{\"ver\":\"1\",\"tone_break\":\"Kazoo\",\"tone_bed\":3}", ack, sizeof(ack));
    uint16_t v;
    nvs_config_get_tone_break(&v);
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_TONE_BREAK, v); /* unchanged */
    nvs_config_get_tone_bed(&v);
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_TONE_BED, v);
    TEST_ASSERT_NOT_NULL(strstr(ack, "tone_break"));
    TEST_ASSERT_NOT_NULL(strstr(ack, "tone_bed"));
}

void test_alert_volume_applied_and_range_enforced(void) {
    char ack[256];
    apply("{\"ver\":\"1\",\"alert_volume\":150}", ack, sizeof(ack));
    uint16_t v;
    nvs_config_get_alert_volume(&v);
    TEST_ASSERT_EQUAL_UINT16(150, v);
    apply("{\"ver\":\"2\",\"alert_volume\":999}", ack, sizeof(ack));
    nvs_config_get_alert_volume(&v);
    TEST_ASSERT_EQUAL_UINT16(150, v); /* out of range: value kept */
    TEST_ASSERT_NOT_NULL(strstr(ack, "alert_volume"));
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
        "{\"ver\":\"1\",\"timers\":[{\"name\":\"Piano\",\"min\":20,\"reload\":true,\"break\":true},{},"
        "{\"name\":\"Meditation\",\"min\":10,\"reload\":true},{}]}",
        ack, sizeof(ack));
    nvs_timer_defs_blob_t defs;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&defs));
    TEST_ASSERT_EQUAL_STRING("Piano", defs.defs[0].name);
    TEST_ASSERT_EQUAL_INT32(20, defs.defs[0].min);
    TEST_ASSERT_EQUAL_UINT8(1, defs.defs[0].reload);
    TEST_ASSERT_EQUAL_STRING("", defs.defs[1].name); /* {} = disabled */
    TEST_ASSERT_EQUAL_STRING("Meditation", defs.defs[2].name);
    TEST_ASSERT_EQUAL_UINT8(1, defs.defs[2].reload);         /* reload: true */
    TEST_ASSERT_EQUAL_UINT8(1, defs.defs[0].break_eligible); /* break: true */
    TEST_ASSERT_EQUAL_UINT8(0, defs.defs[2].break_eligible); /* absent on a NEW slot = false */
    TEST_ASSERT_EQUAL_STRING("", defs.defs[3].name);         /* {} = disabled */
}

/* ---- optional keys: absent means UNCHANGED for an existing slot --------

   BUG-6: `break` is settable from HA's per-timer switch but is absent from
   the documented `timers` schema, so a documentation-shaped document used
   to clear it on every application — and the retained set/ command that
   would have restored it is consumed on apply, so nothing healed it. The
   rule is now: absent leaves an existing slot alone, and is false only for
   a slot the document is defining for the first time. */

/* Seed the stored table the way a device that has been running would have
   it: HA's switch wrote break_eligible for a slot the document does not
   describe that field for. */
static void seed_defs(const char *n1, uint8_t reload1, uint8_t break1) {
    nvs_timer_defs_blob_t b;
    memset(&b, 0, sizeof(b));
    b.version = TIMER_DEFS_BLOB_VERSION;
    snprintf(b.defs[0].name, sizeof(b.defs[0].name), "%s", n1);
    b.defs[0].min = 45;
    b.defs[0].reload = reload1;
    b.defs[0].break_eligible = break1;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_timer_defs(&b));
}

/* The regression itself. */
void test_an_absent_break_leaves_an_existing_slot_alone(void) {
    seed_defs("Laundry", 0, 1);
    char ack[256];
    /* Exactly the documented shape: name, min, reload — no `break`. */
    apply("{\"ver\":\"1\",\"timers\":[{\"name\":\"Laundry\",\"min\":45,\"reload\":false}]}", ack, sizeof(ack));
    nvs_timer_defs_blob_t defs;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&defs));
    TEST_ASSERT_EQUAL_UINT8(1, defs.defs[0].break_eligible);
}

/* The other half of the rule — without this the fix would just be
   "ignore break", which loses the safe default for a brand-new timer. */
void test_an_absent_break_is_false_for_a_newly_defined_slot(void) {
    char ack[256]; /* nothing seeded: slot 0 is new */
    apply("{\"ver\":\"1\",\"timers\":[{\"name\":\"Minecraft\",\"min\":30}]}", ack, sizeof(ack));
    nvs_timer_defs_blob_t defs;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&defs));
    TEST_ASSERT_EQUAL_UINT8(0, defs.defs[0].break_eligible);
}

/* Absent must mean "unchanged", NOT "the key is ignored": an explicit
   false has to still clear a set flag, or the document loses the ability
   to turn the thing off. */
void test_an_explicit_break_false_still_clears_an_existing_slot(void) {
    seed_defs("Laundry", 0, 1);
    char ack[256];
    apply("{\"ver\":\"1\",\"timers\":[{\"name\":\"Laundry\",\"min\":45,\"break\":false}]}", ack, sizeof(ack));
    nvs_timer_defs_blob_t defs;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&defs));
    TEST_ASSERT_EQUAL_UINT8(0, defs.defs[0].break_eligible);
}

/* `reload` has the identical shape and the identical defect; the rule is
   applied to both rather than special-cased to the field that was
   reported. It bites less often only because `reload` IS documented. */
void test_an_absent_reload_leaves_an_existing_slot_alone(void) {
    seed_defs("Laundry", 1, 0);
    char ack[256];
    apply("{\"ver\":\"1\",\"timers\":[{\"name\":\"Laundry\",\"min\":45}]}", ack, sizeof(ack));
    nvs_timer_defs_blob_t defs;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&defs));
    TEST_ASSERT_EQUAL_UINT8(1, defs.defs[0].reload);
}

/* "Existing" means the slot HAS a definition, not merely that the blob
   exists. A disabled slot must not donate stale flags to the timer that
   replaces it — that is the case where the safe default still has to win. */
void test_a_disabled_slot_does_not_donate_its_flags(void) {
    nvs_timer_defs_blob_t b;
    memset(&b, 0, sizeof(b));
    b.version = TIMER_DEFS_BLOB_VERSION;
    b.defs[0].name[0] = '\0'; /* disabled... */
    b.defs[0].break_eligible = 1;
    b.defs[0].reload = 1; /* ...but carrying stale flags */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_timer_defs(&b));

    char ack[256];
    apply("{\"ver\":\"1\",\"timers\":[{\"name\":\"Minecraft\",\"min\":30}]}", ack, sizeof(ack));
    nvs_timer_defs_blob_t defs;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&defs));
    TEST_ASSERT_EQUAL_UINT8(0, defs.defs[0].break_eligible);
    TEST_ASSERT_EQUAL_UINT8(0, defs.defs[0].reload);
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

/* One bad entry rejects the whole array, so the ack must say WHICH entry
   or the edit looks like it silently did nothing to every timer. */
void test_timers_error_names_the_offending_entry(void) {
    char ack[256];
    apply(
        "{\"ver\":\"1\",\"timers\":[{\"name\":\"Piano\",\"min\":20},"
        "{\"name\":\"NonEligible Testing\",\"min\":2},{\"name\":\"Violin\",\"min\":30}]}",
        ack, sizeof(ack));
    TEST_ASSERT_NOT_NULL(strstr(ack, "timers[1]"));
    nvs_timer_defs_blob_t defs;
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, nvs_config_get_timer_defs(&defs));
}

/* Same for an out-of-range duration — the slot index is the useful part. */
void test_timers_bad_min_names_the_offending_entry(void) {
    char ack[256];
    apply("{\"ver\":\"1\",\"timers\":[{\"name\":\"Piano\",\"min\":20},{\"name\":\"Violin\",\"min\":9999}]}", ack,
          sizeof(ack));
    TEST_ASSERT_NOT_NULL(strstr(ack, "timers[1]"));
}

/* ---- OTA fields (the "three places" rule) ----------------------------
   Both HA-settable OTA fields must be parsed from the bulk retained
   document as well as the per-entity set path. `break_eligible` shipped
   with an entity and no bulk-document key, and every application of the
   retained document silently cleared it (BUG-6). These pin the same
   shape for ota_url / ota_on_sync so it cannot happen again. */

void test_ota_fields_apply_from_bulk_document(void) {
    char ack[256];
    const char *doc = "{\"ver\":\"1\",\"ota_url\":\"https://example.com/ota.json\",\"ota_on_sync\":true}";
    TEST_ASSERT_EQUAL(CONFIG_APPLIED, apply(doc, ack, sizeof(ack)));
    char s[CFG_BOUND_OTA_URL_MAX];
    nvs_config_get_ota_url(s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("https://example.com/ota.json", s);
    uint16_t v;
    nvs_config_get_ota_on_sync(&v);
    TEST_ASSERT_EQUAL_UINT16(1, v);
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"ok\":true"));
}

void test_ota_on_sync_false_applies(void) {
    char ack[256];
    nvs_config_set_ota_on_sync(1);
    TEST_ASSERT_EQUAL(CONFIG_APPLIED, apply("{\"ver\":\"1\",\"ota_on_sync\":false}", ack, sizeof(ack)));
    uint16_t v;
    nvs_config_get_ota_on_sync(&v);
    TEST_ASSERT_EQUAL_UINT16(0, v);
}

/* The BUG-6 shape: a document that says nothing about the OTA fields must
   leave an HA-set value alone, not clear it. */
void test_document_omitting_ota_fields_leaves_them_alone(void) {
    char ack[256];
    nvs_config_set_ota_url("https://ha.example/ota.json");
    nvs_config_set_ota_on_sync(1);
    TEST_ASSERT_EQUAL(CONFIG_APPLIED, apply("{\"ver\":\"1\",\"weekday_min\":45}", ack, sizeof(ack)));
    char s[CFG_BOUND_OTA_URL_MAX];
    nvs_config_get_ota_url(s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("https://ha.example/ota.json", s);
    uint16_t v;
    nvs_config_get_ota_on_sync(&v);
    TEST_ASSERT_EQUAL_UINT16(1, v);
}

void test_bulk_ota_url_rejects_non_https(void) {
    char ack[256];
    nvs_config_set_ota_url("https://good.example/ota.json");
    TEST_ASSERT_EQUAL(CONFIG_APPLIED,
                      apply("{\"ver\":\"1\",\"ota_url\":\"http://evil.example/ota.json\"}", ack, sizeof(ack)));
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"ota_url\""));
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"ok\":false"));
    char s[CFG_BOUND_OTA_URL_MAX];
    nvs_config_get_ota_url(s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("https://good.example/ota.json", s); /* unchanged */
}

void test_bulk_ota_url_empty_disables_and_is_valid(void) {
    char ack[256];
    nvs_config_set_ota_url("https://good.example/ota.json");
    TEST_ASSERT_EQUAL(CONFIG_APPLIED, apply("{\"ver\":\"1\",\"ota_url\":\"\"}", ack, sizeof(ack)));
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"ok\":true"));
    char s[CFG_BOUND_OTA_URL_MAX];
    nvs_config_get_ota_url(s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("", s);
}

void test_bulk_ota_url_overlong_rejected(void) {
    char ack[256];
    char doc[CFG_BOUND_OTA_URL_MAX + 64];
    char url[CFG_BOUND_OTA_URL_MAX + 8];
    memset(url, 'u', sizeof(url) - 1);
    memcpy(url, "https://", 8);
    url[sizeof(url) - 1] = '\0';
    snprintf(doc, sizeof(doc), "{\"ver\":\"1\",\"ota_url\":\"%s\"}", url);
    apply(doc, ack, sizeof(ack));
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"ota_url\""));
    char s[CFG_BOUND_OTA_URL_MAX];
    nvs_config_get_ota_url(s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING(NVS_DEFAULT_OTA_URL, s); /* never written */
}

void test_bulk_ota_fields_reject_wrong_types(void) {
    char ack[256];
    apply("{\"ver\":\"1\",\"ota_on_sync\":1,\"ota_url\":42}", ack, sizeof(ack));
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"ota_on_sync\""));
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"ota_url\""));
    uint16_t v;
    nvs_config_get_ota_on_sync(&v);
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_OTA_ON_SYNC, v);
}

/* ---- ack integrity under a fully-invalid document ----
   err_add used to let snprintf truncate mid-field-name, leaving an
   unterminated JSON string. HA cannot parse that, so EVERY error in the
   ack is lost — not just the one that overflowed. The OTA fields sort
   last and so were the first to be dropped. */

/* Every field present and wrong-typed: the worst case for the error list. */
static const char *const ALL_WRONG =
    "{\"ver\":\"1\",\"name\":1,\"tz\":2,\"weekday_min\":\"x\",\"weekend_min\":\"x\","
    "\"holiday_min\":\"x\",\"summer_min\":\"x\",\"quiet_start\":\"x\",\"quiet_end\":\"x\","
    "\"bedtime\":\"x\",\"break_interval_min\":\"x\",\"break_duration_min\":\"x\","
    "\"tone_expiry\":1,\"tone_break\":1,\"tone_bed\":1,\"alert_volume\":\"x\","
    "\"summer_start\":1,\"school_start\":1,\"school_end\":1,"
    "\"ota_url\":42,\"ota_on_sync\":1,\"holidays\":5,\"timers\":5}";

void test_worst_case_ack_is_parseable_json(void) {
    char ack[CONFIG_ACK_MIN];
    apply(ALL_WRONG, ack, sizeof(ack));
    cJSON *root = cJSON_Parse(ack);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, ack); /* the whole point */
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(root, "ok")));
    TEST_ASSERT_TRUE(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(root, "errors")));
    cJSON_Delete(root);
}

void test_worst_case_ack_flags_truncation(void) {
    char ack[CONFIG_ACK_MIN];
    apply(ALL_WRONG, ack, sizeof(ack));
    cJSON *root = cJSON_Parse(ack);
    TEST_ASSERT_NOT_NULL(root);
    /* Without the flag a dropped entry is indistinguishable from a field
       that applied cleanly. */
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "errors_truncated")));
    cJSON_Delete(root);
}

void test_worst_case_ack_fits_the_declared_minimum(void) {
    char ack[CONFIG_ACK_MIN];
    memset(ack, 0x7F, sizeof(ack));
    apply(ALL_WRONG, ack, sizeof(ack));
    /* Fits WHOLE, with the NUL inside the buffer — not merely "snprintf
       didn't crash". */
    TEST_ASSERT_TRUE(strlen(ack) < sizeof(ack));
}

/* Every named error must be a complete field name, never a fragment. */
void test_truncated_error_list_contains_no_partial_names(void) {
    char ack[CONFIG_ACK_MIN];
    apply(ALL_WRONG, ack, sizeof(ack));
    cJSON *root = cJSON_Parse(ack);
    TEST_ASSERT_NOT_NULL(root);
    const cJSON *errors = cJSON_GetObjectItemCaseSensitive(root, "errors");
    const cJSON *item;
    cJSON_ArrayForEach(item, errors) {
        TEST_ASSERT_TRUE(cJSON_IsString(item));
        /* Every real field name appears verbatim in the document. */
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(ALL_WRONG, item->valuestring), item->valuestring);
    }
    cJSON_Delete(root);
}

/* A handful of errors must NOT claim truncation. */
void test_small_error_list_is_not_flagged_truncated(void) {
    char ack[CONFIG_ACK_MIN];
    apply("{\"ver\":\"1\",\"weekday_min\":\"x\",\"ota_url\":\"http://nope\"}", ack, sizeof(ack));
    cJSON *root = cJSON_Parse(ack);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(root, "errors_truncated"));
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"ota_url\""));
    cJSON_Delete(root);
}

/* ---- ver is interpolated into all three ack emissions ----
   A quote in ver produced {"ver":"a"b","ok":true}, which does not parse —
   defeating the errors[] work, and reachable with one mistyped ver. */

void test_ver_with_a_quote_is_rejected_and_ack_parses(void) {
    char ack[CONFIG_ACK_MIN];
    TEST_ASSERT_EQUAL(CONFIG_INVALID, apply("{\"ver\":\"a\\\"b\",\"weekday_min\":45}", ack, sizeof(ack)));
    cJSON *root = cJSON_Parse(ack);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, ack);
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(root, "ok")));
    TEST_ASSERT_EQUAL_STRING("ver", cJSON_GetObjectItemCaseSensitive(root, "err")->valuestring);
    cJSON_Delete(root);
    /* nothing applied, and cfg_ver untouched */
    uint16_t v;
    nvs_config_get_weekday_min(&v);
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_WEEKDAY_MIN, v);
}

void test_ver_with_a_backslash_is_rejected(void) {
    char ack[CONFIG_ACK_MIN];
    TEST_ASSERT_EQUAL(CONFIG_INVALID, apply("{\"ver\":\"a\\\\b\"}", ack, sizeof(ack)));
    cJSON *root = cJSON_Parse(ack);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, ack);
    cJSON_Delete(root);
}

void test_ver_with_a_control_character_is_rejected(void) {
    char ack[CONFIG_ACK_MIN];
    TEST_ASSERT_EQUAL(CONFIG_INVALID, apply("{\"ver\":\"a\\nb\"}", ack, sizeof(ack)));
    cJSON *root = cJSON_Parse(ack);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, ack);
    cJSON_Delete(root);
}

/* The SKIPPED emission interpolates ver too — it is only reachable with a
   clean ver now, but pin that its ack parses. */
void test_skipped_ack_parses(void) {
    char ack[CONFIG_ACK_MIN];
    apply("{\"ver\":\"20260811\",\"weekday_min\":45}", ack, sizeof(ack));
    TEST_ASSERT_EQUAL(CONFIG_SKIPPED, apply("{\"ver\":\"20260811\"}", ack, sizeof(ack)));
    cJSON *root = cJSON_Parse(ack);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, ack);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "skipped")));
    cJSON_Delete(root);
}

/* A clean ver at full width still round-trips (the check rejects
   characters, not length). */
void test_clean_ver_still_applies(void) {
    char ack[CONFIG_ACK_MIN];
    TEST_ASSERT_EQUAL(CONFIG_APPLIED, apply("{\"ver\":\"2026-08-11T00:00:01Z\"}", ack, sizeof(ack)));
    char stored[24];
    nvs_config_get_cfg_ver(stored, sizeof(stored));
    TEST_ASSERT_EQUAL_STRING("2026-08-11T00:00:01Z", stored);
}

/* ---- a failed NVS write must reach the ack ----
   Rejecting instead of truncating only pays off if someone hears it. */

void test_nvs_write_failure_is_reported_not_swallowed(void) {
    char ack[CONFIG_ACK_MIN];
    mock_nvs_fail_writes(1); /* first write of the document fails */
    apply("{\"ver\":\"1\",\"weekday_min\":45}", ack, sizeof(ack));
    cJSON *root = cJSON_Parse(ack);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, ack);
    TEST_ASSERT_TRUE_MESSAGE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(root, "ok")), ack);
    TEST_ASSERT_NOT_NULL(strstr(ack, "weekday_min"));
    cJSON_Delete(root);
}

void test_ota_url_nvs_write_failure_is_reported(void) {
    char ack[CONFIG_ACK_MIN];
    mock_nvs_fail_writes(1);
    apply("{\"ver\":\"1\",\"ota_url\":\"https://example.com/ota.json\"}", ack, sizeof(ack));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(ack, "ota_url"), ack);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_full_document_applies_and_stores_ver);
    RUN_TEST(test_same_ver_skips_without_writing);
    RUN_TEST(test_new_ver_reapplies);
    RUN_TEST(test_partial_document_touches_only_present_fields);
    RUN_TEST(test_invalid_field_reported_but_others_apply);
    RUN_TEST(test_bad_date_shape_rejected);
    RUN_TEST(test_valid_dates_applied);
    RUN_TEST(test_impossible_calendar_date_rejected);
    RUN_TEST(test_numeric_ver_accepted);
    RUN_TEST(test_quiet_hhmm_out_of_range_minute_rejected);
    RUN_TEST(test_legal_edge_values_applied);
    RUN_TEST(test_bedtime_applies_evening_and_zero_rejects_daytime);
    RUN_TEST(test_tone_enum_applied_by_option_string);
    RUN_TEST(test_tone_enum_unknown_or_nonstring_rejected);
    RUN_TEST(test_alert_volume_applied_and_range_enforced);
    RUN_TEST(test_short_timers_array_disables_trailing_slots);
    RUN_TEST(test_timers_wrong_type_rejected);
    RUN_TEST(test_holidays_non_string_element_rejected_keeps_good);
    RUN_TEST(test_missing_ver_is_invalid);
    RUN_TEST(test_malformed_json_is_invalid_no_crash);
    RUN_TEST(test_holidays_array_becomes_newline_blob);
    RUN_TEST(test_holidays_reject_bad_entries_keep_good);
    RUN_TEST(test_holidays_cap_enforced);
    RUN_TEST(test_timers_array_maps_to_blob);
    RUN_TEST(test_an_absent_break_leaves_an_existing_slot_alone);
    RUN_TEST(test_an_absent_break_is_false_for_a_newly_defined_slot);
    RUN_TEST(test_an_explicit_break_false_still_clears_an_existing_slot);
    RUN_TEST(test_an_absent_reload_leaves_an_existing_slot_alone);
    RUN_TEST(test_a_disabled_slot_does_not_donate_its_flags);
    RUN_TEST(test_timers_overlong_name_rejected);
    RUN_TEST(test_timers_error_names_the_offending_entry);
    RUN_TEST(test_timers_bad_min_names_the_offending_entry);
    RUN_TEST(test_ota_fields_apply_from_bulk_document);
    RUN_TEST(test_ota_on_sync_false_applies);
    RUN_TEST(test_document_omitting_ota_fields_leaves_them_alone);
    RUN_TEST(test_bulk_ota_url_rejects_non_https);
    RUN_TEST(test_bulk_ota_url_empty_disables_and_is_valid);
    RUN_TEST(test_bulk_ota_url_overlong_rejected);
    RUN_TEST(test_bulk_ota_fields_reject_wrong_types);
    RUN_TEST(test_worst_case_ack_is_parseable_json);
    RUN_TEST(test_worst_case_ack_flags_truncation);
    RUN_TEST(test_worst_case_ack_fits_the_declared_minimum);
    RUN_TEST(test_truncated_error_list_contains_no_partial_names);
    RUN_TEST(test_small_error_list_is_not_flagged_truncated);
    RUN_TEST(test_ver_with_a_quote_is_rejected_and_ack_parses);
    RUN_TEST(test_ver_with_a_backslash_is_rejected);
    RUN_TEST(test_ver_with_a_control_character_is_rejected);
    RUN_TEST(test_skipped_ack_parses);
    RUN_TEST(test_clean_ver_still_applies);
    RUN_TEST(test_nvs_write_failure_is_reported_not_swallowed);
    RUN_TEST(test_ota_url_nvs_write_failure_is_reported);
    return UNITY_END();
}
