#include <stdio.h>
#include <string.h>
#include <unity.h>

/* Single-TU: the editable-config applier over mock NVS + real accessors
   and validators. No cJSON — values arrive as strings (from MQTT).

   timer.c is here because load_defs() falls back to timer_slot_def() when
   the blob cannot be read (BUG-8): the real slot table, not a stub, so the
   fallback is exercised against the same enablement rule the device uses.
   Tests that do not call timer_set_defs() see an empty table, which makes
   the fallback identical to the zeroed blob it replaced. */
// clang-format off
#include "mock_hal_nvs.c"
#include "mock_hal_time.c"
#include "../../main/timer.c"
#include "../../main/nvs_config.c"
#include "../../main/quiet_hours.c"
#include "../../main/bedtime.c"
#include "../../main/config_validate.c"
#include "../../main/tones.c"
#include "../../main/ha_config.c"
// clang-format on
/* Header only: the set/<key> transport slot the registry's advertised
   maximums have to fit through. */
#include "mqtt_rx.h"

static void seed_blob(void); /* defined with the Phase B tests below */

void setUp(void) {
    mock_nvs_reset();
    timer_set_defs(NULL, 0); /* s_defs is a static: clear it like the NVS */
}
void tearDown(void) {}

/* ---- ha_config_set: validate + persist per kind ---- */

void test_set_u16_valid_persists(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("weekday_min", "45", ack, sizeof(ack)));
    uint16_t v;
    nvs_config_get_weekday_min(&v);
    TEST_ASSERT_EQUAL_UINT16(45, v);
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"ok\":true"));
}

void test_set_u16_out_of_range_rejected(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("weekday_min", "5000", ack, sizeof(ack)));
    uint16_t v;
    nvs_config_get_weekday_min(&v);
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_WEEKDAY_MIN, v); /* unchanged */
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"ok\":false"));
}

void test_set_u16_non_numeric_rejected(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("weekday_min", "lots", ack, sizeof(ack)));
}

void test_set_hhmm_valid_and_invalid(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("quiet_start", "2130", ack, sizeof(ack)));
    uint16_t v;
    nvs_config_get_quiet_start(&v);
    TEST_ASSERT_EQUAL_UINT16(2130, v);
    /* minute 60 is not a real time */
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("quiet_start", "2160", ack, sizeof(ack)));
}

void test_set_break_interval_zero_allowed(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("break_interval_min", "0", ack, sizeof(ack)));
    uint16_t v;
    nvs_config_get_break_interval_min(&v);
    TEST_ASSERT_EQUAL_UINT16(0, v);
}

void test_set_str_valid_and_too_long(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("name", "Kitchen", ack, sizeof(ack)));
    char s[64];
    nvs_config_get_dev_name(s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("Kitchen", s);
    char toolong[64];
    memset(toolong, 'x', sizeof(toolong) - 1);
    toolong[sizeof(toolong) - 1] = '\0';
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("name", toolong, ack, sizeof(ack)));
}

void test_set_unknown_key(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_UNKNOWN, ha_config_set("nonsense", "1", ack, sizeof(ack)));
}

/* ---- validation hardening (review H3/L6, parse_int, HHMM bounds) ---- */

void test_set_str_rejects_quote_and_backslash(void) {
    char ack[128];
    /* a name containing " or \ would corrupt discovery/state JSON */
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("name", "bad\"name", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("name", "back\\slash", ack, sizeof(ack)));
    /* control characters likewise */
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("name", "line\nbreak", ack, sizeof(ack)));
    /* a clean name with an apostrophe is fine */
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("name", "Kids' Room", ack, sizeof(ack)));
}

void test_set_timer_name_rejects_quote(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("timer1_name", "a\"b", ack, sizeof(ack)));
}

void test_set_treload_rejects_non_onoff(void) {
    seed_blob();
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("timer1_reload", "on", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("timer1_reload", "true", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("timer1_reload", "", ack, sizeof(ack)));
    /* reload flag untouched by the rejected writes */
    nvs_timer_defs_blob_t b;
    nvs_config_get_timer_defs(&b);
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[0].reload);
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer1_reload", "OFF", ack, sizeof(ack)));
    nvs_config_get_timer_defs(&b);
    TEST_ASSERT_EQUAL_UINT8(0, b.defs[0].reload);
}

void test_hhmm_boundaries(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("quiet_start", "2359", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("quiet_start", "2400", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("quiet_start", "0060", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("quiet_start", "-1", ack, sizeof(ack)));
}

void test_parse_int_trailing_junk_rejected(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("weekday_min", "45x", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("weekday_min", " 45 ", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("weekday_min", "", ack, sizeof(ack)));
}

/* ---- NVS write failure surfaces as a rejection (review M4) ---- */

void test_set_nvs_failure_rejected(void) {
    char ack[128];
    mock_nvs_fail_writes(1);
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("weekday_min", "45", ack, sizeof(ack)));
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"err\":\"nvs\""));
}

void test_set_timer_nvs_failure_rejected(void) {
    char ack[128];
    mock_nvs_fail_writes(1);
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("timer1_min", "25", ack, sizeof(ack)));
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"err\":\"nvs\""));
}

/* ---- state JSON: escaping + worst-case size fits the firmware buffer ---- */

void test_state_json_escapes_specials(void) {
    /* config_apply (cJSON) can write a name with a quote even though the
       set path rejects one — the state builder must still escape it. */
    nvs_config_set_dev_name("a\"b\\c");
    char buf[HA_CONFIG_STATE_MAX];
    ha_config_state_json(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"name\":\"a\\\"b\\\\c\""));
}

/* Fill `dst` (a buffer of `cap`) with the longest value it can hold, made
   entirely of characters ha_config_json_escape DOUBLES. The escaped form
   is what the buffer actually has to hold, so a worst case built from
   plain ASCII understates it by half. */
static void fill_escapable(char *dst, size_t cap) {
    for (size_t i = 0; i + 1 < cap; i++)
        dst[i] = (i % 2) ? '\\' : '"';
    dst[cap - 1] = '\0';
}

void test_state_json_worst_case_fits_firmware_buffer(void) {
    char ack[128];
    /* Every numeric field at its widest rendering. */
    ha_config_set("weekday_min", "1440", ack, sizeof(ack));
    ha_config_set("weekend_min", "1440", ack, sizeof(ack));
    ha_config_set("holiday_min", "1440", ack, sizeof(ack));
    ha_config_set("summer_min", "1440", ack, sizeof(ack));
    ha_config_set("break_interval_min", "480", ack, sizeof(ack));
    ha_config_set("break_duration_min", "120", ack, sizeof(ack));
    ha_config_set("quiet_start", "2359", ack, sizeof(ack));
    ha_config_set("quiet_end", "2359", ack, sizeof(ack));
    ha_config_set("bedtime", "2359", ack, sizeof(ack));
    ha_config_set("alert_volume", "200", ack, sizeof(ack));
    /* Selects render the OPTION STRING, so all three go to the longest
       one — two of the defaults are shorter, which is part of why this
       guard used to read ~190 B under the truth. */
    ha_config_set("tone_expiry", "Marimba arpeggio", ack, sizeof(ack));
    ha_config_set("tone_break", "Marimba arpeggio", ack, sizeof(ack));
    ha_config_set("tone_bed", "Marimba arpeggio", ack, sizeof(ack));
    ha_config_set("ota_on_sync", "ON", ack, sizeof(ack));

    /* Strings: maxed to their declared bound AND made of characters the
       escaper doubles. ha_config_set rejects quote/backslash, but the
       bulk-document path reaches these same fields with no cleanliness
       check (config_apply passes a NULL validator for name/tz, and
       apply_timers has none at all), so these values are reachable on a
       real device — see test_state_json_escapes_specials. Write them the
       way that path does, through the accessors. */
    char name[CFG_BOUND_NAME_MAX];
    fill_escapable(name, sizeof(name));
    nvs_config_set_dev_name(name);
    char tz[CFG_BOUND_TZ_MAX];
    fill_escapable(tz, sizeof(tz));
    nvs_config_set_tz(tz);

    nvs_timer_defs_blob_t defs;
    memset(&defs, 0, sizeof(defs));
    defs.version = TIMER_DEFS_BLOB_VERSION;
    for (int i = 0; i < TIMER_EXTRA_SLOTS; i++) {
        fill_escapable(defs.defs[i].name, sizeof(defs.defs[i].name));
        defs.defs[i].min = 1440;
        defs.defs[i].reload = 1;
        defs.defs[i].break_eligible = 1;
    }
    nvs_config_set_timer_defs(&defs);

    /* The OTA endpoint is the longest CFG_STR in the registry. It keeps a
       real https:// prefix because config_is_ota_url would reject
       anything else, and the set path is the only way it is written. */
    char url[CFG_BOUND_OTA_URL_MAX];
    memset(url, 'u', sizeof(url) - 1);
    memcpy(url, "https://", 8);
    url[sizeof(url) - 1] = '\0';
    ha_config_set("ota_url", url, ack, sizeof(ack));

    char buf[HA_CONFIG_STATE_MAX];
    int ret = ha_config_state_json(buf, sizeof(buf));
    TEST_ASSERT_TRUE(ret < HA_CONFIG_STATE_MAX); /* not truncated */
    TEST_ASSERT_EQUAL_INT((int)strlen(buf), ret);
    /* Headroom the firmware buffer actually has, so a future field
       addition trips here rather than silently knocking every editable
       control offline (a truncated doc is never published). */
    printf("  worst-case cfg state: %d / %d bytes\n", ret, HA_CONFIG_STATE_MAX);
}

/* ---- ha_config_state_json ---- */

void test_state_json_reports_current_values(void) {
    ha_config_set("weekday_min", "45", (char[128]){0}, 128);
    ha_config_set("name", "Kitchen", (char[128]){0}, 128);
    char buf[512];
    ha_config_state_json(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"weekday_min\":45"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"name\":\"Kitchen\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"quiet_start\":")); /* default present */
}

/* ---- discovery payloads ---- */

static const cfg_field_t *field_by_key(const char *key) {
    int n = 0;
    const cfg_field_t *f = ha_config_fields(&n);
    for (int i = 0; i < n; i++)
        if (strcmp(f[i].key, key) == 0)
            return &f[i];
    return NULL;
}

void test_discovery_number_has_command_bounds_and_config_category(void) {
    const cfg_field_t *f = field_by_key("weekday_min");
    TEST_ASSERT_NOT_NULL(f);
    char buf[700];
    ha_config_discovery(buf, sizeof(buf), "magtag-a1b2c3", "Kitchen MagTag", "fw", f);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"cmd_t\":\"magtag/magtag-a1b2c3/set/weekday_min\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"stat_t\":\"magtag/magtag-a1b2c3/cfg\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "value_json.weekday_min"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"min\":1"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"max\":1440"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"step\":1"));          /* round values must be valid in HA */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"mode\":\"box\""));    /* numeric entry, not a slider */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"optimistic\":true")); /* no snap-back on edit */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ent_cat\":\"config\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"retain\":true")); /* command retained for the sleeping device */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ids\":[\"magtag-a1b2c3\"]"));
}

void test_discovery_text_has_mode(void) {
    const cfg_field_t *f = field_by_key("name");
    char buf[700];
    ha_config_discovery(buf, sizeof(buf), "magtag-a1b2c3", "Kitchen", "fw", f);
    TEST_ASSERT_EQUAL_STRING("text", f->component);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"mode\":\"text\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"cmd_t\":\"magtag/magtag-a1b2c3/set/name\""));
}

/* Every text entity must advertise its length cap. HA's text platform
   defaults max to 255, so without this the UI accepts a name the device
   then rejects with "len" — and the rejection is invisible unless you are
   watching the ack topic. `hi` is the buffer size, so the longest string
   that fits is hi - 1. */
void test_discovery_text_advertises_max_length(void) {
    char buf[700];
    const struct {
        const char *key;
        const char *max;
    } cases[] = {
        {"name", "\"max\":31"},        /* CFG_BOUND_NAME_MAX 32 */
        {"tz", "\"max\":47"},          /* CFG_BOUND_TZ_MAX 48 */
        {"timer1_name", "\"max\":15"}, /* blob name field is 16 bytes */
        {"timer4_name", "\"max\":15"},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const cfg_field_t *f = field_by_key(cases[i].key);
        TEST_ASSERT_NOT_NULL(f);
        TEST_ASSERT_EQUAL_STRING("text", f->component);
        ha_config_discovery(buf, sizeof(buf), "magtag-a1b2c3", "K", "fw", f);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, cases[i].max), cases[i].key);
    }
}

/* The advertised max and the device-side check must be the same number:
   an HA UI that accepts exactly max characters must never produce a
   value the device rejects. 15 fits, 16 does not. */
void test_timer_name_max_matches_the_reject_boundary(void) {
    const cfg_field_t *f = field_by_key("timer1_name");
    TEST_ASSERT_NOT_NULL(f);
    char ack[128], name[64];
    memset(name, 'x', sizeof(name));
    name[f->hi - 1] = '\0'; /* exactly the advertised max */
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer1_name", name, ack, sizeof(ack)));
    name[f->hi - 1] = 'x';
    name[f->hi] = '\0'; /* one over */
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("timer1_name", name, ack, sizeof(ack)));
    TEST_ASSERT_NOT_NULL(strstr(ack, "len"));
}

void test_discovery_topic(void) {
    const cfg_field_t *f = field_by_key("weekday_min");
    char buf[128];
    ha_config_discovery_topic(buf, sizeof(buf), "magtag-a1b2c3", f);
    TEST_ASSERT_EQUAL_STRING("homeassistant/number/magtag-a1b2c3_weekday_min/config", buf);
}

/* ---- bedtime: field-specific HHMM validity (0 or 1800-2359) ---- */

void test_set_bedtime_accepts_evening_and_zero(void) {
    char ack[128];
    uint16_t v;
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("bedtime", "1800", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("bedtime", "2359", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("bedtime", "0", ack, sizeof(ack)));
    nvs_config_get_bedtime(&v);
    TEST_ASSERT_EQUAL_UINT16(0, v); /* disabled sticks */
}

void test_set_bedtime_rejects_daytime(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("bedtime", "1759", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("bedtime", "900", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("bedtime", "2400", ack, sizeof(ack)));
    uint16_t v;
    nvs_config_get_bedtime(&v);
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_BEDTIME, v); /* unchanged */
    /* quiet fields keep their looser any-clock-time rule */
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("quiet_start", "900", ack, sizeof(ack)));
}

/* ---- alert-tone selects (CFG_ENUM) ---- */

void test_set_tone_select_valid_option_persists(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("tone_expiry", "Gran Vals", ack, sizeof(ack)));
    uint16_t v = 0;
    nvs_config_get_tone_expiry(&v);
    TEST_ASSERT_EQUAL_UINT16(TONE_GRANVALS, v);
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"ok\":true"));
}

void test_set_tone_select_rejects_unknown_and_wrong_case(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("tone_break", "Kazoo", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("tone_break", "gran vals", ack, sizeof(ack)));
    uint16_t v = 0;
    nvs_config_get_tone_break(&v);
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_TONE_BREAK, v); /* unchanged */
    TEST_ASSERT_NOT_NULL(strstr(ack, "option"));
}

void test_state_json_emits_tone_option_strings(void) {
    char buf[HA_CONFIG_STATE_MAX];
    ha_config_state_json(buf, sizeof(buf)); /* defaults */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"tone_expiry\":\"Marimba arpeggio\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"tone_break\":\"Gentle chime\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"tone_bed\":\"Gran Vals\""));
}

void test_state_json_clamps_out_of_range_tone_index(void) {
    nvs_config_set_tone_expiry(999); /* e.g. stored by a future firmware */
    char buf[HA_CONFIG_STATE_MAX];
    ha_config_state_json(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"tone_expiry\":\"Classic beep\""));
}

void test_discovery_select_lists_options(void) {
    const cfg_field_t *f = field_by_key("tone_bed");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_STRING("select", f->component);
    char buf[700];
    ha_config_discovery(buf, sizeof(buf), "magtag-a1b2c3", "K", "fw", f);
    TEST_ASSERT_NOT_NULL(strstr(buf,
                                "\"options\":[\"Classic beep\",\"Ding-ding\",\"Gentle chime\","
                                "\"Marimba arpeggio\",\"Gran Vals\",\"Custom WAV\"]"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"optimistic\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"cmd_t\":\"magtag/magtag-a1b2c3/set/tone_bed\""));
}

/* ---- alert volume (CFG_U16, percent with boost range) ---- */

void test_set_alert_volume_valid_persists(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("alert_volume", "80", ack, sizeof(ack)));
    uint16_t v = 0;
    nvs_config_get_alert_volume(&v);
    TEST_ASSERT_EQUAL_UINT16(80, v);
    /* 0 = mute and the boost ceiling are both legal */
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("alert_volume", "0", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("alert_volume", "200", ack, sizeof(ack)));
}

void test_set_alert_volume_out_of_range_rejected(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("alert_volume", "201", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("alert_volume", "-1", ack, sizeof(ack)));
    uint16_t v = 0;
    nvs_config_get_alert_volume(&v);
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_ALERT_VOLUME, v); /* unchanged */
}

void test_alert_volume_discovery_and_state(void) {
    const cfg_field_t *f = field_by_key("alert_volume");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_STRING("number", f->component);
    char buf[700];
    ha_config_discovery(buf, sizeof(buf), "magtag-a1b2c3", "K", "fw", f);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"min\":0"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"max\":200"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"unit_of_meas\":\"%\""));
    char state[HA_CONFIG_STATE_MAX];
    ha_config_state_json(state, sizeof(state));
    char expect[48];
    snprintf(expect, sizeof(expect), "\"alert_volume\":%u", (unsigned)NVS_DEFAULT_ALERT_VOLUME);
    TEST_ASSERT_NOT_NULL(strstr(state, expect));
}

/* ---- Phase B: editable timer definitions (read-modify-write the blob) ---- */

static void seed_blob(void) {
    nvs_timer_defs_blob_t b;
    memset(&b, 0, sizeof(b));
    b.version = TIMER_DEFS_BLOB_VERSION;
    snprintf(b.defs[0].name, sizeof(b.defs[0].name), "Piano");
    b.defs[0].min = 15;
    b.defs[0].reload = 1;
    nvs_config_set_timer_defs(&b);
}

void test_set_timer_name_enables_slot(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer2_name", "Running", ack, sizeof(ack)));
    nvs_timer_defs_blob_t b;
    nvs_config_get_timer_defs(&b);
    TEST_ASSERT_EQUAL_STRING("Running", b.defs[1].name);
}

void test_set_timer_name_empty_disables_slot(void) {
    seed_blob();
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer1_name", "", ack, sizeof(ack)));
    nvs_timer_defs_blob_t b;
    nvs_config_get_timer_defs(&b);
    TEST_ASSERT_EQUAL_STRING("", b.defs[0].name); /* disabled */
}

void test_set_timer_name_too_long_rejected(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("timer1_name", "SixteenCharsPlus!", ack, sizeof(ack)));
}

/* Per-timer break-eligible switch: the HA-facing half of the flag. */
void test_set_timer_break_eligible(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer1_break", "ON", ack, sizeof(ack)));
    nvs_timer_defs_blob_t b;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[0].break_eligible);
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer1_break", "OFF", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
    TEST_ASSERT_EQUAL_UINT8(0, b.defs[0].break_eligible);
}

void test_set_timer_break_eligible_rejects_non_onoff(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer1_break", "ON", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("timer1_break", "true", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("timer1_break", "", ack, sizeof(ack)));
    nvs_timer_defs_blob_t b;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[0].break_eligible); /* untouched */
}

/* Editing one per-slot field must not clobber its siblings: they all
   read-modify-write the same blob. */
void test_timer_fields_are_independent(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer1_break", "ON", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer1_reload", "ON", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer1_min", "25", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer1_name", "Violin", ack, sizeof(ack)));
    nvs_timer_defs_blob_t b;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
    TEST_ASSERT_EQUAL_STRING("Violin", b.defs[0].name);
    TEST_ASSERT_EQUAL_INT32(25, b.defs[0].min);
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[0].reload);
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[0].break_eligible);
}

void test_set_timer_min_and_reload(void) {
    seed_blob();
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer1_min", "25", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer1_reload", "OFF", ack, sizeof(ack)));
    nvs_timer_defs_blob_t b;
    nvs_config_get_timer_defs(&b);
    TEST_ASSERT_EQUAL_INT32(25, b.defs[0].min);
    TEST_ASSERT_EQUAL_UINT8(0, b.defs[0].reload);
    TEST_ASSERT_EQUAL_STRING("Piano", b.defs[0].name); /* name preserved (read-modify-write) */
}

void test_set_timer_min_out_of_range_rejected(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("timer1_min", "9999", ack, sizeof(ack)));
}

void test_state_json_includes_timer_fields(void) {
    seed_blob();
    char buf[768];
    ha_config_state_json(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timer1_name\":\"Piano\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timer1_min\":15"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timer1_reload\":\"ON\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timer1_break\":\"OFF\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timer2_name\":\"\"")); /* empty slot */
}

void test_discovery_timer_reload_is_switch(void) {
    const cfg_field_t *f = field_by_key("timer1_reload");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_STRING("switch", f->component);
    char buf[700];
    ha_config_discovery(buf, sizeof(buf), "magtag-a1b2c3", "K", "fw", f);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"pl_on\":\"ON\""));
    /* Optimistic = assumed-state: HA renders it as two lightning-bolt
       buttons instead of a toggle. Tried both on-device; the user prefers
       that over the non-optimistic toggle's snap-back on every flip. */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"optimistic\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"cmd_t\":\"magtag/magtag-a1b2c3/set/timer1_reload\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "value_json.timer1_reload"));
}

/* ---- OTA fields: generic CFG_BOOL switch + validated text ---- */

void test_set_ota_url_https_persists(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("ota_url", "https://example.com/ota.json", ack, sizeof(ack)));
    char s[CFG_BOUND_OTA_URL_MAX];
    nvs_config_get_ota_url(s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("https://example.com/ota.json", s);
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"ok\":true"));
}

void test_set_ota_url_rejects_non_https(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("ota_url", "https://good.example/ota.json", ack, sizeof(ack)));
    /* http:// would make the update channel unauthenticated */
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("ota_url", "http://evil.example/ota.json", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("ota_url", "evil.example/ota.json", ack, sizeof(ack)));
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"ok\":false"));
    /* the good value is still there — a rejected set writes nothing */
    char s[CFG_BOUND_OTA_URL_MAX];
    nvs_config_get_ota_url(s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("https://good.example/ota.json", s);
}

void test_set_ota_url_empty_disables_ota(void) {
    char ack[128];
    ha_config_set("ota_url", "https://example.com/ota.json", ack, sizeof(ack));
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("ota_url", "", ack, sizeof(ack)));
    char s[CFG_BOUND_OTA_URL_MAX];
    nvs_config_get_ota_url(s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("", s);
}

void test_set_ota_url_too_long_rejected(void) {
    char ack[128];
    char url[CFG_BOUND_OTA_URL_MAX + 8];
    memset(url, 'u', sizeof(url) - 1);
    memcpy(url, "https://", 8);
    url[sizeof(url) - 1] = '\0';
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("ota_url", url, ack, sizeof(ack)));
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"err\":\"len\""));
}

void test_set_ota_on_sync_switch_round_trip(void) {
    char ack[128];
    uint16_t v;
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("ota_on_sync", "ON", ack, sizeof(ack)));
    nvs_config_get_ota_on_sync(&v);
    TEST_ASSERT_EQUAL_UINT16(1, v);
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("ota_on_sync", "OFF", ack, sizeof(ack)));
    nvs_config_get_ota_on_sync(&v);
    TEST_ASSERT_EQUAL_UINT16(0, v);
}

void test_set_ota_on_sync_rejects_non_onoff(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("ota_on_sync", "1", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("ota_on_sync", "on", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("ota_on_sync", "", ack, sizeof(ack)));
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"err\":\"onoff\""));
}

void test_set_ota_on_sync_nvs_failure_rejected(void) {
    char ack[128];
    mock_nvs_fail_writes(1);
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("ota_on_sync", "ON", ack, sizeof(ack)));
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"err\":\"nvs\""));
}

void test_state_json_includes_ota_fields(void) {
    char ack[128];
    ha_config_set("ota_url", "https://example.com/ota.json", ack, sizeof(ack));
    ha_config_set("ota_on_sync", "ON", ack, sizeof(ack));
    char buf[HA_CONFIG_STATE_MAX];
    ha_config_state_json(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ota_url\":\"https://example.com/ota.json\""));
    /* CFG_BOOL renders as the discovery pl_on/pl_off strings, like the
       slot-bound switches — not as 0/1. */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ota_on_sync\":\"ON\""));
    ha_config_set("ota_on_sync", "OFF", ack, sizeof(ack));
    ha_config_state_json(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ota_on_sync\":\"OFF\""));
}

/* A stored value from another firmware (or a corrupt one) must still
   render as a legal HA switch state. */
void test_state_json_ota_on_sync_nonzero_reads_as_on(void) {
    nvs_config_set_ota_on_sync(7);
    char buf[HA_CONFIG_STATE_MAX];
    ha_config_state_json(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ota_on_sync\":\"ON\""));
}

void test_discovery_ota_on_sync_is_switch(void) {
    const cfg_field_t *f = field_by_key("ota_on_sync");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_STRING("switch", f->component);
    TEST_ASSERT_EQUAL(0, f->slot); /* generic, not slot-bound */
    char buf[700];
    ha_config_discovery(buf, sizeof(buf), "magtag-a1b2c3", "K", "fw", f);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"pl_on\":\"ON\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"pl_off\":\"OFF\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"optimistic\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"cmd_t\":\"magtag/magtag-a1b2c3/set/ota_on_sync\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "value_json.ota_on_sync"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ent_cat\":\"config\""));
}

void test_discovery_ota_url_is_text_with_max(void) {
    const cfg_field_t *f = field_by_key("ota_url");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_STRING("text", f->component);
    char buf[700];
    ha_config_discovery(buf, sizeof(buf), "magtag-a1b2c3", "K", "fw", f);
    char expect[32];
    snprintf(expect, sizeof(expect), "\"max\":%d", CFG_BOUND_OTA_URL_MAX - 1);
    TEST_ASSERT_NOT_NULL(strstr(buf, expect));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"cmd_t\":\"magtag/magtag-a1b2c3/set/ota_url\""));
}

/* ---- discovery freshness fingerprint ----
   mqtt_ha.c republishes discovery only when the schema version or this
   fingerprint changed. Discovery carries BOTH dev.name and dev.sw, so
   both have to be in it. */

void test_device_hash_is_deterministic(void) {
    TEST_ASSERT_EQUAL_UINT16(ha_config_device_hash("Kitchen", "1.5.0"), ha_config_device_hash("Kitchen", "1.5.0"));
}

/* THE regression: without fw in the fingerprint, an OTA update leaves the
   HA device card showing the version it was first discovered with,
   forever — a DISC_SCHEMA_VER bump masks it exactly once. */
void test_device_hash_changes_when_firmware_version_changes(void) {
    TEST_ASSERT_NOT_EQUAL(ha_config_device_hash("Kitchen", "1.5.0"), ha_config_device_hash("Kitchen", "1.6.0"));
    /* a build-metadata-only difference still has to republish */
    TEST_ASSERT_NOT_EQUAL(ha_config_device_hash("Kitchen", "1.5.0"), ha_config_device_hash("Kitchen", "1.5.0-dirty"));
}

void test_device_hash_changes_when_name_changes(void) {
    TEST_ASSERT_NOT_EQUAL(ha_config_device_hash("Kitchen", "1.5.0"), ha_config_device_hash("Playroom", "1.5.0"));
}

/* The two strings are fingerprinted together, so the boundary between
   them must be marked or a rename could cancel a version bump out. */
void test_device_hash_does_not_confuse_the_field_boundary(void) {
    TEST_ASSERT_NOT_EQUAL(ha_config_device_hash("ab", "c"), ha_config_device_hash("a", "bc"));
}

void test_device_hash_tolerates_null(void) {
    /* fw is a pointer off the stat snapshot; a NULL must not crash the
       window (ASan would catch a deref here). */
    ha_config_device_hash(NULL, NULL);
    TEST_ASSERT_NOT_EQUAL(ha_config_device_hash("Kitchen", NULL), ha_config_device_hash("Kitchen", "1.5.0"));
}

/* ---- the discovery gate itself ----
   The compare/publish/stamp wiring lives in a static function in
   mqtt_ha.c, which has no host suite; this is the predicate it calls. */

void test_discovery_stale_on_a_fresh_device(void) {
    /* Blank NVS reads 0/0 — must republish, or a new device gets no
       entities at all. */
    TEST_ASSERT_TRUE(ha_config_discovery_stale(0, 0, 17, 0x1234));
}

void test_discovery_not_stale_when_both_match(void) {
    TEST_ASSERT_FALSE(ha_config_discovery_stale(17, 0x1234, 17, 0x1234));
}

void test_discovery_stale_on_schema_bump_alone(void) {
    TEST_ASSERT_TRUE(ha_config_discovery_stale(16, 0x1234, 17, 0x1234));
}

/* The leg this package added: no schema bump, only a new fingerprint
   (renamed device, new firmware version, renamed timer slot). */
void test_discovery_stale_on_fingerprint_change_alone(void) {
    TEST_ASSERT_TRUE(ha_config_discovery_stale(17, 0x1234, 17, 0x5678));
}

/* ---- slot names in the discovery fingerprint ----
   mqtt_ha publishes `<Name> remaining` / `<Name> limit` / `<Name> runs`
   per enabled slot, named from the HA-editable timerN_name. Neither the
   name nor the slot's existence is in DISC_SCHEMA_VER. */

void test_discovery_hash_changes_when_a_slot_is_renamed(void) {
    char ack[128];
    ha_config_set("timer1_name", "Piano", ack, sizeof(ack));
    uint16_t before = ha_config_discovery_hash("Kitchen", "1.5.0");
    ha_config_set("timer1_name", "Violin", ack, sizeof(ack));
    TEST_ASSERT_NOT_EQUAL(before, ha_config_discovery_hash("Kitchen", "1.5.0"));
}

void test_discovery_hash_changes_when_a_slot_is_enabled_or_cleared(void) {
    char ack[128];
    uint16_t empty = ha_config_discovery_hash("Kitchen", "1.5.0");
    ha_config_set("timer2_name", "Reading", ack, sizeof(ack));
    uint16_t enabled = ha_config_discovery_hash("Kitchen", "1.5.0");
    TEST_ASSERT_NOT_EQUAL(empty, enabled);
    /* Clearing the name retires those entities — also a discovery change. */
    ha_config_set("timer2_name", "", ack, sizeof(ack));
    TEST_ASSERT_NOT_EQUAL(enabled, ha_config_discovery_hash("Kitchen", "1.5.0"));
}

/* Slot names are folded with the same separator discipline as the dev
   block, so shifting a name between slots cannot cancel out. */
void test_discovery_hash_does_not_confuse_slot_boundaries(void) {
    char ack[128];
    ha_config_set("timer1_name", "ab", ack, sizeof(ack));
    ha_config_set("timer2_name", "", ack, sizeof(ack));
    uint16_t a = ha_config_discovery_hash("Kitchen", "1.5.0");
    ha_config_set("timer1_name", "a", ack, sizeof(ack));
    ha_config_set("timer2_name", "b", ack, sizeof(ack));
    TEST_ASSERT_NOT_EQUAL(a, ha_config_discovery_hash("Kitchen", "1.5.0"));
}

void test_discovery_hash_still_tracks_the_device_block(void) {
    /* The dev-block legs must survive being folded together with slots. */
    TEST_ASSERT_NOT_EQUAL(ha_config_discovery_hash("Kitchen", "1.5.0"), ha_config_discovery_hash("Kitchen", "1.6.0"));
    TEST_ASSERT_NOT_EQUAL(ha_config_discovery_hash("Kitchen", "1.5.0"), ha_config_discovery_hash("Playroom", "1.5.0"));
}

/* ---- transport ----
   The set/<key> slot must be able to carry the longest value any field
   advertises to HA. It could not: ota_url advertised max 127 while the
   slot capped a payload at 79, so the endpoint was dropped in mqtt_rx —
   no ack, retained command never cleared, re-dropped every window, and
   OTA silently never turned on. Walk the registry, don't spot-check. */

void test_every_string_field_fits_the_set_transport(void) {
    int n = 0;
    const cfg_field_t *f = ha_config_fields(&n);
    for (int i = 0; i < n; i++) {
        if (f[i].kind != CFG_STR && f[i].kind != CFG_TNAME)
            continue;
        /* hi is the buffer size, so hi-1 chars must fit the value slot. */
        TEST_ASSERT_TRUE_MESSAGE((size_t)f[i].hi <= sizeof(((mqtt_set_kv_t *)0)->value), f[i].key);
    }
}

void test_every_field_key_fits_the_set_transport(void) {
    int n = 0;
    const cfg_field_t *f = ha_config_fields(&n);
    for (int i = 0; i < n; i++)
        TEST_ASSERT_TRUE_MESSAGE(strlen(f[i].key) < sizeof(((mqtt_set_kv_t *)0)->key), f[i].key);
}

/* ---- enablement is the name AND the duration ----
   timer.c gates a slot on name[0] != 0 AND duration > 0, and CFG_TNAME
   writes only the name. A name-only fold therefore missed the moment a
   slot actually became enabled. */

/* THE broken flow: two edits in two different device windows. */
void test_discovery_hash_changes_when_only_the_duration_enables_a_slot(void) {
    char ack[128];
    /* Window 1: name set, min still 0 -> slot is NOT yet enabled. */
    ha_config_set("timer4_name", "Yoga", ack, sizeof(ack));
    uint16_t named_but_disabled = ha_config_discovery_hash("Kitchen", "1.5.0");
    /* Window 2: min set -> slot flips to enabled and its three per-slot
       entities should now appear. The name did not change. */
    ha_config_set("timer4_min", "20", ack, sizeof(ack));
    TEST_ASSERT_NOT_EQUAL(named_but_disabled, ha_config_discovery_hash("Kitchen", "1.5.0"));
}

/* The reviewer's measured collision: these two hashed identically. */
void test_discovery_hash_separates_zero_and_nonzero_duration(void) {
    char ack[128];
    nvs_timer_defs_blob_t b;
    memset(&b, 0, sizeof(b));
    b.version = TIMER_DEFS_BLOB_VERSION;
    snprintf(b.defs[0].name, sizeof(b.defs[0].name), "Piano");
    b.defs[0].min = 20;
    nvs_config_set_timer_defs(&b);
    uint16_t with_duration = ha_config_discovery_hash("Kitchen", "1.5.0");
    b.defs[0].min = 0;
    nvs_config_set_timer_defs(&b);
    TEST_ASSERT_NOT_EQUAL(with_duration, ha_config_discovery_hash("Kitchen", "1.5.0"));
    (void)ack;
}

/* Discovery depends on entity NAMES and existence, not on how long the
   timer runs — so a plain duration edit between two enabled values must
   NOT force a full discovery republish (a wasted radio burst on battery). */
void test_discovery_hash_ignores_a_duration_change_that_keeps_it_enabled(void) {
    nvs_timer_defs_blob_t b;
    memset(&b, 0, sizeof(b));
    b.version = TIMER_DEFS_BLOB_VERSION;
    snprintf(b.defs[0].name, sizeof(b.defs[0].name), "Piano");
    b.defs[0].min = 20;
    nvs_config_set_timer_defs(&b);
    uint16_t at20 = ha_config_discovery_hash("Kitchen", "1.5.0");
    b.defs[0].min = 30;
    nvs_config_set_timer_defs(&b);
    TEST_ASSERT_EQUAL_UINT16(at20, ha_config_discovery_hash("Kitchen", "1.5.0"));
}

/* An unterminated name in the blob must not read into the next slot. */
void test_discovery_hash_tolerates_an_unterminated_slot_name(void) {
    nvs_timer_defs_blob_t b;
    memset(&b, 0, sizeof(b));
    b.version = TIMER_DEFS_BLOB_VERSION;
    memset(b.defs[0].name, 'x', sizeof(b.defs[0].name)); /* no NUL */
    b.defs[0].min = 20;
    nvs_config_set_timer_defs(&b);
    ha_config_discovery_hash("Kitchen", "1.5.0"); /* ASan catches an overrun */
    char buf[HA_CONFIG_STATE_MAX];
    ha_config_state_json(buf, sizeof(buf)); /* same field, via jesc */
}

/* ---- BUG-8: load_defs()'s fallback when the blob cannot be read ----

   timer_defs_install() no longer materializes the blob at boot, so
   "unreadable" is the ordinary state of a device whose NVS was erased —
   not an exotic error. load_defs() therefore falls back to the table the
   boot installed rather than to zeros, and all three of its callers are
   pinned below. */

/* The table timer_defs_install() would have left in place. File scope
   because timer.c keeps the pointers, not copies. */
static const timer_def_t INSTALLED[TIMER_SLOT_COUNT] = {
    {"Screen", 0, false, false}, {"Piano", 15 * 60, true, true}, {"Meditation", 10 * 60, false, true},
    {"", 0, false, false},       {"", 0, false, false},
};

/* Store a blob the getter refuses. A stale layout version is the
   version-drift loss the entry lists as candidate 2, and it makes the read
   fail without adding a read-failure hook to a mock 39 other suites share. */
static void store_unreadable_blob(void) {
    nvs_timer_defs_blob_t b;
    memset(&b, 0, sizeof(b));
    b.version = (uint8_t)(TIMER_DEFS_BLOB_VERSION - 1);
    snprintf(b.defs[0].name, sizeof(b.defs[0].name), "Stale");
    nvs_config_set_timer_defs(&b);
    nvs_timer_defs_blob_t probe;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&probe));
}

/* The caller with teeth. A read-modify-write on an unreadable blob used to
   persist ZEROES for every slot the edit did not touch, so one transient
   failure while the operator nudged timer2_min wiped timer1 outright. */
void test_edit_with_unreadable_blob_keeps_the_other_slots(void) {
    timer_set_defs(INSTALLED, TIMER_SLOT_COUNT);
    store_unreadable_blob();
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer2_min", "25", ack, sizeof(ack)));
    nvs_timer_defs_blob_t b;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
    TEST_ASSERT_EQUAL_INT32(25, b.defs[1].min);        /* the edit landed */
    TEST_ASSERT_EQUAL_STRING("Piano", b.defs[0].name); /* the bystander survived */
    TEST_ASSERT_EQUAL_INT32(15, b.defs[0].min);
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[0].reload);
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[0].break_eligible);
    TEST_ASSERT_EQUAL_STRING("Meditation", b.defs[1].name);
}

/* The cfg state drives HA's text/number/switch controls. Empty names here
   are what the device would publish on the first window after an NVS erase
   if the fallback were still zeros. */
void test_state_json_falls_back_to_the_installed_table(void) {
    timer_set_defs(INSTALLED, TIMER_SLOT_COUNT);
    store_unreadable_blob();
    char buf[HA_CONFIG_STATE_MAX];
    ha_config_state_json(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timer1_name\":\"Piano\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timer1_min\":15"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timer1_break\":\"ON\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timer2_name\":\"Meditation\""));
}

/* The discovery hash is the caller that cannot wait for someone else to
   write the blob: mqtt_ha.c runs publish_states() BEFORE apply_incoming()
   applies the retained document. It also has to agree with the per-timer
   entities mqtt_ha.c builds from this same timer_slot_def(). */
void test_discovery_hash_falls_back_to_the_installed_table(void) {
    timer_set_defs(INSTALLED, TIMER_SLOT_COUNT);
    store_unreadable_blob();
    uint16_t from_installed = ha_config_discovery_hash("Kitchen", "1.5.0");

    /* The same two slots, but stored: the fallback must reach the same
       fingerprint, or the first window after an erase burns a discovery
       republish and the second burns another one changing it back. */
    mock_nvs_reset();
    nvs_timer_defs_blob_t b;
    memset(&b, 0, sizeof(b));
    b.version = TIMER_DEFS_BLOB_VERSION;
    snprintf(b.defs[0].name, sizeof(b.defs[0].name), "Piano");
    b.defs[0].min = 15;
    snprintf(b.defs[1].name, sizeof(b.defs[1].name), "Meditation");
    b.defs[1].min = 10;
    nvs_config_set_timer_defs(&b);
    TEST_ASSERT_EQUAL_UINT16(ha_config_discovery_hash("Kitchen", "1.5.0"), from_installed);

    /* And it is not the all-empty hash the zeroed fallback produced. */
    mock_nvs_reset();
    timer_set_defs(NULL, 0);
    TEST_ASSERT_NOT_EQUAL(from_installed, ha_config_discovery_hash("Kitchen", "1.5.0"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_set_timer_name_enables_slot);
    RUN_TEST(test_set_timer_name_empty_disables_slot);
    RUN_TEST(test_set_timer_name_too_long_rejected);
    RUN_TEST(test_set_timer_break_eligible);
    RUN_TEST(test_set_timer_break_eligible_rejects_non_onoff);
    RUN_TEST(test_timer_fields_are_independent);
    RUN_TEST(test_set_timer_min_and_reload);
    RUN_TEST(test_set_timer_min_out_of_range_rejected);
    RUN_TEST(test_state_json_includes_timer_fields);
    RUN_TEST(test_discovery_timer_reload_is_switch);
    RUN_TEST(test_set_u16_valid_persists);
    RUN_TEST(test_set_u16_out_of_range_rejected);
    RUN_TEST(test_set_u16_non_numeric_rejected);
    RUN_TEST(test_set_hhmm_valid_and_invalid);
    RUN_TEST(test_set_break_interval_zero_allowed);
    RUN_TEST(test_set_str_valid_and_too_long);
    RUN_TEST(test_set_unknown_key);
    RUN_TEST(test_set_str_rejects_quote_and_backslash);
    RUN_TEST(test_set_timer_name_rejects_quote);
    RUN_TEST(test_set_treload_rejects_non_onoff);
    RUN_TEST(test_hhmm_boundaries);
    RUN_TEST(test_parse_int_trailing_junk_rejected);
    RUN_TEST(test_set_nvs_failure_rejected);
    RUN_TEST(test_set_timer_nvs_failure_rejected);
    RUN_TEST(test_state_json_escapes_specials);
    RUN_TEST(test_state_json_worst_case_fits_firmware_buffer);
    RUN_TEST(test_state_json_reports_current_values);
    RUN_TEST(test_discovery_number_has_command_bounds_and_config_category);
    RUN_TEST(test_discovery_text_has_mode);
    RUN_TEST(test_discovery_text_advertises_max_length);
    RUN_TEST(test_timer_name_max_matches_the_reject_boundary);
    RUN_TEST(test_discovery_topic);
    RUN_TEST(test_set_bedtime_accepts_evening_and_zero);
    RUN_TEST(test_set_bedtime_rejects_daytime);
    RUN_TEST(test_set_tone_select_valid_option_persists);
    RUN_TEST(test_set_tone_select_rejects_unknown_and_wrong_case);
    RUN_TEST(test_state_json_emits_tone_option_strings);
    RUN_TEST(test_state_json_clamps_out_of_range_tone_index);
    RUN_TEST(test_discovery_select_lists_options);
    RUN_TEST(test_set_alert_volume_valid_persists);
    RUN_TEST(test_set_alert_volume_out_of_range_rejected);
    RUN_TEST(test_alert_volume_discovery_and_state);
    RUN_TEST(test_set_ota_url_https_persists);
    RUN_TEST(test_set_ota_url_rejects_non_https);
    RUN_TEST(test_set_ota_url_empty_disables_ota);
    RUN_TEST(test_set_ota_url_too_long_rejected);
    RUN_TEST(test_set_ota_on_sync_switch_round_trip);
    RUN_TEST(test_set_ota_on_sync_rejects_non_onoff);
    RUN_TEST(test_set_ota_on_sync_nvs_failure_rejected);
    RUN_TEST(test_state_json_includes_ota_fields);
    RUN_TEST(test_state_json_ota_on_sync_nonzero_reads_as_on);
    RUN_TEST(test_discovery_ota_on_sync_is_switch);
    RUN_TEST(test_discovery_ota_url_is_text_with_max);
    RUN_TEST(test_device_hash_is_deterministic);
    RUN_TEST(test_device_hash_changes_when_firmware_version_changes);
    RUN_TEST(test_device_hash_changes_when_name_changes);
    RUN_TEST(test_device_hash_does_not_confuse_the_field_boundary);
    RUN_TEST(test_device_hash_tolerates_null);
    RUN_TEST(test_discovery_stale_on_a_fresh_device);
    RUN_TEST(test_discovery_not_stale_when_both_match);
    RUN_TEST(test_discovery_stale_on_schema_bump_alone);
    RUN_TEST(test_discovery_stale_on_fingerprint_change_alone);
    RUN_TEST(test_discovery_hash_changes_when_a_slot_is_renamed);
    RUN_TEST(test_discovery_hash_changes_when_a_slot_is_enabled_or_cleared);
    RUN_TEST(test_discovery_hash_does_not_confuse_slot_boundaries);
    RUN_TEST(test_discovery_hash_still_tracks_the_device_block);
    RUN_TEST(test_every_string_field_fits_the_set_transport);
    RUN_TEST(test_every_field_key_fits_the_set_transport);
    RUN_TEST(test_discovery_hash_changes_when_only_the_duration_enables_a_slot);
    RUN_TEST(test_discovery_hash_separates_zero_and_nonzero_duration);
    RUN_TEST(test_discovery_hash_ignores_a_duration_change_that_keeps_it_enabled);
    RUN_TEST(test_discovery_hash_tolerates_an_unterminated_slot_name);
    RUN_TEST(test_edit_with_unreadable_blob_keeps_the_other_slots);
    RUN_TEST(test_state_json_falls_back_to_the_installed_table);
    RUN_TEST(test_discovery_hash_falls_back_to_the_installed_table);
    return UNITY_END();
}
