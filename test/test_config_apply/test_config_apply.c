#include <string.h>
#include <unity.h>

/* Single-TU: cJSON + the config applier over the mock NVS + real
   nvs_config accessors (so validation and persistence are exercised
   end-to-end without hardware).

   timer_defs.c (and timer.c under it) are here because apply_timers()
   resolves the bottom rung of its optional-key ladder from the compile-time
   table, via timer_defs_compiled(). This TU defines no CONFIG_MAGTAG_TIMER*
   symbols, so that table is empty and the rung yields 0 — which is exactly
   the pre-existing behaviour every case below was written against. The
   suite that pins the rung itself needs a non-empty compile-time table and
   lives in test_timer_defs. */
// clang-format off
#include "cJSON.h"
#include "mock_hal_nvs.c"
#include "mock_hal_time.c"
#include "../../main/timer.c"
#include "../../main/nvs_config.c"
#include "../../main/chores.c"
#include "../../main/chore_store.c"
#include "../../main/quiet_hours.c"
#include "../../main/bedtime.c"
#include "../../main/config_validate.c"
#include "../../main/tones.c"
#include "../../main/config_apply.c"
#include "../../main/timer_defs.c"
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
    /* Absent on a NEW slot falls to the compile-time value for that slot,
       which is 0 here: this TU defines no CONFIG_MAGTAG_TIMER* symbols. */
    TEST_ASSERT_EQUAL_UINT8(0, defs.defs[2].break_eligible);
    TEST_ASSERT_EQUAL_STRING("", defs.defs[3].name); /* {} = disabled */
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

/* Every field present and wrong-typed: the worst case for the error list.
   THE CHORE FIELDS BELONG IN HERE. ERR_LIST_CAP (160 B) was sized against
   the 22 fields this document used to carry; the chore checklist adds five
   more nameable ones and the list is the resource they compete for, so a
   document that omits them stopped being the worst case the moment they
   landed. With 27 wrong fields the ack still surfaces 12 entries — the
   four 13-14 byte chore_free_* names sort early (right after the
   allocations) and consume ~68 of the 160 bytes, so 15 fields are now
   dropped rather than 10.

   ONE CONSEQUENCE WORTH KNOWING BEFORE IT SURPRISES SOMEONE: "chores"
   itself is applied near the end of the pass and is therefore among the
   displaced — the flagship field of the feature is INVISIBLE in a
   maximally-wrong ack. That is not a correctness break (the four tests
   below pin what actually matters: the ack is valid JSON, flagged
   errors_truncated, terminated cleanly with no partial name, and inside
   CONFIG_ACK_MIN), and per-field tests cover "chores" on its own. It is
   recorded here because the alternative — discovering it from a support
   thread — is worse. Widening ERR_LIST_CAP is a size/ack-budget decision
   above this task. */
static const char *const ALL_WRONG =
    "{\"ver\":\"1\",\"name\":1,\"tz\":2,\"weekday_min\":\"x\",\"weekend_min\":\"x\","
    "\"holiday_min\":\"x\",\"summer_min\":\"x\","
    "\"chore_free_wd\":\"x\",\"chore_free_we\":\"x\",\"chore_free_hol\":\"x\","
    "\"chore_free_sum\":\"x\",\"quiet_start\":\"x\",\"quiet_end\":\"x\","
    "\"bedtime\":\"x\",\"break_interval_min\":\"x\",\"break_duration_min\":\"x\","
    "\"tone_expiry\":1,\"tone_break\":1,\"tone_bed\":1,\"alert_volume\":\"x\","
    "\"summer_start\":1,\"school_start\":1,\"school_end\":1,"
    "\"ota_url\":42,\"ota_on_sync\":1,\"holidays\":5,\"chores\":5,\"timers\":5}";

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
    /* The headroom, stated rather than implied: 216 of CONFIG_ACK_MIN's 256
       bytes with the chore fields in the document (12 entries surfaced of
       27 wrong fields). Asserted as a bound, not an equality — the exact
       figure moves with any field rename — but a change that eats the
       remaining 40 bytes should have to come here and say so, because the
       byte after the cap is where err_add's clean-termination guarantee is
       the only thing standing between HA and an unparseable ack. */
    TEST_ASSERT_TRUE_MESSAGE(strlen(ack) <= 232, ack);
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

/* ---- the chore checklist: the list, the free slice, the cross-field rule ----
   Design 1.2 (cap 3, name cap 20 BYTES, enforced with a named error in the
   config_ack and NEVER by truncation) and rows C1/C10/C11/C12. */

static uint8_t stored_chores(char names[CHORE_MAX][CHORE_NAME_BUF]) {
    uint8_t n = 0xFF;
    chore_store_load_names(names, &n);
    return n;
}

/* The errors list holds WHOLE quoted names, so match the quotes: a bare
   substring test would let "chore_free_wd" answer for a longer key and
   would also hit the ack's own object keys. */
static bool ack_names(const char *ack, const char *field) {
    char quoted[40];
    snprintf(quoted, sizeof(quoted), "\"%s\"", field);
    return strstr(ack, quoted) != NULL;
}

void test_chores_three_applied_and_readable_back(void) {
    char ack[CONFIG_ACK_MIN];
    TEST_ASSERT_EQUAL(CONFIG_APPLIED, apply("{\"ver\":\"1\",\"chores\":[\"Dishes away\",\"Trash out\",\"Homework\"]}",
                                            ack, sizeof(ack)));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(ack, "\"ok\":true"), ack);
    char names[CHORE_MAX][CHORE_NAME_BUF];
    TEST_ASSERT_EQUAL_UINT8(3, stored_chores(names));
    TEST_ASSERT_EQUAL_STRING("Dishes away", names[0]);
    TEST_ASSERT_EQUAL_STRING("Trash out", names[1]);
    TEST_ASSERT_EQUAL_STRING("Homework", names[2]);
}

/* The plan row's central requirement: a fourth chore is an ERROR, not a
   silently dropped entry, and nothing partial reaches NVS. */
void test_chores_a_fourth_entry_is_an_error_and_writes_nothing(void) {
    char ack[CONFIG_ACK_MIN];
    apply("{\"ver\":\"1\",\"chores\":[\"Dishes\",\"Trash\"]}", ack, sizeof(ack));
    apply("{\"ver\":\"2\",\"chores\":[\"Dishes\",\"Trash\",\"Homework\",\"Laundry\"]}", ack, sizeof(ack));
    TEST_ASSERT_TRUE_MESSAGE(ack_names(ack, "chores"), ack);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(ack, "\"ok\":false"), ack);
    char names[CHORE_MAX][CHORE_NAME_BUF];
    TEST_ASSERT_EQUAL_UINT8(2, stored_chores(names)); /* not 3, and not the first three */
    TEST_ASSERT_EQUAL_STRING("Dishes", names[0]);
    TEST_ASSERT_EQUAL_STRING("Trash", names[1]);
    TEST_ASSERT_EQUAL_STRING("", names[2]);
}

/* 21 bytes is one past CHORE_NAME_MAX: an error, never a 20-byte stump. */
void test_chores_overlong_name_is_an_error_not_a_truncation(void) {
    char ack[CONFIG_ACK_MIN];
    apply("{\"ver\":\"1\",\"chores\":[\"123456789012345678901\"]}", ack, sizeof(ack));
    TEST_ASSERT_TRUE_MESSAGE(ack_names(ack, "chores"), ack);
    char names[CHORE_MAX][CHORE_NAME_BUF];
    TEST_ASSERT_EQUAL_UINT8(0, stored_chores(names));
    TEST_ASSERT_EQUAL_STRING("", names[0]);
}

void test_chores_name_at_the_cap_is_accepted_whole(void) {
    char ack[CONFIG_ACK_MIN];
    apply("{\"ver\":\"1\",\"chores\":[\"12345678901234567890\"]}", ack, sizeof(ack)); /* exactly 20 */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(ack, "\"ok\":true"), ack);
    char names[CHORE_MAX][CHORE_NAME_BUF];
    TEST_ASSERT_EQUAL_UINT8(1, stored_chores(names));
    TEST_ASSERT_EQUAL_STRING("12345678901234567890", names[0]);
}

void test_chores_non_string_entry_is_an_error_and_writes_nothing(void) {
    char ack[CONFIG_ACK_MIN];
    apply("{\"ver\":\"1\",\"chores\":[\"Dishes\",7]}", ack, sizeof(ack));
    TEST_ASSERT_TRUE_MESSAGE(ack_names(ack, "chores"), ack);
    char names[CHORE_MAX][CHORE_NAME_BUF];
    TEST_ASSERT_EQUAL_UINT8(0, stored_chores(names));
}

void test_chores_empty_name_is_an_error(void) {
    char ack[CONFIG_ACK_MIN];
    apply("{\"ver\":\"1\",\"chores\":[\"\",\"Trash\"]}", ack, sizeof(ack));
    TEST_ASSERT_TRUE_MESSAGE(ack_names(ack, "chores"), ack);
    char names[CHORE_MAX][CHORE_NAME_BUF];
    TEST_ASSERT_EQUAL_UINT8(0, stored_chores(names));
}

/* A name reaches the hand-built JSON of the HA layer, so it is held to the
   same rule `ver` is: config_is_clean_str. */
void test_chores_name_with_a_quote_is_rejected(void) {
    char ack[CONFIG_ACK_MIN];
    apply("{\"ver\":\"1\",\"chores\":[\"Say \\\"hi\\\"\"]}", ack, sizeof(ack));
    TEST_ASSERT_TRUE_MESSAGE(ack_names(ack, "chores"), ack);
    char names[CHORE_MAX][CHORE_NAME_BUF];
    TEST_ASSERT_EQUAL_UINT8(0, stored_chores(names));
    cJSON *root = cJSON_Parse(ack); /* and the ack itself still parses */
    TEST_ASSERT_NOT_NULL_MESSAGE(root, ack);
    cJSON_Delete(root);
}

void test_chores_non_array_is_an_error(void) {
    char ack[CONFIG_ACK_MIN];
    apply("{\"ver\":\"1\",\"chores\":\"Dishes\"}", ack, sizeof(ack));
    TEST_ASSERT_TRUE_MESSAGE(ack_names(ack, "chores"), ack);
    char names[CHORE_MAX][CHORE_NAME_BUF];
    TEST_ASSERT_EQUAL_UINT8(0, stored_chores(names));
}

/* Absent is a no-op, like every other field: a document that says nothing
   about the list must not clear it. */
void test_chores_absent_leaves_an_existing_list_alone(void) {
    char ack[CONFIG_ACK_MIN];
    apply("{\"ver\":\"1\",\"chores\":[\"Dishes\",\"Trash\"]}", ack, sizeof(ack));
    apply("{\"ver\":\"2\",\"weekday_min\":45}", ack, sizeof(ack));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(ack, "\"ok\":true"), ack);
    char names[CHORE_MAX][CHORE_NAME_BUF];
    TEST_ASSERT_EQUAL_UINT8(2, stored_chores(names));
    TEST_ASSERT_EQUAL_STRING("Dishes", names[0]);
}

/* An EXPLICIT empty array is the documented way to turn the feature off
   (row C1), and it is not the same statement as saying nothing. */
void test_chores_explicit_empty_array_clears_the_list(void) {
    char ack[CONFIG_ACK_MIN];
    apply("{\"ver\":\"1\",\"chores\":[\"Dishes\",\"Trash\"]}", ack, sizeof(ack));
    apply("{\"ver\":\"2\",\"chores\":[]}", ack, sizeof(ack));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(ack, "\"ok\":true"), ack);
    char names[CHORE_MAX][CHORE_NAME_BUF];
    TEST_ASSERT_EQUAL_UINT8(0, stored_chores(names));
    TEST_ASSERT_EQUAL_STRING("", names[0]);
}

void test_chores_nvs_write_failure_is_reported_and_keeps_the_live_acks(void) {
    char ack[CONFIG_ACK_MIN];
    timer_chore_set_acked(0x1);
    timer_chore_set_released(false);
    mock_nvs_fail_writes(1);
    apply("{\"ver\":\"1\",\"chores\":[\"Dishes\"]}", ack, sizeof(ack));
    TEST_ASSERT_TRUE_MESSAGE(ack_names(ack, "chores"), ack);
    /* The list on flash did not move, so the positional acks still mean
       what they meant. */
    TEST_ASSERT_EQUAL_UINT8(0x1, timer_chore_acked());
}

/* Judgement call (b): the live RTC acks are POSITIONAL and this apply runs
   mid-wake, so a list edit has to reconcile them here — row C10's rule,
   acks cleared and `released` PRESERVED. chore_store_load_ack() applies the
   same rule on the next boot; nothing but this applies it to RTC. */
void test_a_chore_list_edit_clears_the_live_acks_and_keeps_the_release(void) {
    char ack[CONFIG_ACK_MIN];
    apply("{\"ver\":\"1\",\"chores\":[\"Dishes\",\"Trash\"]}", ack, sizeof(ack));
    timer_chore_set_acked(0x3);
    timer_chore_set_released(true);
    apply("{\"ver\":\"2\",\"chores\":[\"Dishes\",\"Homework\"]}", ack, sizeof(ack));
    TEST_ASSERT_EQUAL_UINT8(0, timer_chore_acked());
    TEST_ASSERT_TRUE(timer_chore_released());
}

void test_re_applying_the_same_chore_list_leaves_the_live_acks_alone(void) {
    char ack[CONFIG_ACK_MIN];
    apply("{\"ver\":\"1\",\"chores\":[\"Dishes\",\"Trash\"]}", ack, sizeof(ack));
    timer_chore_set_acked(0x2);
    timer_chore_set_released(false);
    apply("{\"ver\":\"2\",\"chores\":[\"Dishes\",\"Trash\"]}", ack, sizeof(ack));
    TEST_ASSERT_EQUAL_UINT8(0x2, timer_chore_acked());
    TEST_ASSERT_FALSE(timer_chore_released());
}

/* A number (7) was the only non-string entry covered. These are the other
   four JSON types a hand-written document or a HA template can produce,
   and each must be refused WHOLE rather than coerced or skipped. The
   nested array is the realistic one: a template with one extra level of
   brackets. The last case puts a good entry first, so "reject the array"
   is distinguished from "reject from the bad entry onward". */
void test_chores_structured_entries_are_errors_and_write_nothing(void) {
    static const char *const docs[] = {
        "{\"ver\":\"1\",\"chores\":[[\"Dishes\"]]}",
        "{\"ver\":\"1\",\"chores\":[{\"name\":\"Dishes\"}]}",
        "{\"ver\":\"1\",\"chores\":[null]}",
        "{\"ver\":\"1\",\"chores\":[true]}",
        "{\"ver\":\"1\",\"chores\":[\"Dishes\",null]}",
    };
    for (size_t i = 0; i < sizeof(docs) / sizeof(docs[0]); i++) {
        mock_nvs_reset();
        char ack[CONFIG_ACK_MIN];
        apply(docs[i], ack, sizeof(ack));
        TEST_ASSERT_TRUE_MESSAGE(ack_names(ack, "chores"), docs[i]);
        char names[CHORE_MAX][CHORE_NAME_BUF];
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, stored_chores(names), docs[i]);
    }
}

/* CHORE_MAX is 3 and names[] has exactly three rows, so the count test has
   to come FIRST in the per-entry contract — five entries is the case that
   writes names[3] and names[4] if it does not, and the suite runs under
   ASan, so that is a failure rather than silent corruption. */
void test_chores_five_entries_are_rejected_and_store_nothing(void) {
    char ack[CONFIG_ACK_MIN];
    apply("{\"ver\":\"1\",\"chores\":[\"A\",\"B\",\"C\",\"D\",\"E\"]}", ack, sizeof(ack));
    TEST_ASSERT_TRUE_MESSAGE(ack_names(ack, "chores"), ack);
    char names[CHORE_MAX][CHORE_NAME_BUF];
    TEST_ASSERT_EQUAL_UINT8(0, stored_chores(names));
}

/* The quote is tested above; these are the other two classes
   config_is_clean_str() refuses (backslash, and any byte below 0x20). */
void test_chores_name_with_a_backslash_or_control_byte_is_rejected(void) {
    char ack[CONFIG_ACK_MIN];
    char names[CHORE_MAX][CHORE_NAME_BUF];
    apply("{\"ver\":\"1\",\"chores\":[\"C:\\\\Dishes\"]}", ack, sizeof(ack));
    TEST_ASSERT_TRUE_MESSAGE(ack_names(ack, "chores"), ack);
    TEST_ASSERT_EQUAL_UINT8(0, stored_chores(names));
    mock_nvs_reset();
    apply("{\"ver\":\"1\",\"chores\":[\"Dish\\tes\"]}", ack, sizeof(ack));
    TEST_ASSERT_TRUE_MESSAGE(ack_names(ack, "chores"), ack);
    TEST_ASSERT_EQUAL_UINT8(0, stored_chores(names));
}

/* CHORE_NAME_MAX IS A BYTE COUNT, which apply_chores() asserts in a comment
   and nothing pinned. Seven U+6D17 are seven CHARACTERS and twenty-one
   BYTES, so the name is over the cap and refused whole. The failure this
   guards against is not the rejection but the alternative: keeping twenty
   bytes of it, which splits the seventh codepoint and stores a name no
   renderer can draw. Written as \u escapes so the assertion does not
   depend on this file's source encoding. */
void test_chores_a_21_byte_utf8_name_is_rejected_whole(void) {
    char ack[CONFIG_ACK_MIN];
    apply("{\"ver\":\"1\",\"chores\":[\"\\u6d17\\u6d17\\u6d17\\u6d17\\u6d17\\u6d17\\u6d17\"]}", ack, sizeof(ack));
    TEST_ASSERT_TRUE_MESSAGE(ack_names(ack, "chores"), ack);
    char names[CHORE_MAX][CHORE_NAME_BUF];
    TEST_ASSERT_EQUAL_UINT8(0, stored_chores(names));
    TEST_ASSERT_EQUAL_STRING("", names[0]); /* no 20-byte prefix left behind */
}

/* Per-field independence: a rejected `chores` must not take the rest of the
   document down with it, and must not damage the list already stored. This
   is the shape that makes the ack actionable — fix the one named field and
   re-send. */
void test_a_rejected_chores_array_leaves_the_other_fields_applied(void) {
    char ack[CONFIG_ACK_MIN];
    apply("{\"ver\":\"1\",\"chores\":[\"Dishes\",\"Trash\"]}", ack, sizeof(ack));
    apply("{\"ver\":\"2\",\"chores\":[\"A\",\"B\",\"C\",\"D\"],\"weekday_min\":120,\"chore_free_wd\":45}", ack,
          sizeof(ack));
    TEST_ASSERT_TRUE_MESSAGE(ack_names(ack, "chores"), ack);
    TEST_ASSERT_FALSE_MESSAGE(ack_names(ack, "chore_free_wd"), ack);
    uint16_t v = 0;
    nvs_config_get_chore_free_wd(&v);
    TEST_ASSERT_EQUAL_UINT16(45, v);
    nvs_config_get_weekday_min(&v);
    TEST_ASSERT_EQUAL_UINT16(120, v);
    char names[CHORE_MAX][CHORE_NAME_BUF];
    TEST_ASSERT_EQUAL_UINT8(2, stored_chores(names));
    TEST_ASSERT_EQUAL_STRING("Dishes", names[0]);
    TEST_ASSERT_EQUAL_STRING("Trash", names[1]);
}

/* The write-failure test above pins the live ACKS. THE OLD LIST SURVIVING
   is the other half, and it was unpinned: "nothing partial reaches NVS"
   has to mean the previous list is still readable, not merely that the new
   one is absent. That is also what makes the preserved acks correct — they
   are positional against this list. */
void test_a_failed_chore_save_leaves_the_previous_list_readable(void) {
    char ack[CONFIG_ACK_MIN];
    apply("{\"ver\":\"1\",\"chores\":[\"Dishes\",\"Trash\"]}", ack, sizeof(ack));
    mock_nvs_fail_writes(1); /* the chores blob is this document's first write */
    apply("{\"ver\":\"2\",\"chores\":[\"Homework\"]}", ack, sizeof(ack));
    TEST_ASSERT_TRUE_MESSAGE(ack_names(ack, "chores"), ack);
    char names[CHORE_MAX][CHORE_NAME_BUF];
    TEST_ASSERT_EQUAL_UINT8(2, stored_chores(names));
    TEST_ASSERT_EQUAL_STRING("Dishes", names[0]);
    TEST_ASSERT_EQUAL_STRING("Trash", names[1]);
}

/* DUPLICATES ARE ACCEPTED. This test exists so that is a decision on
   record rather than an accident: nothing anywhere rejects them — there is
   no duplicate-name guard in chores.c, chore_store.c or config_apply.c —
   and three identical names store as three rows with an ok:true ack.
   NOT a correctness break: the ack bits are POSITIONAL, so each row ticks
   independently, and chores_list_hash() is well defined over duplicates.
   IT IS AN UNRESOLVED USABILITY HOLE: three identical rows on the panel
   with no way for a kid to tell which one they just ticked. Rejecting is a
   policy choice design 1.2 does not make, and the chore screen belongs to
   M2 — so M2 owns the decision (dedupe on entry, dedupe in the UI, or
   leave it). If the answer becomes "reject", invert this test. */
void test_duplicate_chore_names_are_accepted_as_distinct_rows(void) {
    char ack[CONFIG_ACK_MIN];
    TEST_ASSERT_EQUAL(CONFIG_APPLIED,
                      apply("{\"ver\":\"1\",\"chores\":[\"Dishes\",\"Dishes\",\"Dishes\"]}", ack, sizeof(ack)));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(ack, "\"ok\":true"), ack);
    char names[CHORE_MAX][CHORE_NAME_BUF];
    TEST_ASSERT_EQUAL_UINT8(3, stored_chores(names));
    TEST_ASSERT_EQUAL_STRING("Dishes", names[0]);
    TEST_ASSERT_EQUAL_STRING("Dishes", names[1]);
    TEST_ASSERT_EQUAL_STRING("Dishes", names[2]);
}

/* ---- the four chore_free_* minute keys ---- */

void test_chore_free_applies_at_both_bounds(void) {
    char ack[CONFIG_ACK_MIN];
    apply(
        "{\"ver\":\"1\",\"weekday_min\":1440,\"weekend_min\":1440,\"holiday_min\":1440,\"summer_min\":1440,"
        "\"chore_free_wd\":0,\"chore_free_we\":1440,\"chore_free_hol\":15,\"chore_free_sum\":1440}",
        ack, sizeof(ack));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(ack, "\"ok\":true"), ack);
    uint16_t v = 0xFFFF;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_chore_free_wd(&v));
    TEST_ASSERT_EQUAL_UINT16(0, v);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_chore_free_we(&v));
    TEST_ASSERT_EQUAL_UINT16(1440, v);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_chore_free_hol(&v));
    TEST_ASSERT_EQUAL_UINT16(15, v);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_chore_free_sum(&v));
    TEST_ASSERT_EQUAL_UINT16(1440, v);
}

void test_chore_free_above_the_ceiling_is_named_and_not_stored(void) {
    char ack[CONFIG_ACK_MIN];
    apply(
        "{\"ver\":\"1\",\"chore_free_wd\":1441,\"chore_free_we\":1441,"
        "\"chore_free_hol\":1441,\"chore_free_sum\":1441}",
        ack, sizeof(ack));
    TEST_ASSERT_TRUE_MESSAGE(ack_names(ack, "chore_free_wd"), ack);
    TEST_ASSERT_TRUE_MESSAGE(ack_names(ack, "chore_free_we"), ack);
    TEST_ASSERT_TRUE_MESSAGE(ack_names(ack, "chore_free_hol"), ack);
    TEST_ASSERT_TRUE_MESSAGE(ack_names(ack, "chore_free_sum"), ack);
    uint16_t v = 0xFFFF;
    nvs_config_get_chore_free_wd(&v);
    TEST_ASSERT_EQUAL_UINT16(0, v);
    nvs_config_get_chore_free_sum(&v);
    TEST_ASSERT_EQUAL_UINT16(0, v);
}

void test_chore_free_wrong_type_is_named(void) {
    char ack[CONFIG_ACK_MIN];
    apply("{\"ver\":\"1\",\"chore_free_wd\":\"30\"}", ack, sizeof(ack));
    TEST_ASSERT_TRUE_MESSAGE(ack_names(ack, "chore_free_wd"), ack);
}

/* ---- the cross-field rule (C11 / C12) ---- */

/* C11's pair, weekday being the day type a school-term Monday resolves to.
   config_apply names it; whether the device BLOCKS on it is the M2 gate's
   question, which is why the broken pair is left standing in NVS for that
   gate to find. */
void test_chore_free_over_its_allocation_is_named(void) {
    char ack[CONFIG_ACK_MIN];
    apply("{\"ver\":\"1\",\"weekday_min\":60,\"chore_free_wd\":90}", ack, sizeof(ack));
    TEST_ASSERT_TRUE_MESSAGE(ack_names(ack, "chore_free_wd"), ack);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(ack, "\"ok\":false"), ack);
    uint16_t v = 0;
    nvs_config_get_chore_free_wd(&v);
    TEST_ASSERT_EQUAL_UINT16(90, v); /* dormant, durable, and visible to the gate */
    nvs_config_get_weekday_min(&v);
    TEST_ASSERT_EQUAL_UINT16(60, v);
}

/* C12: "the device does not block in December over a broken summer
   setting" — but it is still NAMED. So every pair is validated regardless
   of which day type is today's, and only the summer one is named here. */
void test_a_broken_non_today_pair_is_named_too(void) {
    char ack[CONFIG_ACK_MIN];
    apply(
        "{\"ver\":\"1\",\"weekday_min\":120,\"chore_free_wd\":30,"
        "\"summer_min\":60,\"chore_free_sum\":90}",
        ack, sizeof(ack));
    TEST_ASSERT_TRUE_MESSAGE(ack_names(ack, "chore_free_sum"), ack);
    TEST_ASSERT_FALSE_MESSAGE(ack_names(ack, "chore_free_wd"), ack);
}

/* `chore_free == allocation` is the per-day-type off switch (design 3.3),
   so `==` must be VALID — a hand-written `<` would reject it. */
void test_chore_free_equal_to_its_allocation_is_valid(void) {
    char ack[CONFIG_ACK_MIN];
    apply("{\"ver\":\"1\",\"weekend_min\":60,\"chore_free_we\":60}", ack, sizeof(ack));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(ack, "\"ok\":true"), ack);
}

/* Judgement call (a). cJSON preserves key order, so a cross-field check
   performed AS the field is applied compares against whichever allocation
   happens to be in NVS at that instant — and the same document then gives
   two different answers. Both orders, both directions, one test. */
void test_the_cross_field_result_is_independent_of_key_order(void) {
    char valid_a[CONFIG_ACK_MIN], valid_b[CONFIG_ACK_MIN];
    char broken_a[CONFIG_ACK_MIN], broken_b[CONFIG_ACK_MIN];
    uint16_t fa = 0, fb = 0, aa = 0, ab = 0;

    /* VALID pair. 1000 exceeds the unwritten weekday_min default (60), so
       validating the free slice before the allocation lands answers
       "invalid" — the regression this pins. */
    apply("{\"ver\":\"1\",\"weekday_min\":1440,\"chore_free_wd\":1000}", valid_a, sizeof(valid_a));
    nvs_config_get_chore_free_wd(&fa);
    nvs_config_get_weekday_min(&aa);
    mock_nvs_reset();
    apply("{\"ver\":\"1\",\"chore_free_wd\":1000,\"weekday_min\":1440}", valid_b, sizeof(valid_b));
    nvs_config_get_chore_free_wd(&fb);
    nvs_config_get_weekday_min(&ab);
    TEST_ASSERT_EQUAL_STRING(valid_a, valid_b);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(valid_a, "\"ok\":true"), valid_a);
    TEST_ASSERT_EQUAL_UINT16(fa, fb);
    TEST_ASSERT_EQUAL_UINT16(aa, ab);
    TEST_ASSERT_EQUAL_UINT16(1000, fa);
    TEST_ASSERT_EQUAL_UINT16(1440, aa);

    /* BROKEN pair, same treatment: named in both orders. */
    mock_nvs_reset();
    apply("{\"ver\":\"1\",\"weekday_min\":30,\"chore_free_wd\":1000}", broken_a, sizeof(broken_a));
    mock_nvs_reset();
    apply("{\"ver\":\"1\",\"chore_free_wd\":1000,\"weekday_min\":30}", broken_b, sizeof(broken_b));
    TEST_ASSERT_EQUAL_STRING(broken_a, broken_b);
    TEST_ASSERT_TRUE_MESSAGE(ack_names(broken_a, "chore_free_wd"), broken_a);
}

/* The other direction of the same pair: the document moves the ALLOCATION
   and says nothing about the free slice. Naming the chore_free_* key is
   deliberate — it is the actionable half. */
void test_lowering_an_allocation_names_the_stale_chore_free(void) {
    char ack[CONFIG_ACK_MIN];
    apply("{\"ver\":\"1\",\"weekday_min\":120,\"chore_free_wd\":120}", ack, sizeof(ack));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(ack, "\"ok\":true"), ack);
    apply("{\"ver\":\"2\",\"weekday_min\":30}", ack, sizeof(ack));
    TEST_ASSERT_TRUE_MESSAGE(ack_names(ack, "chore_free_wd"), ack);
}

/* Nothing re-validates a value sitting in NVS (config_validate.h says so
   in as many words), so errors[] stays a statement about THIS document: a
   pair the document does not speak to is not re-reported every window. */
void test_a_pair_the_document_does_not_touch_is_not_revalidated(void) {
    char ack[CONFIG_ACK_MIN];
    apply("{\"ver\":\"1\",\"weekday_min\":120,\"chore_free_wd\":120}", ack, sizeof(ack));
    apply("{\"ver\":\"2\",\"weekday_min\":30}", ack, sizeof(ack)); /* leaves 30/120 stored */
    apply("{\"ver\":\"3\",\"bedtime\":1900}", ack, sizeof(ack));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(ack, "\"ok\":true"), ack);
    TEST_ASSERT_FALSE_MESSAGE(ack_names(ack, "chore_free_wd"), ack);
}

/* One field, one entry. The range check and the cross-field check can both
   fire on the same key (the value is refused, so the STORED one is what
   the pair is judged on), and a duplicate entry wastes the 160-byte error
   list and reads as two separate faults. */
void test_a_field_that_fails_both_checks_is_named_once(void) {
    char ack[CONFIG_ACK_MIN];
    apply("{\"ver\":\"1\",\"weekday_min\":120,\"chore_free_wd\":120}", ack, sizeof(ack));
    apply("{\"ver\":\"2\",\"weekday_min\":30,\"chore_free_wd\":1441}", ack, sizeof(ack));
    const char *first = strstr(ack, "chore_free_wd");
    TEST_ASSERT_NOT_NULL_MESSAGE(first, ack);
    TEST_ASSERT_NULL_MESSAGE(strstr(first + 1, "chore_free_wd"), ack);
}

/* The whole feature in one document, which is also the CONFIG_BUF_MAX
   headroom case: three 20-byte names plus all four minute keys. 2048 is
   CONFIG_BUF_MAX in mqtt_ha.c, which is private to that file — and the
   gate there is `total_len < CONFIG_BUF_MAX`, so the largest document
   that is actually accepted is 2047 bytes. The `<` below is therefore the
   right comparison against 2048 by luck rather than by reasoning; it is
   two orders of magnitude clear of the real ceiling either way. */
void test_a_full_chore_document_applies_whole(void) {
    char ack[CONFIG_ACK_MIN];
    const char *doc =
        "{\"ver\":\"20260909\","
        "\"chores\":[\"12345678901234567890\",\"12345678901234567890\",\"12345678901234567890\"],"
        "\"weekday_min\":1440,\"weekend_min\":1440,\"holiday_min\":1440,\"summer_min\":1440,"
        "\"chore_free_wd\":1440,\"chore_free_we\":1440,\"chore_free_hol\":1440,"
        "\"chore_free_sum\":1440}";
    TEST_ASSERT_TRUE_MESSAGE(strlen(doc) < 2048, doc);
    TEST_ASSERT_EQUAL(CONFIG_APPLIED, apply(doc, ack, sizeof(ack)));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(ack, "\"ok\":true"), ack);
    char names[CHORE_MAX][CHORE_NAME_BUF];
    TEST_ASSERT_EQUAL_UINT8(3, stored_chores(names));
    TEST_ASSERT_EQUAL_STRING("12345678901234567890", names[2]);
}

/* The refusal ack lives here rather than in mqtt_ha.c because mqtt_ha.c
   has no host suite, and these exact bytes are what an operator — and any
   HA template sensor built on config_ack — reads. Shape follows the parse
   failure above it: no "ver", because a document that never fit the
   receive buffer was never parsed and has no version to echo. */
void test_the_too_long_ack_states_the_size_and_the_ceiling(void) {
    char ack[CONFIG_ACK_MIN];
    int n = config_ack_too_long(ack, sizeof(ack), 2500, 2047);
    TEST_ASSERT_EQUAL_STRING("{\"ok\":false,\"err\":\"too_long\",\"len\":2500,\"max\":2047}", ack);
    TEST_ASSERT_EQUAL_INT((int)strlen(ack), n);
}

/* The ack must fit CONFIG_ACK_MIN WHOLE — config_apply.h says a truncated
   ack is unparseable JSON and HA loses the whole message. Nothing bounds
   the length an MQTT broker may declare, so the numbers are pinned at the
   widest an int can print rather than at a plausible payload size. */
void test_the_too_long_ack_fits_the_minimum_ack_buffer(void) {
    char ack[CONFIG_ACK_MIN];
    int n = config_ack_too_long(ack, sizeof(ack), 2147483647, -2147483647 - 1);
    TEST_ASSERT_TRUE_MESSAGE(n < CONFIG_ACK_MIN, ack);
    TEST_ASSERT_EQUAL_INT((int)strlen(ack), n);
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
    RUN_TEST(test_chores_three_applied_and_readable_back);
    RUN_TEST(test_chores_a_fourth_entry_is_an_error_and_writes_nothing);
    RUN_TEST(test_chores_overlong_name_is_an_error_not_a_truncation);
    RUN_TEST(test_chores_name_at_the_cap_is_accepted_whole);
    RUN_TEST(test_chores_non_string_entry_is_an_error_and_writes_nothing);
    RUN_TEST(test_chores_empty_name_is_an_error);
    RUN_TEST(test_chores_name_with_a_quote_is_rejected);
    RUN_TEST(test_chores_non_array_is_an_error);
    RUN_TEST(test_chores_absent_leaves_an_existing_list_alone);
    RUN_TEST(test_chores_explicit_empty_array_clears_the_list);
    RUN_TEST(test_chores_nvs_write_failure_is_reported_and_keeps_the_live_acks);
    RUN_TEST(test_a_chore_list_edit_clears_the_live_acks_and_keeps_the_release);
    RUN_TEST(test_re_applying_the_same_chore_list_leaves_the_live_acks_alone);
    RUN_TEST(test_chores_structured_entries_are_errors_and_write_nothing);
    RUN_TEST(test_chores_five_entries_are_rejected_and_store_nothing);
    RUN_TEST(test_chores_name_with_a_backslash_or_control_byte_is_rejected);
    RUN_TEST(test_chores_a_21_byte_utf8_name_is_rejected_whole);
    RUN_TEST(test_a_rejected_chores_array_leaves_the_other_fields_applied);
    RUN_TEST(test_a_failed_chore_save_leaves_the_previous_list_readable);
    RUN_TEST(test_duplicate_chore_names_are_accepted_as_distinct_rows);
    RUN_TEST(test_chore_free_applies_at_both_bounds);
    RUN_TEST(test_chore_free_above_the_ceiling_is_named_and_not_stored);
    RUN_TEST(test_chore_free_wrong_type_is_named);
    RUN_TEST(test_chore_free_over_its_allocation_is_named);
    RUN_TEST(test_a_broken_non_today_pair_is_named_too);
    RUN_TEST(test_chore_free_equal_to_its_allocation_is_valid);
    RUN_TEST(test_the_cross_field_result_is_independent_of_key_order);
    RUN_TEST(test_lowering_an_allocation_names_the_stale_chore_free);
    RUN_TEST(test_a_pair_the_document_does_not_touch_is_not_revalidated);
    RUN_TEST(test_a_field_that_fails_both_checks_is_named_once);
    RUN_TEST(test_a_full_chore_document_applies_whole);
    RUN_TEST(test_the_too_long_ack_states_the_size_and_the_ceiling);
    RUN_TEST(test_the_too_long_ack_fits_the_minimum_ack_buffer);
    return UNITY_END();
}
