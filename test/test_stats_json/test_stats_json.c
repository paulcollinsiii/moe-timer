#include <string.h>
#include <unity.h>

/* Single-TU compilation */
#include "../../main/stats_json.c"

void setUp(void) {}
void tearDown(void) {}

static stats_snapshot_t base_snapshot(void) {
    return (stats_snapshot_t){
        .batt_pct = 87,
        .batt_mv = 4012,
        .light_mv = 420,
        .state = "RUNNING",
        .active_timer = "Screen",
        .remaining_s = 3400,
        .allocation_s = 3600,
        .day_type = "Weekday",
        .completions = {0, 2, 0, 1},
        .charge_lock = false,
        .fw = "v1.4.0-test",
    };
}

/* ---- stat payload ---- */

void test_stat_payload_exact(void) {
    char buf[512];
    stats_snapshot_t s = base_snapshot();
    int n = stats_json_stat(buf, sizeof(buf), &s);
    TEST_ASSERT_EQUAL_STRING(
        "{\"batt_pct\":87,\"batt_mv\":4012,\"light_mv\":420,\"state\":\"RUNNING\","
        "\"active_timer\":\"Screen\",\"remaining_s\":3400,\"allocation_s\":3600,"
        "\"day_type\":\"Weekday\",\"completions\":[0,2,0,1],\"charge_lock\":false,"
        "\"fw\":\"v1.4.0-test\"}",
        buf);
    TEST_ASSERT_EQUAL_INT((int)strlen(buf), n);
}

void test_stat_payload_charge_lock_true(void) {
    char buf[512];
    stats_snapshot_t s = base_snapshot();
    s.charge_lock = true;
    stats_json_stat(buf, sizeof(buf), &s);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"charge_lock\":true"));
}

void test_stat_payload_escapes_timer_name(void) {
    char buf[512];
    stats_snapshot_t s = base_snapshot();
    s.active_timer = "Say \"Om\"\\now"; /* quotes + backslash must escape */
    stats_json_stat(buf, sizeof(buf), &s);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"active_timer\":\"Say \\\"Om\\\"\\\\now\""));
}

void test_stat_payload_reports_needed_length_when_truncated(void) {
    char buf[32];
    stats_snapshot_t s = base_snapshot();
    int n = stats_json_stat(buf, sizeof(buf), &s);
    TEST_ASSERT_GREATER_THAN_INT((int)sizeof(buf), n);  /* snprintf semantics */
    TEST_ASSERT_EQUAL_CHAR('\0', buf[sizeof(buf) - 1]); /* still terminated */
}

void test_stat_payload_null_string_fields_are_safe(void) {
    /* A NULL state/day_type/fw/active_timer must not crash the builder
       (defensive: main.c always populates them, but the payload builder
       is the pure boundary and should never invoke UB on bad input). */
    char buf[512];
    stats_snapshot_t s = base_snapshot();
    s.state = NULL;
    s.active_timer = NULL;
    s.day_type = NULL;
    s.fw = NULL;
    int n = stats_json_stat(buf, sizeof(buf), &s);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    /* NULL renders as empty strings, JSON stays well-formed */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"state\":\"\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"active_timer\":\"\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"day_type\":\"\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"fw\":\"\""));
}

/* ---- daily summary ---- */

void test_summary_payload_exact(void) {
    char buf[256];
    const uint16_t comp[TIMER_EXTRA_SLOTS] = {1, 2, 0, 0};
    int n = stats_json_summary(buf, sizeof(buf), "2026-07-08", 3200, comp);
    TEST_ASSERT_EQUAL_STRING("{\"date\":\"2026-07-08\",\"screen_used_s\":3200,\"completions\":[1,2,0,0]}", buf);
    TEST_ASSERT_EQUAL_INT((int)strlen(buf), n);
}

/* ---- HA discovery ---- */

void test_discovery_entity_table_is_populated(void) {
    int count = 0;
    const ha_entity_t *ents = stats_json_entities(&count);
    TEST_ASSERT_NOT_NULL(ents);
    /* battery, battery_mv, light, state, active_timer, remaining,
       allocation, day_type, charge_lock, screen_used + 4 completions */
    TEST_ASSERT_EQUAL_INT(10 + TIMER_EXTRA_SLOTS, count);
}

void test_discovery_topic(void) {
    char buf[128];
    int count = 0;
    const ha_entity_t *ents = stats_json_entities(&count);
    stats_json_discovery_topic(buf, sizeof(buf), "magtag-a1b2c3", &ents[0]);
    TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/magtag-a1b2c3_battery/config", buf);
}

void test_discovery_battery_payload(void) {
    char buf[600];
    int count = 0;
    const ha_entity_t *ents = stats_json_entities(&count); /* [0] = battery */
    int n = stats_json_discovery(buf, sizeof(buf), "magtag-a1b2c3", "Kitchen MagTag", "v1.4.0-test", &ents[0]);
    TEST_ASSERT_EQUAL_STRING(
        "{\"name\":\"Battery\",\"uniq_id\":\"magtag-a1b2c3_battery\","
        "\"stat_t\":\"magtag/magtag-a1b2c3/stat\",\"val_tpl\":\"{{ value_json.batt_pct }}\","
        "\"unit_of_meas\":\"%\",\"dev_cla\":\"battery\",\"expire_after\":7500,"
        "\"dev\":{\"ids\":[\"magtag-a1b2c3\"],\"name\":\"Kitchen MagTag\",\"mf\":\"Adafruit\","
        "\"mdl\":\"MagTag 2.9\",\"sw\":\"v1.4.0-test\"}}",
        buf);
    TEST_ASSERT_EQUAL_INT((int)strlen(buf), n);
}

void test_discovery_binary_sensor_has_payload_states(void) {
    char buf[600];
    int count = 0;
    const ha_entity_t *ents = stats_json_entities(&count);
    const ha_entity_t *lock = NULL;
    for (int i = 0; i < count; i++) {
        if (strcmp(ents[i].key, "charge_lock") == 0)
            lock = &ents[i];
    }
    TEST_ASSERT_NOT_NULL(lock);
    TEST_ASSERT_EQUAL_STRING("binary_sensor", lock->component);
    stats_json_discovery(buf, sizeof(buf), "magtag-a1b2c3", "Kitchen MagTag", "fw", lock);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"pl_on\":\"ON\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"pl_off\":\"OFF\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "value_json.charge_lock"));
}

void test_discovery_summary_sensor_uses_summary_topic_no_expire(void) {
    char buf[600];
    int count = 0;
    const ha_entity_t *ents = stats_json_entities(&count);
    const ha_entity_t *used = NULL;
    for (int i = 0; i < count; i++) {
        if (strcmp(ents[i].key, "screen_used") == 0)
            used = &ents[i];
    }
    TEST_ASSERT_NOT_NULL(used);
    stats_json_discovery(buf, sizeof(buf), "magtag-a1b2c3", "Kitchen MagTag", "fw", used);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"stat_t\":\"magtag/magtag-a1b2c3/summary\""));
    TEST_ASSERT_NULL(strstr(buf, "expire_after")); /* daily value must persist */
}

void test_discovery_completions_use_runtime_slot_names(void) {
    char buf[600];
    int count = 0;
    const ha_entity_t *ents = stats_json_entities(&count);
    const ha_entity_t *c1 = NULL;
    for (int i = 0; i < count; i++) {
        if (strcmp(ents[i].key, "completions_1") == 0)
            c1 = &ents[i];
    }
    TEST_ASSERT_NOT_NULL(c1);
    /* Runtime slot name overrides the table name via the override arg */
    stats_json_discovery_named(buf, sizeof(buf), "magtag-a1b2c3", "Kitchen MagTag", "fw", c1, "Piano runs");
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"name\":\"Piano runs\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "value_json.completions[0]"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_stat_payload_exact);
    RUN_TEST(test_stat_payload_charge_lock_true);
    RUN_TEST(test_stat_payload_escapes_timer_name);
    RUN_TEST(test_stat_payload_reports_needed_length_when_truncated);
    RUN_TEST(test_stat_payload_null_string_fields_are_safe);
    RUN_TEST(test_summary_payload_exact);
    RUN_TEST(test_discovery_entity_table_is_populated);
    RUN_TEST(test_discovery_topic);
    RUN_TEST(test_discovery_battery_payload);
    RUN_TEST(test_discovery_binary_sensor_has_payload_states);
    RUN_TEST(test_discovery_summary_sensor_uses_summary_topic_no_expire);
    RUN_TEST(test_discovery_completions_use_runtime_slot_names);
    return UNITY_END();
}
