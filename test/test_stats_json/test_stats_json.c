#include <stdio.h>
#include <string.h>
#include <unity.h>

/* Single-TU compilation */
#include "../../main/stats_json.c"

void setUp(void) {}
void tearDown(void) {}

/* What a device that has never attempted an update reports. Named rather
   than passed as NULL so the ordinary cases exercise the same path the
   firmware takes; the NULL tolerance gets its own case below. */
static const ota_stat_t NO_OTA;

/* Likewise for the diagnostics leg: a device that has never panicked and
   whose live readings are all zero. Zeroed rather than NULL so the
   ordinary cases walk the same code the firmware does — the NULL
   tolerance has its own case. */
static const diag_stat_t NO_DIAG;

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
        .break_remaining_s = 0,
    };
}

/* ---- stat payload ---- */

void test_stat_payload_exact(void) {
    char buf[512];
    stats_snapshot_t s = base_snapshot();
    int n = stats_json_stat(buf, sizeof(buf), &s, &NO_OTA, &NO_DIAG);
    /* remaining_s/allocation_s are PER-SLOT arrays ([0] = Screen, [N] =
       extra timer N) so HA tracks each timer's own history — the old
       active-timer scalars mixed different timers into one series. */
    TEST_ASSERT_EQUAL_STRING(
        "{\"batt_pct\":87,\"batt_mv\":4012,\"light_mv\":420,\"state\":\"RUNNING\","
        "\"active_timer\":\"Screen\",\"remaining_s\":[3400,840,0,300,900],"
        "\"allocation_s\":[3600,900,0,600,900],"
        "\"day_type\":\"Weekday\",\"completions\":[0,2,0,1],\"charge_lock\":false,"
        "\"break_s\":0,\"accum_s\":0,\"fw\":\"v1.4.0-test\",\"reset\":\"DEEPSLEEP\","
        "\"ota_result\":\"\",\"ota_target\":\"\",\"ota_fails\":0,\"ota_dl_ms\":0,"
        "\"panics\":0,\"pphase\":\"\",\"pup_s\":0,\"pheap\":0,\"pstk_main\":0,\"pstk_net\":0,"
        "\"heap\":0,\"heap_min\":0,\"stk_main\":0,\"stk_net\":0,\"nvs_free\":0}",
        buf);
    TEST_ASSERT_EQUAL_INT((int)strlen(buf), n);
}

void test_stat_payload_reports_a_running_break(void) {
    /* A Screen Break can run behind any selected timer, so HA cannot
       infer it from "state" any more — it needs its own field. */
    char buf[512];
    stats_snapshot_t s = base_snapshot();
    s.break_remaining_s = 754;
    stats_json_stat(buf, sizeof(buf), &s, &NO_OTA, &NO_DIAG);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"break_s\":754"));
}

/* The exposure balance is the only visibility HA gets into why a break
   did or did not fire — against the known interval it answers the
   question directly. Always >= 0: app_state feeds it the clamped read. */
void test_stat_payload_reports_the_exposure_balance(void) {
    char buf[512];
    stats_snapshot_t s = base_snapshot();
    s.accum_s = 1500;
    stats_json_stat(buf, sizeof(buf), &s, &NO_OTA, &NO_DIAG);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"accum_s\":1500"));
}

void test_stat_payload_reset_reason_flags_crash_wakes(void) {
    /* Boot forensics over MQTT: the USB CDC console drops output around
       sleep/reset transitions, so the reset reason rides the stat payload
       — a BROWNOUT/PANIC value on a wake means the PREVIOUS wake died. */
    char buf[512];
    stats_snapshot_t s = base_snapshot();
    s.reset_reason = "BROWNOUT";
    stats_json_stat(buf, sizeof(buf), &s, &NO_OTA, &NO_DIAG);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"reset\":\"BROWNOUT\""));
}

void test_stat_payload_charge_lock_true(void) {
    char buf[512];
    stats_snapshot_t s = base_snapshot();
    s.charge_lock = true;
    stats_json_stat(buf, sizeof(buf), &s, &NO_OTA, &NO_DIAG);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"charge_lock\":true"));
}

void test_stat_payload_escapes_timer_name(void) {
    char buf[512];
    stats_snapshot_t s = base_snapshot();
    s.active_timer = "Say \"Om\"\\now"; /* quotes + backslash must escape */
    stats_json_stat(buf, sizeof(buf), &s, &NO_OTA, &NO_DIAG);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"active_timer\":\"Say \\\"Om\\\"\\\\now\""));
}

void test_stat_payload_reports_needed_length_when_truncated(void) {
    char buf[32];
    stats_snapshot_t s = base_snapshot();
    int n = stats_json_stat(buf, sizeof(buf), &s, &NO_OTA, &NO_DIAG);
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
    int n = stats_json_stat(buf, sizeof(buf), &s, &NO_OTA, &NO_DIAG);
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
       charge_lock, last_reset, screen_remaining, screen_limit,
       screen_break, break_remaining, screen_exposure,
       ota_result, ota_target, ota_fails, ota_dl_ms,
       panic_count, panic_phase, panic_uptime, panic_heap,
       panic_stack_main, panic_stack_net,
       heap_free, heap_min, stack_main, stack_net, nvs_free
       + per extra slot: completions, remaining, limit */
    TEST_ASSERT_EQUAL_INT(28 + 3 * TIMER_EXTRA_SLOTS, count);
}

/* THE BUMP, pinned to the table it describes.

   Home Assistant does not re-read a retained discovery config it has
   already seen, and mqtt_ha.c only republishes them when this number
   moves. A new row in ENTITIES with the version left alone therefore
   produces a device that publishes a field no entity is subscribed to --
   nothing appears, nothing errors, and the only way to find out is to
   flash it and go looking. Asserting the two numbers TOGETHER turns that
   into a failing host test: adding an entity fails the count above, and
   fixing the count without touching the version fails this. */
void test_discovery_schema_version_moves_with_the_entity_table(void) {
    int count = 0;
    (void)stats_json_entities(&count);
    TEST_ASSERT_EQUAL_INT(28 + 3 * TIMER_EXTRA_SLOTS, count);
    TEST_ASSERT_EQUAL_INT(20, STATS_JSON_DISC_SCHEMA_VER);
}

/* ---- the OTA leg of the stat payload ---- */

/* The four fields come from the ota_stat_t argument, which mqtt_ha.c
   fills from NVS at publish time -- NOT from stats_snapshot_t, which was
   frozen on the main task before this wake's check ran. The snapshot has
   no OTA fields at all, so the mistake cannot be made silently; this
   pins the values it CAN carry. */
void test_stat_payload_carries_the_ota_fields(void) {
    char buf[512];
    stats_snapshot_t s = base_snapshot();
    ota_stat_t ota = {.fails = 2, .dl_ms = 41250};
    snprintf(ota.result, sizeof(ota.result), "%s", "rolled_back");
    snprintf(ota.target, sizeof(ota.target), "%s", "1.6.0");
    stats_json_stat(buf, sizeof(buf), &s, &ota, &NO_DIAG);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ota_result\":\"rolled_back\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ota_target\":\"1.6.0\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ota_fails\":2"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ota_dl_ms\":41250"));
}

/* Same snapshot, different OTA argument, different payload: the fields
   track the argument and nothing else. A builder that ignored `ota` and
   emitted constants would pass the case above and fail this one. */
void test_stat_payload_ota_fields_track_the_argument(void) {
    char a[512], b[512];
    stats_snapshot_t s = base_snapshot();
    ota_stat_t first = {.fails = 1, .dl_ms = 1000};
    ota_stat_t second = {.fails = 3, .dl_ms = 2000};
    snprintf(first.result, sizeof(first.result), "%s", "timeout");
    snprintf(second.result, sizeof(second.result), "%s", "gave_up");
    stats_json_stat(a, sizeof(a), &s, &first, &NO_DIAG);
    stats_json_stat(b, sizeof(b), &s, &second, &NO_DIAG);
    TEST_ASSERT_NOT_NULL(
        strstr(a, "\"ota_result\":\"timeout\",\"ota_target\":\"\",\"ota_fails\":1,\"ota_dl_ms\":1000"));
    TEST_ASSERT_NOT_NULL(
        strstr(b, "\"ota_result\":\"gave_up\",\"ota_target\":\"\",\"ota_fails\":3,\"ota_dl_ms\":2000"));
}

/* A duration is a u32 because a u16 saturates at 65.5 s, well under the
   download's own deadline. The payload must carry the full range. */
void test_stat_payload_ota_duration_survives_a_long_download(void) {
    char buf[512];
    stats_snapshot_t s = base_snapshot();
    ota_stat_t ota = {.dl_ms = 298000}; /* just inside a 300 s budget */
    stats_json_stat(buf, sizeof(buf), &s, &ota, &NO_DIAG);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ota_dl_ms\":298000"));
}

/* The pure boundary must not fault on a NULL OTA leg either. */
void test_stat_payload_null_ota_is_safe(void) {
    char buf[512];
    stats_snapshot_t s = base_snapshot();
    int n = stats_json_stat(buf, sizeof(buf), &s, NULL, NULL);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ota_result\":\"\",\"ota_target\":\"\",\"ota_fails\":0,\"ota_dl_ms\":0"));
    /* And the diagnostics leg. An empty pphase is the "no breadcrumb on
       file" reading, which is exactly what a NULL leg means. */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"panics\":0,\"pphase\":\"\",\"pup_s\":0,\"pheap\":0"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"nvs_free\":0}"));
}

/* Reason codes and version strings come off flash, and NVS bytes are not
   trustworthy input: a quote or a backslash in either would otherwise
   close the JSON string early and hand Home Assistant a payload it drops
   silently. */
void test_stat_payload_escapes_the_ota_strings(void) {
    char buf[512];
    stats_snapshot_t s = base_snapshot();
    ota_stat_t ota = {0};
    snprintf(ota.result, sizeof(ota.result), "%s", "a\"b");
    snprintf(ota.target, sizeof(ota.target), "%s", "c\\d");
    stats_json_stat(buf, sizeof(buf), &s, &ota, &NO_DIAG);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ota_result\":\"a\\\"b\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ota_target\":\"c\\\\d\""));
}

/* The stat payload is built into a fixed buffer and publish_states DROPS
   any build that reaches its size -- silently, with no log line and no
   retry. Every string field at its stored width, every escape doubling
   it, every counter at its maximum.

   The diagnostics leg added eleven fields and took the worst case past
   the old 768-byte buffer, which is why STATS_JSON_PAYLOAD_MAX moved to
   1024 rather than this assertion being relaxed: a payload that does not
   fit is not published AT ALL, so on a device that is panicking the
   evidence would be dropped precisely when it exists. pphase is filled
   with backslashes even though the label builder can only emit letters
   and '+' -- the buffer has to survive the field's declared width, not
   the current producer's habits. */
void test_stat_payload_worst_case_fits_the_publish_buffer(void) {
    char buf[STATS_JSON_PAYLOAD_MAX];
    stats_snapshot_t s = base_snapshot();
    char longname[64], longday[24], longfw[32], longrst[24];
    memset(longname, '\\', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';
    memset(longday, '\\', sizeof(longday) - 1);
    longday[sizeof(longday) - 1] = '\0';
    memset(longfw, '\\', sizeof(longfw) - 1);
    longfw[sizeof(longfw) - 1] = '\0';
    memset(longrst, '\\', sizeof(longrst) - 1);
    longrst[sizeof(longrst) - 1] = '\0';
    s.state = longday;
    s.active_timer = longname;
    s.day_type = longday;
    s.fw = longfw;
    s.reset_reason = longrst;
    s.batt_pct = -999;
    s.batt_mv = -99999;
    s.light_mv = -99999;
    for (int i = 0; i < TIMER_SLOT_COUNT; i++) {
        s.remaining_s[i] = -2147483647;
        s.allocation_s[i] = 4294967295u;
    }
    for (int i = 0; i < TIMER_EXTRA_SLOTS; i++)
        s.completions[i] = 65535;
    s.break_remaining_s = -2147483647;
    s.accum_s = -2147483647;

    ota_stat_t ota = {.fails = 65535, .dl_ms = 4294967295u};
    memset(ota.result, '\\', sizeof(ota.result) - 1);
    memset(ota.target, '\\', sizeof(ota.target) - 1);
    ota.result[sizeof(ota.result) - 1] = '\0';
    ota.target[sizeof(ota.target) - 1] = '\0';

    diag_stat_t diag = {
        .panics = 4294967295u,
        .panic_uptime_s = 4294967295u,
        .panic_heap = 4294967295u,
        .panic_stack_main = 65535,
        .panic_stack_net = 65535,
        .heap_free = 4294967295u,
        .heap_min = 4294967295u,
        .stack_main = 65535,
        .stack_net = 65535,
        .nvs_free = 65535,
    };
    memset(diag.panic_phase, '\\', sizeof(diag.panic_phase) - 1);
    diag.panic_phase[sizeof(diag.panic_phase) - 1] = '\0';

    int n = stats_json_stat(buf, sizeof(buf), &s, &ota, &diag);
    TEST_ASSERT_LESS_THAN_INT((int)sizeof(buf), n);
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

/* expire_after 0 on all four, and it is a decision. These are the record
   of the last update attempt, not live telemetry: an expiry blanks them
   on exactly the device this task exists to make legible -- one that
   tried to update, rolled back, and is now failing to check in. */
void test_ota_entities_never_expire(void) {
    int count = 0;
    const ha_entity_t *ents = stats_json_entities(&count);
    const char *keys[] = {"ota_result", "ota_target", "ota_fails", "ota_dl_ms"};
    for (unsigned k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        const ha_entity_t *e = NULL;
        for (int i = 0; i < count; i++) {
            if (strcmp(ents[i].key, keys[k]) == 0)
                e = &ents[i];
        }
        TEST_ASSERT_NOT_NULL(e);
        TEST_ASSERT_EQUAL_INT(0, e->expire_after);
        TEST_ASSERT_EQUAL_STRING("stat", e->topic_suffix);
        TEST_ASSERT_FALSE(e->binary);
    }
}

/* "Did my update work?" is an operator question, not a diagnostic one, so
   ota_result sits at the top level of the device card and the three
   supporting fields do not. */
void test_ota_result_is_the_primary_entity_of_the_four(void) {
    int count = 0;
    const ha_entity_t *ents = stats_json_entities(&count);
    for (int i = 0; i < count; i++) {
        if (strcmp(ents[i].key, "ota_result") == 0)
            TEST_ASSERT_NULL(ents[i].ent_cat);
        if (strcmp(ents[i].key, "ota_target") == 0 || strcmp(ents[i].key, "ota_fails") == 0 ||
            strcmp(ents[i].key, "ota_dl_ms") == 0)
            TEST_ASSERT_EQUAL_STRING("diagnostic", ents[i].ent_cat);
    }
}

/* ---- the diagnostics leg ---- */

/* The values come from the diag_stat_t argument, which mqtt_ha.c fills
   on the NETWORK task at publish time -- never from stats_snapshot_t,
   which was frozen on the main task before the radio came up and so
   cannot show TLS-time heap or the network task's stack floor at all. */
void test_stat_payload_carries_the_panic_fields(void) {
    char buf[STATS_JSON_PAYLOAD_MAX];
    stats_snapshot_t s = base_snapshot();
    diag_stat_t d = {
        .panics = 7,
        .panic_uptime_s = 41,
        .panic_heap = 21000,
        .panic_stack_main = 3500,
        .panic_stack_net = 1200,
        .heap_free = 88000,
        .heap_min = 60000,
        .stack_main = 4000,
        .stack_net = 2100,
        .nvs_free = 190,
    };
    snprintf(d.panic_phase, sizeof(d.panic_phase), "%s", "RENDER+OTA_CHECK");
    stats_json_stat(buf, sizeof(buf), &s, &NO_OTA, &d);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"panics\":7,\"pphase\":\"RENDER+OTA_CHECK\",\"pup_s\":41,\"pheap\":21000"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"pstk_main\":3500,\"pstk_net\":1200"));
    TEST_ASSERT_NOT_NULL(
        strstr(buf, "\"heap\":88000,\"heap_min\":60000,\"stk_main\":4000,\"stk_net\":2100,\"nvs_free\":190}"));
}

/* The phase label is produced by panic_diag.c from a fixed literal map,
   so today it cannot contain a quote. The escape still has to be there:
   a field that is safe only because of what another module currently
   does stops being safe the day that module changes, and an unescaped
   quote here silently costs Home Assistant the WHOLE payload, not just
   this field. */
void test_stat_payload_escapes_the_panic_phase(void) {
    char buf[STATS_JSON_PAYLOAD_MAX];
    stats_snapshot_t s = base_snapshot();
    diag_stat_t d = {0};
    snprintf(d.panic_phase, sizeof(d.panic_phase), "%s", "a\"b\\c");
    stats_json_stat(buf, sizeof(buf), &s, &NO_OTA, &d);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"pphase\":\"a\\\"b\\\\c\""));
}

/* Same call for the same reason as the OTA leg's: the counter is what
   makes the panic RATE knowable, and a device that has never panicked
   must be distinguishable from one whose breadcrumb was lost. Zero plus
   an empty phase is that reading. */
void test_stat_payload_null_diag_is_safe(void) {
    char buf[STATS_JSON_PAYLOAD_MAX];
    stats_snapshot_t s = base_snapshot();
    int n = stats_json_stat(buf, sizeof(buf), &s, &NO_OTA, NULL);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"panics\":0,\"pphase\":\"\""));
}

/* The four panic entities are the RECORD of an event, not telemetry:
   they change only when a panic happens, and an expiry would blank them
   on exactly the device this exists for -- one that panicked and then
   went quiet. Same precedent as last_reset and the OTA four. */
void test_panic_entities_never_expire(void) {
    int count = 0;
    const ha_entity_t *ents = stats_json_entities(&count);
    const char *keys[] = {"panic_count", "panic_phase",      "panic_uptime",
                          "panic_heap",  "panic_stack_main", "panic_stack_net"};
    for (unsigned k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        const ha_entity_t *e = NULL;
        for (int i = 0; i < count; i++) {
            if (strcmp(ents[i].key, keys[k]) == 0)
                e = &ents[i];
        }
        TEST_ASSERT_NOT_NULL_MESSAGE(e, "a panic entity is missing from the table");
        TEST_ASSERT_EQUAL_INT(0, e->expire_after);
        TEST_ASSERT_EQUAL_STRING("stat", e->topic_suffix);
    }
}

/* The live health readings ARE telemetry, so they expire: a device that
   has stopped checking in must read unavailable rather than show
   yesterday's heap as if it were current. The opposite decision to the
   panic group above, in the same table, which is why both are pinned. */
void test_live_health_entities_expire(void) {
    int count = 0;
    const ha_entity_t *ents = stats_json_entities(&count);
    const char *keys[] = {"heap_free", "heap_min", "stack_main", "stack_net", "nvs_free"};
    for (unsigned k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        const ha_entity_t *e = NULL;
        for (int i = 0; i < count; i++) {
            if (strcmp(ents[i].key, keys[k]) == 0)
                e = &ents[i];
        }
        TEST_ASSERT_NOT_NULL_MESSAGE(e, "a live health entity is missing from the table");
        TEST_ASSERT_GREATER_THAN_INT(0, e->expire_after);
        TEST_ASSERT_EQUAL_STRING("diagnostic", e->ent_cat);
    }
}

/* "Is it still crashing, and how often?" is an operator question, not a
   diagnostic one, so the count sits at the top level of the device card
   and every supporting number does not. */
void test_panic_count_is_the_primary_entity_of_the_group(void) {
    int count = 0;
    const ha_entity_t *ents = stats_json_entities(&count);
    for (int i = 0; i < count; i++) {
        if (strcmp(ents[i].key, "panic_count") == 0)
            TEST_ASSERT_NULL(ents[i].ent_cat);
        if (strcmp(ents[i].key, "panic_phase") == 0 || strcmp(ents[i].key, "panic_uptime") == 0 ||
            strcmp(ents[i].key, "panic_heap") == 0)
            TEST_ASSERT_EQUAL_STRING("diagnostic", ents[i].ent_cat);
    }
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
        "{\"name\":\"Battery\",\"uniq_id\":\"magtag-a1b2c3_battery\",\"obj_id\":\"magtag-a1b2c3_battery\","
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

void test_discovery_screen_break_entities(void) {
    char buf[600];
    /* Primary binary sensor: "is a break on right now" is a top-level
       fact about the device, not a diagnostic. */
    const ha_entity_t *brk = find_entity("screen_break");
    TEST_ASSERT_NOT_NULL(brk);
    TEST_ASSERT_EQUAL_STRING("binary_sensor", brk->component);
    TEST_ASSERT_EQUAL_STRING("Screen break", brk->name);
    TEST_ASSERT_NULL(brk->ent_cat);
    TEST_ASSERT_NOT_NULL(strstr(brk->tpl, "value_json.break_s"));
    stats_json_discovery(buf, sizeof(buf), "magtag-a1b2c3", "Kitchen", "fw", brk);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"pl_on\":\"ON\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"pl_off\":\"OFF\""));

    /* Diagnostic countdown alongside it */
    const ha_entity_t *rem = find_entity("break_remaining");
    TEST_ASSERT_NOT_NULL(rem);
    TEST_ASSERT_EQUAL_STRING("sensor", rem->component);
    TEST_ASSERT_EQUAL_STRING("Screen break remaining", rem->name);
    TEST_ASSERT_EQUAL_STRING("min", rem->unit);
    TEST_ASSERT_EQUAL_STRING("duration", rem->dev_class);
    TEST_ASSERT_EQUAL_STRING("diagnostic", rem->ent_cat);
    TEST_ASSERT_NOT_NULL(strstr(rem->tpl, "value_json.break_s"));
}

void test_screen_exposure_entity(void) {
    const ha_entity_t *exp = find_entity("screen_exposure");
    TEST_ASSERT_NOT_NULL(exp);
    TEST_ASSERT_EQUAL_STRING("sensor", exp->component);
    TEST_ASSERT_EQUAL_STRING("Screen exposure", exp->name);
    TEST_ASSERT_EQUAL_STRING("min", exp->unit);
    TEST_ASSERT_EQUAL_STRING("duration", exp->dev_class);
    TEST_ASSERT_EQUAL_STRING("diagnostic", exp->ent_cat);
    TEST_ASSERT_NOT_NULL(strstr(exp->tpl, "value_json.accum_s"));
    /* mqtt_ha.c attaches runtime slot names by matching these three
       prefixes; screen_exposure must stay clear of all of them or it
       would be renamed after a timer that has nothing to do with it. */
    TEST_ASSERT_NOT_EQUAL(0, strncmp(exp->key, "remaining_", 10));
    TEST_ASSERT_NOT_EQUAL(0, strncmp(exp->key, "limit_", 6));
    TEST_ASSERT_NOT_EQUAL(0, strncmp(exp->key, "completions_", 12));
}

void test_break_entities_are_not_mistaken_for_per_slot_sensors(void) {
    /* mqtt_ha.c matches per-slot keys by prefix ("remaining_", "limit_",
       "completions_") to attach the runtime timer name. "break_remaining"
       must not collide with that, or discovery would look up a slot and
       skip the entity entirely. */
    const ha_entity_t *rem = find_entity("break_remaining");
    TEST_ASSERT_NOT_NULL(rem);
    TEST_ASSERT_TRUE(strncmp(rem->key, "remaining_", 10) != 0);
    TEST_ASSERT_TRUE(strncmp(rem->key, "limit_", 6) != 0);
    TEST_ASSERT_TRUE(strncmp(rem->key, "completions_", 12) != 0);
    const ha_entity_t *brk = find_entity("screen_break");
    TEST_ASSERT_NOT_NULL(brk);
    TEST_ASSERT_TRUE(strncmp(brk->key, "remaining_", 10) != 0);
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

/* HA builds an entity_id from the device name and the entity name unless
   discovery says otherwise, so renaming a device re-slugs every entity
   under it. That is not hypothetical here: a house automation excluding
   "magtag" stopped matching after two devices were renamed, and swept
   their switches off overnight. obj_id pins the entity_id to the
   MAC-derived device id instead. The rule these three tests hold is that
   EVERY discovery payload carries obj_id, and that its value is the same
   string as uniq_id. */
void test_discovery_object_id_is_the_unique_id(void) {
    char buf[STATS_JSON_PAYLOAD_MAX];
    char want[128];
    int count = 0;
    const ha_entity_t *ents = stats_json_entities(&count);
    for (int i = 0; i < count; i++) {
        stats_json_discovery(buf, sizeof(buf), "magtag-a1b2c3", "Kitchen MagTag", "fw", &ents[i]);
        snprintf(want, sizeof(want), "\"obj_id\":\"magtag-a1b2c3_%s\"", ents[i].key);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, want), ents[i].key);
        snprintf(want, sizeof(want), "\"uniq_id\":\"magtag-a1b2c3_%s\"", ents[i].key);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, want), ents[i].key);
    }
}

void test_discovery_object_id_does_not_move_when_the_device_is_renamed(void) {
    /* The actual regression, stated as a test: two device names that HA
       would slug differently must produce the same obj_id. The name
       itself still changes, which is what stops this passing vacuously. */
    char a[STATS_JSON_PAYLOAD_MAX], b[STATS_JSON_PAYLOAD_MAX];
    int count = 0;
    const ha_entity_t *ents = stats_json_entities(&count);
    stats_json_discovery(a, sizeof(a), "magtag-a1b2c3", "Testing Timer", "fw", &ents[0]);
    stats_json_discovery(b, sizeof(b), "magtag-a1b2c3", "Julia's Timer", "fw", &ents[0]);

    char want[128];
    snprintf(want, sizeof(want), "\"obj_id\":\"magtag-a1b2c3_%s\"", ents[0].key);
    TEST_ASSERT_NOT_NULL(strstr(a, want));
    TEST_ASSERT_NOT_NULL(strstr(b, want));
    TEST_ASSERT_NOT_NULL(strstr(a, "Testing Timer"));
    TEST_ASSERT_NOT_NULL(strstr(b, "Julia"));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(a, b));
}

void test_every_discovery_payload_fits_the_publish_buffer(void) {
    /* mqtt_ha.c builds discovery into the same STATS_JSON_PAYLOAD_MAX
       buffer as the stat payload, and an overflow there means the entity
       simply never appears in HA. It logs the skip now, but a bound is
       better than a log. Worst realistic case: the longest device name
       the name entity accepts, and the per-slot sensors' runtime name
       override at its own cap. */
    char buf[STATS_JSON_PAYLOAD_MAX];
    char longname[64];
    memset(longname, 'W', sizeof(longname) - 1); /* widest glyph, 63 chars */
    longname[sizeof(longname) - 1] = '\0';
    char override[48];
    memset(override, 'W', sizeof(override) - 1);
    override[sizeof(override) - 1] = '\0';

    int count = 0, worst = 0;
    const ha_entity_t *ents = stats_json_entities(&count);
    for (int i = 0; i < count; i++) {
        const int n = stats_json_discovery_named(buf, sizeof(buf), "magtag-a1b2c3", longname, "fw", &ents[i], override);
        if (n > worst)
            worst = n;
        TEST_ASSERT_TRUE_MESSAGE(n < (int)sizeof(buf), ents[i].key);
    }
    /* Not an equality assert — this is a headroom report that fails only
       if the margin is gone. Kept loose so an entity can be added without
       editing a magic number, and tight enough to notice a doubling. */
    TEST_ASSERT_TRUE_MESSAGE(worst < (int)sizeof(buf) - 128, "discovery headroom below 128 B");
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
    RUN_TEST(test_stat_payload_reports_the_exposure_balance);
    RUN_TEST(test_stat_payload_reset_reason_flags_crash_wakes);
    RUN_TEST(test_discovery_last_reset_diagnostic_sensor);
    RUN_TEST(test_stat_payload_escapes_timer_name);
    RUN_TEST(test_stat_payload_reports_needed_length_when_truncated);
    RUN_TEST(test_stat_payload_null_string_fields_are_safe);
    RUN_TEST(test_summary_payload_exact);
    RUN_TEST(test_discovery_entity_table_is_populated);
    RUN_TEST(test_discovery_schema_version_moves_with_the_entity_table);
    RUN_TEST(test_stat_payload_carries_the_ota_fields);
    RUN_TEST(test_stat_payload_ota_fields_track_the_argument);
    RUN_TEST(test_stat_payload_ota_duration_survives_a_long_download);
    RUN_TEST(test_stat_payload_null_ota_is_safe);
    RUN_TEST(test_stat_payload_escapes_the_ota_strings);
    RUN_TEST(test_stat_payload_worst_case_fits_the_publish_buffer);
    RUN_TEST(test_ota_entities_never_expire);
    RUN_TEST(test_ota_result_is_the_primary_entity_of_the_four);

    RUN_TEST(test_stat_payload_carries_the_panic_fields);
    RUN_TEST(test_stat_payload_escapes_the_panic_phase);
    RUN_TEST(test_stat_payload_null_diag_is_safe);
    RUN_TEST(test_panic_entities_never_expire);
    RUN_TEST(test_live_health_entities_expire);
    RUN_TEST(test_panic_count_is_the_primary_entity_of_the_group);
    RUN_TEST(test_discovery_topic);
    RUN_TEST(test_discovery_battery_payload);
    RUN_TEST(test_discovery_object_id_is_the_unique_id);
    RUN_TEST(test_discovery_object_id_does_not_move_when_the_device_is_renamed);
    RUN_TEST(test_every_discovery_payload_fits_the_publish_buffer);
    RUN_TEST(test_discovery_binary_sensor_has_payload_states);
    RUN_TEST(test_discovery_completions_use_runtime_slot_names);
    RUN_TEST(test_stat_payload_reports_a_running_break);
    RUN_TEST(test_discovery_screen_break_entities);
    RUN_TEST(test_screen_exposure_entity);
    RUN_TEST(test_break_entities_are_not_mistaken_for_per_slot_sensors);
    return UNITY_END();
}
