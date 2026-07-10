#include <string.h>
#include <unity.h>

/* Single-TU: the editable-config applier over mock NVS + real accessors
   and validators. No cJSON — values arrive as strings (from MQTT). */
// clang-format off
#include "mock_hal_nvs.c"
#include "../../main/nvs_config.c"
#include "../../main/quiet_hours.c"
#include "../../main/config_validate.c"
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
    }
    char buf[HA_CONFIG_STATE_MAX];
    int ret = ha_config_state_json(buf, sizeof(buf));
    TEST_ASSERT_TRUE(ret < HA_CONFIG_STATE_MAX); /* not truncated */
    TEST_ASSERT_EQUAL_INT((int)strlen(buf), ret);
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

void test_discovery_topic(void) {
    const cfg_field_t *f = field_by_key("weekday_min");
    char buf[128];
    ha_config_discovery_topic(buf, sizeof(buf), "magtag-a1b2c3", f);
    TEST_ASSERT_EQUAL_STRING("homeassistant/number/magtag-a1b2c3_weekday_min/config", buf);
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
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timer2_name\":\"\"")); /* empty slot */
}

void test_discovery_timer_reload_is_switch(void) {
    const cfg_field_t *f = field_by_key("timer1_reload");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_STRING("switch", f->component);
    char buf[700];
    ha_config_discovery(buf, sizeof(buf), "magtag-a1b2c3", "K", "fw", f);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"pl_on\":\"ON\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"cmd_t\":\"magtag/magtag-a1b2c3/set/timer1_reload\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "value_json.timer1_reload"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_set_timer_name_enables_slot);
    RUN_TEST(test_set_timer_name_empty_disables_slot);
    RUN_TEST(test_set_timer_name_too_long_rejected);
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
    RUN_TEST(test_discovery_topic);
    return UNITY_END();
}
