#include <stdio.h>
#include <string.h>
#include <unity.h>

/* Single-TU: the editable-config applier over mock NVS + real accessors
   and validators. No cJSON — values arrive as strings (from MQTT). */
// clang-format off
#include "mock_hal_nvs.c"
#include "../../main/nvs_config.c"
#include "../../main/quiet_hours.c"
#include "../../main/bedtime.c"
#include "../../main/config_validate.c"
#include "../../main/tones.c"
#include "../../main/ha_config.c"
// clang-format on

static void seed_blob(void); /* defined with the Phase B tests below */

void setUp(void) {
    mock_nvs_reset();
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

void test_state_json_worst_case_fits_firmware_buffer(void) {
    char ack[128];
    /* max out every field so the JSON approaches its ceiling */
    ha_config_set("weekday_min", "1440", ack, sizeof(ack));
    ha_config_set("weekend_min", "1440", ack, sizeof(ack));
    ha_config_set("holiday_min", "1440", ack, sizeof(ack));
    ha_config_set("summer_min", "1440", ack, sizeof(ack));
    ha_config_set("break_interval_min", "480", ack, sizeof(ack));
    ha_config_set("break_duration_min", "120", ack, sizeof(ack));
    ha_config_set("quiet_start", "2359", ack, sizeof(ack));
    ha_config_set("quiet_end", "2359", ack, sizeof(ack));
    ha_config_set("name", "Kitchen Countertop MagTag Timerr", ack, sizeof(ack)); /* 31 chars */
    ha_config_set("tz", "America/Argentina/ComodRivadavia-3EDT", ack, sizeof(ack));
    for (int n = 1; n <= 4; n++) {
        char key[16];
        snprintf(key, sizeof(key), "timer%d_name", n);
        ha_config_set(key, "LongTimerName15", ack, sizeof(ack)); /* 15 chars */
        snprintf(key, sizeof(key), "timer%d_min", n);
        ha_config_set(key, "1440", ack, sizeof(ack));
        snprintf(key, sizeof(key), "timer%d_reload", n);
        ha_config_set(key, "ON", ack, sizeof(ack));
        snprintf(key, sizeof(key), "timer%d_break", n);
        ha_config_set(key, "ON", ack, sizeof(ack));
    }
    /* The OTA endpoint is the longest CFG_STR in the registry, so the
       worst case has to include a maxed-out one. */
    char url[CFG_BOUND_OTA_URL_MAX];
    memset(url, 'u', sizeof(url) - 1);
    memcpy(url, "https://", 8);
    url[sizeof(url) - 1] = '\0';
    ha_config_set("ota_url", url, ack, sizeof(ack));
    ha_config_set("ota_on_sync", "ON", ack, sizeof(ack));
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
    return UNITY_END();
}
