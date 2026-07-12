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
        .remaining_s = {3400, 840, 0, 300, 900},
        .allocation_s = {3600, 900, 0, 600, 900},
        .day_type = "Weekday",
        .completions = {0, 2, 0, 1},
        .charge_lock = false,
        .fw = "v1.4.0-test",
        .reset_reason = "DEEPSLEEP",
    };
}

/* ---- stat payload ---- */

void test_stat_payload_exact(void) {
    char buf[512];
    stats_snapshot_t s = base_snapshot();
    int n = stats_json_stat(buf, sizeof(buf), &s);
    /* remaining_s/allocation_s are PER-SLOT arrays ([0] = Screen, [N] =
       extra timer N) so HA tracks each timer's own history — the old
       active-timer scalars mixed different timers into one series. */
    TEST_ASSERT_EQUAL_STRING(
        "{\"batt_pct\":87,\"batt_mv\":4012,\"light_mv\":420,\"state\":\"RUNNING\","
        "\"active_timer\":\"Screen\",\"remaining_s\":[3400,840,0,300,900],"
        "\"allocation_s\":[3600,900,0,600,900],"
        "\"day_type\":\"Weekday\",\"completions\":[0,2,0,1],\"charge_lock\":false,"
        "\"fw\":\"v1.4.0-test\",\"reset\":\"DEEPSLEEP\"}",
        buf);
    TEST_ASSERT_EQUAL_INT((int)strlen(buf), n);
}

void test_stat_payload_reset_reason_flags_crash_wakes(void) {
    /* Boot forensics over MQTT: the USB CDC console drops output around
       sleep/reset transitions, so the reset reason rides the stat payload
       — a BROWNOUT/PANIC value on a wake means the PREVIOUS wake died. */
    char buf[512];
    stats_snapshot_t s = base_snapshot();
    s.reset_reason = "BROWNOUT";
    stats_json_stat(buf, sizeof(buf), &s);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"reset\":\"BROWNOUT\""));
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
    /* battery, battery_mv, light, state, active_timer, day_type,
       charge_lock, last_reset, screen_remaining, screen_limit
       + per extra slot: completions, remaining, limit */
    TEST_ASSERT_EQUAL_INT(10 + 3 * TIMER_EXTRA_SLOTS, count);
}

void test_discovery_last_reset_diagnostic_sensor(void) {
    int count = 0;
    const ha_entity_t *ents = stats_json_entities(&count);
    const ha_entity_t *reset = NULL;
    for (int i = 0; i < count; i++) {
        if (strcmp(ents[i].key, "last_reset") == 0)
            reset = &ents[i];
    }
    TEST_ASSERT_NOT_NULL(reset);
    TEST_ASSERT_EQUAL_STRING("diagnostic", reset->ent_cat);
    TEST_ASSERT_NOT_NULL(strstr(reset->tpl, "value_json.reset"));
    TEST_ASSERT_EQUAL_INT(0, reset->expire_after); /* evidence must not expire */
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

static const ha_entity_t *find_entity(const char *key) {
    int count = 0;
    const ha_entity_t *ents = stats_json_entities(&count);
    for (int i = 0; i < count; i++) {
        if (strcmp(ents[i].key, key) == 0)
            return &ents[i];
    }
    return NULL;
}

void test_retired_active_scoped_entities_gone(void) {
    /* The active-timer-scoped sensors mixed timers into one HA series;
       screen_used is derivable from limit - remaining. All replaced by
       the per-slot sensors below. */
    TEST_ASSERT_NULL(find_entity("remaining"));
    TEST_ASSERT_NULL(find_entity("allocation"));
    TEST_ASSERT_NULL(find_entity("screen_used"));
}

void test_screen_remaining_and_limit_entities(void) {
    char buf[600];
    const ha_entity_t *rem = find_entity("screen_remaining");
    TEST_ASSERT_NOT_NULL(rem);
    TEST_ASSERT_NOT_NULL(strstr(rem->tpl, "value_json.remaining_s[0]"));
    stats_json_discovery(buf, sizeof(buf), "magtag-a1b2c3", "Kitchen", "fw", rem);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"name\":\"Screen time remaining\""));
    TEST_ASSERT_NULL(strstr(buf, "ent_cat")); /* primary, like old Time remaining */

    const ha_entity_t *lim = find_entity("screen_limit");
    TEST_ASSERT_NOT_NULL(lim);
    TEST_ASSERT_NOT_NULL(strstr(lim->tpl, "value_json.allocation_s[0]"));
    TEST_ASSERT_EQUAL_STRING("Screen time limit", lim->name);
    TEST_ASSERT_EQUAL_STRING("diagnostic", lim->ent_cat);
}

void test_per_slot_remaining_and_limit_entities(void) {
    char buf[600];
    const ha_entity_t *rem = find_entity("remaining_1");
    TEST_ASSERT_NOT_NULL(rem);
    TEST_ASSERT_NOT_NULL(strstr(rem->tpl, "value_json.remaining_s[1]"));
    /* Runtime slot name overrides the table default, like completions_N */
    stats_json_discovery_named(buf, sizeof(buf), "magtag-a1b2c3", "Kitchen", "fw", rem, "Piano remaining");
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"name\":\"Piano remaining\""));

    const ha_entity_t *lim = find_entity("limit_4");
    TEST_ASSERT_NOT_NULL(lim);
    TEST_ASSERT_NOT_NULL(strstr(lim->tpl, "value_json.allocation_s[4]"));
    TEST_ASSERT_EQUAL_STRING("diagnostic", lim->ent_cat);
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

void test_discovery_diagnostic_category(void) {
    char buf[600];
    int count = 0;
    const ha_entity_t *ents = stats_json_entities(&count);
    const ha_entity_t *e = NULL;
    for (int i = 0; i < count; i++)
        if (strcmp(ents[i].key, "battery_mv") == 0)
            e = &ents[i];
    TEST_ASSERT_NOT_NULL(e);
    stats_json_discovery(buf, sizeof(buf), "magtag-a1b2c3", "Kitchen", "fw", e);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ent_cat\":\"diagnostic\""));
}

void test_primary_entity_omits_category(void) {
    char buf[600];
    int count = 0;
    const ha_entity_t *ents = stats_json_entities(&count); /* [0] = battery, primary */
    stats_json_discovery(buf, sizeof(buf), "magtag-a1b2c3", "Kitchen", "fw", &ents[0]);
    TEST_ASSERT_NULL(strstr(buf, "ent_cat"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_discovery_diagnostic_category);
    RUN_TEST(test_primary_entity_omits_category);
    RUN_TEST(test_retired_active_scoped_entities_gone);
    RUN_TEST(test_screen_remaining_and_limit_entities);
    RUN_TEST(test_per_slot_remaining_and_limit_entities);
    RUN_TEST(test_stat_payload_exact);
    RUN_TEST(test_stat_payload_charge_lock_true);
    RUN_TEST(test_stat_payload_reset_reason_flags_crash_wakes);
    RUN_TEST(test_discovery_last_reset_diagnostic_sensor);
    RUN_TEST(test_stat_payload_escapes_timer_name);
    RUN_TEST(test_stat_payload_reports_needed_length_when_truncated);
    RUN_TEST(test_stat_payload_null_string_fields_are_safe);
    RUN_TEST(test_summary_payload_exact);
    RUN_TEST(test_discovery_entity_table_is_populated);
    RUN_TEST(test_discovery_topic);
    RUN_TEST(test_discovery_battery_payload);
    RUN_TEST(test_discovery_binary_sensor_has_payload_states);
    RUN_TEST(test_discovery_completions_use_runtime_slot_names);
    return UNITY_END();
}
