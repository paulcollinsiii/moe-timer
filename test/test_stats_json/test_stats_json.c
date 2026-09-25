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
    char buf[STATS_JSON_PAYLOAD_MAX];
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
        "\"chores_left\":0,\"chores_done\":0,\"chore_ack\":[0,0,0],\"cfg_warn\":\"OK\","
        "\"ota_result\":\"\",\"ota_target\":\"\",\"ota_fails\":0,\"ota_dl_ms\":0,"
        "\"panics\":0,\"pphase\":\"\",\"pup_s\":0,\"pheap\":0,\"pstk_main\":0,\"pstk_net\":0,"
        "\"heap\":0,\"heap_min\":0,\"stk_main\":0,\"stk_net\":0,\"nvs_free\":0}",
        buf);
    TEST_ASSERT_EQUAL_INT((int)strlen(buf), n);
}

/* ---- the chore leg (M3-T1, design 1.4) ---- */

void test_stat_payload_carries_the_chore_counts_and_acks(void) {
    char buf[STATS_JSON_PAYLOAD_MAX];
    stats_snapshot_t s = base_snapshot();
    s.chores_left = 1;
    s.chores_done = 2;
    s.chore_acked = 0x05; /* chores 1 and 3 */
    stats_json_stat(buf, sizeof(buf), &s, &NO_OTA, &NO_DIAG);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"chores_left\":1,\"chores_done\":2,\"chore_ack\":[1,0,1],"));
}

/* Each count on its own, so a builder that printed one field under the
   other's key — or either off by one — cannot pass by symmetry. */
void test_stat_payload_chore_counts_are_not_interchangeable(void) {
    char buf[STATS_JSON_PAYLOAD_MAX];
    stats_snapshot_t s = base_snapshot();
    s.chores_left = 3;
    s.chores_done = 0;
    stats_json_stat(buf, sizeof(buf), &s, &NO_OTA, &NO_DIAG);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"chores_left\":3,\"chores_done\":0,\"chore_ack\":[0,0,0]"));
    s.chores_left = 0;
    s.chores_done = 3;
    s.chore_acked = 0x07;
    stats_json_stat(buf, sizeof(buf), &s, &NO_OTA, &NO_DIAG);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"chores_left\":0,\"chores_done\":3,\"chore_ack\":[1,1,1]"));
}

/* The array is always CHORE_MAX long so every chore_N template has an
   index to read, and a position past the configured count reads 0 even
   when the caller left its bit set: a stale bit from a longer list must
   never light a per-chore sensor. Two chores here, and bit 2 set. */
void test_stat_payload_chore_ack_ignores_bits_past_the_configured_count(void) {
    char buf[STATS_JSON_PAYLOAD_MAX];
    stats_snapshot_t s = base_snapshot();
    s.chores_left = 1;
    s.chores_done = 1;
    s.chore_acked = 0x06; /* chore 2 acked, plus a stale bit 2 */
    stats_json_stat(buf, sizeof(buf), &s, &NO_OTA, &NO_DIAG);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"chore_ack\":[0,1,0]"));
}

/* No chores configured (row C1): both counts 0 and every position 0. The
   entities still exist — the chore_N rows are retired at discovery, the
   two counts are not — so the payload still has to feed them. */
void test_stat_payload_no_chores_reports_zeroes(void) {
    char buf[STATS_JSON_PAYLOAD_MAX];
    stats_snapshot_t s = base_snapshot();
    s.chore_acked = 0x07; /* stale bits, and no list */
    stats_json_stat(buf, sizeof(buf), &s, &NO_OTA, &NO_DIAG);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"chores_left\":0,\"chores_done\":0,\"chore_ack\":[0,0,0]"));
}

/* ---- the config warning (M2-D6) ---- */

/* Healthy reads "OK", never "": HA's MQTT sensor ignores an empty state
   and keeps the old one, so a warning that cleared to "" would never be
   seen to clear. */
void test_stat_payload_config_warning_is_ok_when_healthy(void) {
    char buf[STATS_JSON_PAYLOAD_MAX];
    stats_snapshot_t s = base_snapshot();
    s.chore_free_bad = 0;
    stats_json_stat(buf, sizeof(buf), &s, &NO_OTA, &NO_DIAG);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"cfg_warn\":\"OK\""));
}

/* Each day type on its own names that day type and nothing else. */
void test_stat_payload_config_warning_names_each_day_type(void) {
    const char *want[SCHEDULE_DAY_TYPES] = {
        [DAY_WEEKDAY] = "\"cfg_warn\":\"Weekday\"",
        [DAY_WEEKEND] = "\"cfg_warn\":\"Weekend\"",
        [DAY_HOLIDAY] = "\"cfg_warn\":\"Holiday\"",
        [DAY_SUMMER] = "\"cfg_warn\":\"Summer\"",
    };
    for (unsigned d = 0; d < SCHEDULE_DAY_TYPES; d++) {
        char buf[STATS_JSON_PAYLOAD_MAX];
        stats_snapshot_t s = base_snapshot();
        s.chore_free_bad = (uint8_t)(1u << d);
        stats_json_stat(buf, sizeof(buf), &s, &NO_OTA, &NO_DIAG);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, want[d]), want[d]);
    }
}

/* Several at once: all of them, in day_type_t order. */
void test_stat_payload_config_warning_names_several_day_types(void) {
    char buf[STATS_JSON_PAYLOAD_MAX];
    stats_snapshot_t s = base_snapshot();
    s.chore_free_bad = (1u << DAY_WEEKDAY) | (1u << DAY_SUMMER);
    stats_json_stat(buf, sizeof(buf), &s, &NO_OTA, &NO_DIAG);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"cfg_warn\":\"Weekday, Summer\""));
    s.chore_free_bad = 0x0F;
    stats_json_stat(buf, sizeof(buf), &s, &NO_OTA, &NO_DIAG);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"cfg_warn\":\"Weekday, Weekend, Holiday, Summer\""));
}

/* Bits past the last day type name nothing: they are ignored, not
   rendered as a fifth name or trusted into "broken". */
void test_stat_payload_config_warning_ignores_bits_past_the_day_types(void) {
    char buf[STATS_JSON_PAYLOAD_MAX];
    stats_snapshot_t s = base_snapshot();
    s.chore_free_bad = 0xF0;
    stats_json_stat(buf, sizeof(buf), &s, &NO_OTA, &NO_DIAG);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"cfg_warn\":\"OK\""));
    s.chore_free_bad = 0xF4;
    stats_json_stat(buf, sizeof(buf), &s, &NO_OTA, &NO_DIAG);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"cfg_warn\":\"Holiday\""));
}

void test_stat_payload_reports_a_running_break(void) {
    /* A Screen Break can run behind any selected timer, so HA cannot
       infer it from "state" any more — it needs its own field. */
    char buf[STATS_JSON_PAYLOAD_MAX];
    stats_snapshot_t s = base_snapshot();
    s.break_remaining_s = 754;
    stats_json_stat(buf, sizeof(buf), &s, &NO_OTA, &NO_DIAG);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"break_s\":754"));
}

/* The exposure balance is the only visibility HA gets into why a break
   did or did not fire — against the known interval it answers the
   question directly. Always >= 0: app_state feeds it the clamped read. */
void test_stat_payload_reports_the_exposure_balance(void) {
    char buf[STATS_JSON_PAYLOAD_MAX];
    stats_snapshot_t s = base_snapshot();
    s.accum_s = 1500;
    stats_json_stat(buf, sizeof(buf), &s, &NO_OTA, &NO_DIAG);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"accum_s\":1500"));
}

void test_stat_payload_reset_reason_flags_crash_wakes(void) {
    /* Boot forensics over MQTT: the USB CDC console drops output around
       sleep/reset transitions, so the reset reason rides the stat payload
       — a BROWNOUT/PANIC value on a wake means the PREVIOUS wake died. */
    char buf[STATS_JSON_PAYLOAD_MAX];
    stats_snapshot_t s = base_snapshot();
    s.reset_reason = "BROWNOUT";
    stats_json_stat(buf, sizeof(buf), &s, &NO_OTA, &NO_DIAG);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"reset\":\"BROWNOUT\""));
}

void test_stat_payload_charge_lock_true(void) {
    char buf[STATS_JSON_PAYLOAD_MAX];
    stats_snapshot_t s = base_snapshot();
    s.charge_lock = true;
    stats_json_stat(buf, sizeof(buf), &s, &NO_OTA, &NO_DIAG);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"charge_lock\":true"));
}

void test_stat_payload_escapes_timer_name(void) {
    char buf[STATS_JSON_PAYLOAD_MAX];
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
    char buf[STATS_JSON_PAYLOAD_MAX];
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
    int n = stats_json_summary(buf, sizeof(buf), "2026-07-08", 3200, comp, 2, 3);
    TEST_ASSERT_EQUAL_STRING(
        "{\"date\":\"2026-07-08\",\"screen_used_s\":3200,\"completions\":[1,2,0,0],"
        "\"chores_done\":2,\"chores\":3}",
        buf);
    TEST_ASSERT_EQUAL_INT((int)strlen(buf), n);
}

/* The two chore figures are distinct arguments landing in distinct
   fields: swapped, a day with 1 of 3 done would graph 3. And no list at
   all is written as 0 of 0, not omitted, so day_chores records the day. */
void test_summary_payload_chore_fields_are_not_swapped(void) {
    char buf[256];
    const uint16_t comp[TIMER_EXTRA_SLOTS] = {0, 0, 0, 0};
    stats_json_summary(buf, sizeof(buf), "2026-09-24", 0, comp, 1, 3);
    TEST_ASSERT_NOT_NULL(strstr(buf, ",\"chores_done\":1,\"chores\":3}"));
    stats_json_summary(buf, sizeof(buf), "2026-09-24", 0, comp, 0, 0);
    TEST_ASSERT_EQUAL_STRING(
        "{\"date\":\"2026-09-24\",\"screen_used_s\":0,\"completions\":[0,0,0,0],"
        "\"chores_done\":0,\"chores\":0}",
        buf);
}

/* A chore list that could not be read at the rollover: both chore fields
   are OMITTED (chores_done ignored), so day_chores' template renders
   "None" and HA records an unknown day, not a durable 0. Any negative
   count means unknown; 0 does not. The returned length stays the built
   length, as for every other payload. */
void test_summary_payload_omits_the_chore_fields_when_the_list_is_unknown(void) {
    char buf[256];
    const uint16_t comp[TIMER_EXTRA_SLOTS] = {1, 0, 0, 2};
    int n = stats_json_summary(buf, sizeof(buf), "2026-09-24", 60, comp, 2, STATS_JSON_CHORES_UNKNOWN);
    TEST_ASSERT_EQUAL_STRING("{\"date\":\"2026-09-24\",\"screen_used_s\":60,\"completions\":[1,0,0,2]}", buf);
    TEST_ASSERT_EQUAL_INT((int)strlen(buf), n);
    stats_json_summary(buf, sizeof(buf), "2026-09-24", 60, comp, 0, -7);
    TEST_ASSERT_NULL(strstr(buf, "chores"));
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
       heap_free, heap_min, stack_main, stack_net, nvs_free,
       chores_left, chores_done, config_warning, screen_used_day,
       day_chores
       + per extra slot: completions, day_runs, remaining, limit
       + per possible chore: chore_N */
    TEST_ASSERT_EQUAL_INT(33 + 4 * TIMER_EXTRA_SLOTS + CHORE_MAX, count);
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
    TEST_ASSERT_EQUAL_INT(33 + 4 * TIMER_EXTRA_SLOTS + CHORE_MAX, count);
    /* v24: count unchanged, cause outside ENTITIES — BUG-13's cmd_tpl on
       the ha_config registry's text controls (a changed payload on
       existing config rows; test_ha_config's joint pin records it).

       v23: + screen_used_day and the TIMER_EXTRA_SLOTS day_runs_N rows
       (the summary-topic rows, state_class "total" with a last_reset),
       and state_class "measurement" on battery (M4-T1, the dashboard's
       graph data). + day_chores (M4-T5), a sixth summary-topic row, added
       before v23 shipped and so under the same number: the count moved,
       the version rightly did not. The battery half changes no count: a
       changed payload on an existing row needs the bump as much as a new
       row does, and test_only_the_graphed_rows_declare_a_state_class pins
       that half.

       v22: + chores_left, chores_done, config_warning and the CHORE_MAX
       chore_N rows (M3-T1, one bump covering the chore entities,
       M2-D6's warning, and the state_class on the two chore counts).

       History the line keeps: v21 moved the version with the entity count
       unchanged — it added four ha_config REGISTRY rows (the chore_free_*
       controls), which ride the same republish gate but are not rows in
       ENTITIES. The pin is exact and the two numbers are asserted
       together, so it forbids a bump without a count change exactly as
       much as the reverse: any move in either number lands the author in
       this test, which is the point of pinning them side by side. A bump
       whose cause is outside ENTITIES is legitimate and is recorded here
       as such. The config registry has a joint pin of its own in
       test_ha_config. */
    TEST_ASSERT_EQUAL_INT(24, STATS_JSON_DISC_SCHEMA_VER);
}

/* ---- the OTA leg of the stat payload ---- */

/* The four fields come from the ota_stat_t argument, which mqtt_ha.c
   fills from NVS at publish time -- NOT from stats_snapshot_t, which was
   frozen on the main task before this wake's check ran. The snapshot has
   no OTA fields at all, so the mistake cannot be made silently; this
   pins the values it CAN carry. */
void test_stat_payload_carries_the_ota_fields(void) {
    char buf[STATS_JSON_PAYLOAD_MAX];
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
    char a[STATS_JSON_PAYLOAD_MAX], b[STATS_JSON_PAYLOAD_MAX];
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
    char buf[STATS_JSON_PAYLOAD_MAX];
    stats_snapshot_t s = base_snapshot();
    ota_stat_t ota = {.dl_ms = 298000}; /* just inside a 300 s budget */
    stats_json_stat(buf, sizeof(buf), &s, &ota, &NO_DIAG);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ota_dl_ms\":298000"));
}

/* The pure boundary must not fault on a NULL OTA leg either. */
void test_stat_payload_null_ota_is_safe(void) {
    char buf[STATS_JSON_PAYLOAD_MAX];
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
    char buf[STATS_JSON_PAYLOAD_MAX];
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
   the current producer's habits.

   The chore leg did it again: 938 of 1024 before it, about a hundred
   bytes more with it, so STATS_JSON_PAYLOAD_MAX moved to 1280 for the
   same reason rather than this assertion moving. */
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
    /* The chore leg at its declared widths, not its reachable ones: the
       counts are uint8_t and CHORE_MAX caps them at 3 in practice, but the
       buffer has to survive 255. Every ack set, and every day type broken —
       the longest config warning there is. */
    s.chores_left = 255;
    s.chores_done = 255;
    s.chore_acked = 0xFF;
    s.chore_free_bad = 0xFF;

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
    /* The chore leg really is in the measured payload — a worst case that
       silently lost it would pass the assertion above for free. */
    TEST_ASSERT_NOT_NULL(strstr(buf,
                                "\"chores_left\":255,\"chores_done\":255,\"chore_ack\":[1,1,1],"
                                "\"cfg_warn\":\"Weekday, Weekend, Holiday, Summer\""));
    /* Headroom, reported rather than pinned (the gate is needed < size,
       so this many more bytes still publish): a loose floor, the same
       shape as the discovery test's, that fails only when the margin is
       nearly gone rather than on every added byte. */
    printf("stat worst case %d of %d B, headroom %d B\n", n, (int)sizeof(buf), (int)sizeof(buf) - 1 - n);
    TEST_ASSERT_TRUE_MESSAGE(n < (int)sizeof(buf) - 64, "stat payload headroom below 64 B");
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
        "{\"name\":\"Battery\",\"uniq_id\":\"magtag-a1b2c3_battery\","
        "\"def_ent_id\":\"sensor.magtag-a1b2c3_battery\","
        "\"stat_t\":\"magtag/magtag-a1b2c3/stat\",\"val_tpl\":\"{{ value_json.batt_pct }}\","
        "\"unit_of_meas\":\"%\",\"dev_cla\":\"battery\",\"stat_cla\":\"measurement\",\"expire_after\":7500,"
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
    /* mqtt_ha.c attaches runtime slot names to the rows
       stats_json_slot_of() matches; screen_exposure must not be one of
       them or it would be renamed after a timer that has nothing to do
       with it. */
    const char *suffix = NULL;
    TEST_ASSERT_EQUAL_INT(0, stats_json_slot_of(exp, &suffix));
}

void test_break_entities_are_not_mistaken_for_per_slot_sensors(void) {
    /* mqtt_ha.c matches per-slot keys by prefix (stats_json_slot_of) to
       attach the runtime timer name. "break_remaining" must not collide
       with that, or discovery would look up a slot and retire the entity
       entirely. */
    const char *suffix = NULL;
    const ha_entity_t *rem = find_entity("break_remaining");
    TEST_ASSERT_NOT_NULL(rem);
    TEST_ASSERT_EQUAL_INT(0, stats_json_slot_of(rem, &suffix));
    const ha_entity_t *brk = find_entity("screen_break");
    TEST_ASSERT_NOT_NULL(brk);
    TEST_ASSERT_EQUAL_INT(0, stats_json_slot_of(brk, &suffix));
}

/* ---- chore entities (M3-T1, design 1.4) ---- */

/* chores_left is PRIMARY and chores_done DIAGNOSTIC, as the design lists
   them. Neither carries a unit: HA's logbook skips any entity with one,
   and these changing is the audit trail. */
void test_discovery_chore_count_entities(void) {
    char buf[STATS_JSON_PAYLOAD_MAX];
    const ha_entity_t *left = find_entity("chores_left");
    TEST_ASSERT_NOT_NULL(left);
    TEST_ASSERT_EQUAL_STRING("sensor", left->component);
    TEST_ASSERT_EQUAL_STRING("Chores left", left->name);
    TEST_ASSERT_EQUAL_STRING("{{ value_json.chores_left }}", left->tpl);
    TEST_ASSERT_EQUAL_STRING("stat", left->topic_suffix);
    TEST_ASSERT_NULL(left->ent_cat);
    TEST_ASSERT_NULL(left->unit);
    TEST_ASSERT_EQUAL_INT(STAT_EXPIRE_SEC, left->expire_after);
    TEST_ASSERT_EQUAL_STRING("measurement", left->state_class);
    stats_json_discovery(buf, sizeof(buf), "magtag-a1b2c3", "Kitchen", "fw", left);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"def_ent_id\":\"sensor.magtag-a1b2c3_chores_left\""));
    TEST_ASSERT_NULL(strstr(buf, "ent_cat"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"stat_cla\":\"measurement\""));

    const ha_entity_t *done = find_entity("chores_done");
    TEST_ASSERT_NOT_NULL(done);
    TEST_ASSERT_EQUAL_STRING("sensor", done->component);
    TEST_ASSERT_EQUAL_STRING("Chores done", done->name);
    TEST_ASSERT_EQUAL_STRING("{{ value_json.chores_done }}", done->tpl);
    TEST_ASSERT_EQUAL_STRING("diagnostic", done->ent_cat);
    TEST_ASSERT_NULL(done->unit);
    TEST_ASSERT_EQUAL_INT(STAT_EXPIRE_SEC, done->expire_after);
    TEST_ASSERT_EQUAL_STRING("measurement", done->state_class);
    stats_json_discovery(buf, sizeof(buf), "magtag-a1b2c3", "Kitchen", "fw", done);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"def_ent_id\":\"sensor.magtag-a1b2c3_chores_done\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ent_cat\":\"diagnostic\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"stat_cla\":\"measurement\""));
}

/* state_class buys HA's long-term statistics and costs the logbook: HA
   leaves every sensor that declares one out of it. So it is on exactly
   the rows kept for statistics — the ones the M4 dashboard graphs, the
   two summary rows whose graphs are added by hand since M4-T7, and the
   two chore counts — each with the class its statistics need, and nowhere
   else — the config warning above all, whose logbook line IS
   the feature. A row gaining one by accident fails here, as does a graphed
   row losing its class or taking the wrong one, and the builder dropping
   it from the payload. */
static const struct {
    const char *key, *state_class;
} GRAPHED[] = {
    {"battery", "measurement"},     /* Battery Charge: hourly `mean` */
    {"screen_used_day", "total"},   /* screen minutes per day: `change` (by hand) */
    {"day_runs_1", "total"},        /* runs per day per extra timer: `change` */
    {"day_runs_2", "total"},        /* ... */
    {"day_runs_3", "total"},        /* ... */
    {"day_runs_4", "total"},        /* ... */
    {"day_chores", "total"},        /* chores done per day: `change` (by hand) */
    {"chores_left", "measurement"}, /* long-term statistics since v22 */
    {"chores_done", "measurement"},
};
_Static_assert(TIMER_EXTRA_SLOTS == 4, "GRAPHED lists one day_runs_N per extra slot");
#define GRAPHED_COUNT (sizeof(GRAPHED) / sizeof(GRAPHED[0]))

void test_only_the_graphed_rows_declare_a_state_class(void) {
    char buf[STATS_JSON_PAYLOAD_MAX], want[64];
    int count = 0, declared = 0;
    const ha_entity_t *ents = stats_json_entities(&count);
    for (int i = 0; i < count; i++) {
        stats_json_discovery(buf, sizeof(buf), "magtag-a1b2c3", "Kitchen", "fw", &ents[i]);
        const char *expect = NULL;
        for (size_t g = 0; g < GRAPHED_COUNT; g++) {
            if (strcmp(ents[i].key, GRAPHED[g].key) == 0)
                expect = GRAPHED[g].state_class;
        }
        if (expect == NULL) {
            TEST_ASSERT_NULL_MESSAGE(ents[i].state_class, ents[i].key);
            TEST_ASSERT_NULL_MESSAGE(strstr(buf, "stat_cla"), ents[i].key);
            continue;
        }
        declared++;
        TEST_ASSERT_EQUAL_STRING_MESSAGE(expect, ents[i].state_class, ents[i].key);
        snprintf(want, sizeof(want), "\"stat_cla\":\"%s\"", expect);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, want), ents[i].key);
    }
    /* Every graphed key is a row: a renamed or dropped row fails here
       rather than quietly shrinking the allowlist's reach. */
    TEST_ASSERT_EQUAL_INT((int)GRAPHED_COUNT, declared);
    TEST_ASSERT_NULL(find_entity("config_warning")->state_class);
}

/* last_reset_value_template is legal ONLY beside state_class "total": HA's
   MQTT sensor schema rejects the whole discovery config otherwise, so the
   entity would silently never appear. It is on exactly the summary-topic
   rows, which are exactly the "total" rows, and it reaches the payload
   under HA's abbreviation for it. */
void test_only_the_summary_rows_carry_a_last_reset(void) {
    char buf[STATS_JSON_PAYLOAD_MAX];
    int count = 0, carried = 0;
    const ha_entity_t *ents = stats_json_entities(&count);
    for (int i = 0; i < count; i++) {
        stats_json_discovery(buf, sizeof(buf), "magtag-a1b2c3", "Kitchen", "fw", &ents[i]);
        const bool summary = strcmp(ents[i].topic_suffix, "summary") == 0;
        const bool total = ents[i].state_class != NULL && strcmp(ents[i].state_class, "total") == 0;
        TEST_ASSERT_EQUAL_MESSAGE(summary, total, ents[i].key);
        if (!summary) {
            TEST_ASSERT_NULL_MESSAGE(ents[i].last_reset_tpl, ents[i].key);
            TEST_ASSERT_NULL_MESSAGE(strstr(buf, "lrst_val_tpl"), ents[i].key);
            continue;
        }
        carried++;
        TEST_ASSERT_NOT_NULL_MESSAGE(ents[i].last_reset_tpl, ents[i].key);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "\"lrst_val_tpl\":\"{{ value_json.date ~ 'T00:00:00+00:00' }}\""),
                                     ents[i].key);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, ents[i].expire_after, ents[i].key);
    }
    /* screen_used_day, day_runs_1..N, day_chores */
    TEST_ASSERT_EQUAL_INT(2 + TIMER_EXTRA_SLOTS, carried);
}

/* The live run counts keep NO state_class (v23 reverted the first draft's
   total_increasing): the rollover zeroes them before HA sees a run
   finished after the day's last window, so they cannot be the runs graph,
   and without a class each run keeps its logbook line. day_runs_N reads
   the summary instead. */
void test_discovery_completions_keep_no_state_class(void) {
    char buf[STATS_JSON_PAYLOAD_MAX];
    for (int s = 1; s <= TIMER_EXTRA_SLOTS; s++) {
        char key[24];
        snprintf(key, sizeof(key), "completions_%d", s);
        const ha_entity_t *c = find_entity(key);
        TEST_ASSERT_NOT_NULL_MESSAGE(c, key);
        TEST_ASSERT_NULL_MESSAGE(c->state_class, key);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("stat", c->topic_suffix, key);
        stats_json_discovery_named(buf, sizeof(buf), "magtag-a1b2c3", "Kitchen", "fw", c, "Violin runs");
        TEST_ASSERT_NULL_MESSAGE(strstr(buf, "stat_cla"), key);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "\"name\":\"Violin runs\""), key);
    }
}

/* The finished day's Screen minutes, for a "screen minutes per day" graph.
   The whole payload is pinned: the state topic is the retained SUMMARY
   topic (not stat), the template reads the summary's own field, the unit
   matches the other minute sensors, state_class "total" with a
   last_reset from the summary's date (each summary a new cycle), and NO
   expire_after — the summary arrives once a day, and an expiry would
   blank it for the hours in between. */
void test_discovery_screen_used_day_payload(void) {
    char buf[STATS_JSON_PAYLOAD_MAX];
    const ha_entity_t *e = find_entity("screen_used_day");
    TEST_ASSERT_NOT_NULL(e);
    int n = stats_json_discovery(buf, sizeof(buf), "magtag-a1b2c3", "Kitchen MagTag", "v1.4.0-test", e);
    TEST_ASSERT_EQUAL_STRING(
        "{\"name\":\"Screen time per day\",\"uniq_id\":\"magtag-a1b2c3_screen_used_day\","
        "\"def_ent_id\":\"sensor.magtag-a1b2c3_screen_used_day\","
        "\"stat_t\":\"magtag/magtag-a1b2c3/summary\","
        "\"val_tpl\":\"{{ (value_json.screen_used_s / 60) | round(0) }}\","
        "\"unit_of_meas\":\"min\",\"dev_cla\":\"duration\",\"stat_cla\":\"total\","
        "\"lrst_val_tpl\":\"{{ value_json.date ~ 'T00:00:00+00:00' }}\","
        "\"ent_cat\":\"diagnostic\","
        "\"dev\":{\"ids\":[\"magtag-a1b2c3\"],\"name\":\"Kitchen MagTag\",\"mf\":\"Adafruit\","
        "\"mdl\":\"MagTag 2.9\",\"sw\":\"v1.4.0-test\"}}",
        buf);
    TEST_ASSERT_EQUAL_INT((int)strlen(buf), n);
    TEST_ASSERT_EQUAL_INT(0, e->expire_after);
}

/* The finished day's runs of each extra timer, the "how often was Violin
   finished" graph. Pinned whole, per slot: the summary topic, the slot's
   OWN position of the summary's completions array (a neighbour's index
   would graph the wrong timer for ever), "total" with the summary's
   last_reset, no unit, diagnostic like screen_used_day, no expire. Once
   under the table name and once under the runtime slot name mqtt_ha.c
   passes, which must also fit the payload buffer. */
void test_discovery_day_runs_payloads(void) {
    char buf[STATS_JSON_PAYLOAD_MAX], key[16], want[640];
    for (int s = 1; s <= TIMER_EXTRA_SLOTS; s++) {
        snprintf(key, sizeof(key), "day_runs_%d", s);
        const ha_entity_t *e = find_entity(key);
        TEST_ASSERT_NOT_NULL_MESSAGE(e, key);
        for (int named = 0; named < 2; named++) {
            char name[32];
            snprintf(name, sizeof(name), named ? "Violin runs per day" : "Timer %d runs per day", s);
            int n = stats_json_discovery_named(buf, sizeof(buf), "magtag-a1b2c3", "Kitchen MagTag", "v1.4.0-test", e,
                                               named ? name : NULL);
            snprintf(want, sizeof(want),
                     "{\"name\":\"%s\",\"uniq_id\":\"magtag-a1b2c3_day_runs_%d\","
                     "\"def_ent_id\":\"sensor.magtag-a1b2c3_day_runs_%d\","
                     "\"stat_t\":\"magtag/magtag-a1b2c3/summary\","
                     "\"val_tpl\":\"{{ value_json.completions[%d] }}\","
                     "\"stat_cla\":\"total\","
                     "\"lrst_val_tpl\":\"{{ value_json.date ~ 'T00:00:00+00:00' }}\","
                     "\"ent_cat\":\"diagnostic\","
                     "\"dev\":{\"ids\":[\"magtag-a1b2c3\"],\"name\":\"Kitchen MagTag\",\"mf\":\"Adafruit\","
                     "\"mdl\":\"MagTag 2.9\",\"sw\":\"v1.4.0-test\"}}",
                     name, s, s, s - 1);
            TEST_ASSERT_EQUAL_STRING_MESSAGE(want, buf, key);
            TEST_ASSERT_EQUAL_INT_MESSAGE((int)strlen(buf), n, key);
            TEST_ASSERT_LESS_THAN_INT_MESSAGE(STATS_JSON_PAYLOAD_MAX, n, key);
        }
    }
}

/* The name of the summary field a template reads: what follows
   "value_json." up to the first character that cannot be part of it. */
static void summary_field(const char *tpl, char *out, size_t len) {
    TEST_ASSERT_NOT_NULL(tpl);
    const char *field = strstr(tpl, "value_json.");
    TEST_ASSERT_NOT_NULL_MESSAGE(field, tpl);
    field += strlen("value_json.");
    snprintf(out, len, "\"%.*s\":", (int)strcspn(field, " )|}[~"), field);
}

/* The templates and the summary builder are two ends of one wire: every
   field a summary-topic row reads — its value AND its last_reset — must
   be one stats_json_summary writes, under that exact name, or HA renders
   the sensor "unknown" (or drops every last_reset) every day. The date is
   also pinned as the "YYYY-MM-DD" the last_reset template appends a time
   to: anything else would not parse as the datetime HA requires. */
void test_summary_rows_read_fields_the_summary_writes(void) {
    char buf[256], quoted[32];
    const uint16_t comp[TIMER_EXTRA_SLOTS] = {3, 0, 1, 2};
    stats_json_summary(buf, sizeof(buf), "2026-09-24", 5400, comp, 2, 3);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"date\":\"2026-09-24\","));
    int count = 0, rows = 0;
    const ha_entity_t *ents = stats_json_entities(&count);
    for (int i = 0; i < count; i++) {
        if (strcmp(ents[i].topic_suffix, "summary") != 0)
            continue;
        rows++;
        summary_field(ents[i].tpl, quoted, sizeof(quoted));
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, quoted), ents[i].key);
        summary_field(ents[i].last_reset_tpl, quoted, sizeof(quoted));
        TEST_ASSERT_EQUAL_STRING_MESSAGE("\"date\":", quoted, ents[i].key);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, quoted), ents[i].key);
    }
    TEST_ASSERT_EQUAL_INT(2 + TIMER_EXTRA_SLOTS, rows);
    summary_field(find_entity("screen_used_day")->tpl, quoted, sizeof(quoted));
    TEST_ASSERT_EQUAL_STRING("\"screen_used_s\":", quoted);
    summary_field(find_entity("day_runs_1")->tpl, quoted, sizeof(quoted));
    TEST_ASSERT_EQUAL_STRING("\"completions\":", quoted);
    const ha_entity_t *day_chores = find_entity("day_chores");
    TEST_ASSERT_NOT_NULL(day_chores);
    summary_field(day_chores->tpl, quoted, sizeof(quoted));
    TEST_ASSERT_EQUAL_STRING("\"chores_done\":", quoted);
}

/* The finished day's acked chores, for a "chores done per day" graph.
   Pinned whole: the summary topic, the summary's chores_done (NOT the
   configured count beside it) or the literal 'None' when the summary has
   no such field (HA's MQTT sensor maps a "None" render to unknown; an
   empty render would keep the old value under a new last_reset and count
   it again), "total" with the summary's last_reset, no unit, diagnostic
   like day_runs_N, no expire. The template's quotes are single, so they
   pass through the JSON string unescaped. */
void test_discovery_day_chores_payload(void) {
    char buf[STATS_JSON_PAYLOAD_MAX];
    const ha_entity_t *e = find_entity("day_chores");
    TEST_ASSERT_NOT_NULL(e);
    int n = stats_json_discovery(buf, sizeof(buf), "magtag-a1b2c3", "Kitchen MagTag", "v1.4.0-test", e);
    TEST_ASSERT_EQUAL_STRING(
        "{\"name\":\"Chores done per day\",\"uniq_id\":\"magtag-a1b2c3_day_chores\","
        "\"def_ent_id\":\"sensor.magtag-a1b2c3_day_chores\","
        "\"stat_t\":\"magtag/magtag-a1b2c3/summary\","
        "\"val_tpl\":\"{{ value_json.chores_done if value_json.chores_done is defined else 'None' }}\","
        "\"stat_cla\":\"total\","
        "\"lrst_val_tpl\":\"{{ value_json.date ~ 'T00:00:00+00:00' }}\","
        "\"ent_cat\":\"diagnostic\","
        "\"dev\":{\"ids\":[\"magtag-a1b2c3\"],\"name\":\"Kitchen MagTag\",\"mf\":\"Adafruit\","
        "\"mdl\":\"MagTag 2.9\",\"sw\":\"v1.4.0-test\"}}",
        buf);
    TEST_ASSERT_EQUAL_INT((int)strlen(buf), n);
    TEST_ASSERT_EQUAL_INT(0, e->expire_after);
    TEST_ASSERT_FALSE(e->binary);
}

/* mqtt_ha.c renames and retires rows by key: a per-slot prefix match
   would name it after a timer and retire it with a disabled slot, a
   chore_N match would retire it past the configured chore count, and a
   RETIRED[] key would be emptied on every pass. It must be none of them,
   and no other row may share the key. */
void test_day_chores_key_avoids_the_per_slot_chore_and_retired_keys(void) {
    const char *suffix = NULL;
    const ha_entity_t *e = find_entity("day_chores");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(0, stats_json_slot_of(e, &suffix));
    TEST_ASSERT_EQUAL_INT(-1, stats_json_chore_index(e));
    /* A hand copy: the source of truth is mqtt_ha.c's RETIRED[] (inside
       its discovery pass, and mqtt_ha.c is linked into no host suite, so
       a C test cannot read it). A key added there must be added here. */
    static const char *RETIRED_KEYS[] = {"remaining", "allocation", "screen_used"};
    for (size_t i = 0; i < sizeof(RETIRED_KEYS) / sizeof(RETIRED_KEYS[0]); i++)
        TEST_ASSERT_NOT_EQUAL(0, strcmp(e->key, RETIRED_KEYS[i]));
    int count = 0, same = 0;
    const ha_entity_t *ents = stats_json_entities(&count);
    for (int i = 0; i < count; i++)
        same += strcmp(ents[i].key, "day_chores") == 0;
    TEST_ASSERT_EQUAL_INT(1, same);
}

/* The key must not be "screen_used": mqtt_ha.c's RETIRED[] publishes an
   EMPTY retained discovery for sensor/<id>_screen_used on every pass, so
   an entity under that key would be deleted as fast as it was created.
   And it must not be read as a per-slot or per-chore row. */
void test_screen_used_day_key_avoids_the_retired_and_per_slot_keys(void) {
    const char *suffix = NULL;
    const ha_entity_t *e = find_entity("screen_used_day");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_NOT_EQUAL(0, strcmp(e->key, "screen_used"));
    TEST_ASSERT_EQUAL_INT(0, stats_json_slot_of(e, &suffix));
    TEST_ASSERT_EQUAL_INT(-1, stats_json_chore_index(e));
}

/* mqtt_ha.c names each per-slot row after the slot's timer and retires it
   when the slot is disabled, by what stats_json_slot_of() answers. So:
   every per-slot row resolves to its own slot and suffix, each slot has
   exactly its four rows, and nothing else — above all no key that only
   shares a prefix — resolves at all. day_runs_N must not read as
   completions_N (its name would then be "Violin runs", twice) or the
   reverse. */
void test_slot_of_matches_only_the_per_slot_rows(void) {
    static const struct {
        const char *key, *suffix;
        int slot;
    } WANT[] = {
        {"completions_1", "runs", 1},      {"completions_4", "runs", 4},    {"day_runs_1", "runs per day", 1},
        {"day_runs_4", "runs per day", 4}, {"remaining_2", "remaining", 2}, {"limit_3", "limit", 3},
    };
    for (size_t w = 0; w < sizeof(WANT) / sizeof(WANT[0]); w++) {
        const char *suffix = NULL;
        const ha_entity_t *e = find_entity(WANT[w].key);
        TEST_ASSERT_NOT_NULL_MESSAGE(e, WANT[w].key);
        TEST_ASSERT_EQUAL_INT_MESSAGE(WANT[w].slot, stats_json_slot_of(e, &suffix), WANT[w].key);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(WANT[w].suffix, suffix, WANT[w].key);
    }

    int count = 0, per_slot[TIMER_EXTRA_SLOTS + 1] = {0};
    const ha_entity_t *ents = stats_json_entities(&count);
    for (int i = 0; i < count; i++) {
        const char *suffix = NULL;
        const int slot = stats_json_slot_of(&ents[i], &suffix);
        TEST_ASSERT_TRUE_MESSAGE(slot >= 0 && slot <= TIMER_EXTRA_SLOTS, ents[i].key);
        per_slot[slot]++;
        if (slot > 0) {
            /* The slot's rows read the slot's own index. */
            char idx[16];
            snprintf(idx, sizeof(idx), "[%d]",
                     strncmp(ents[i].key, "completions_", 12) == 0 || strncmp(ents[i].key, "day_runs_", 9) == 0
                         ? slot - 1
                         : slot);
            TEST_ASSERT_NOT_NULL_MESSAGE(strstr(ents[i].tpl, idx), ents[i].key);
        }
    }
    for (int s = 1; s <= TIMER_EXTRA_SLOTS; s++)
        TEST_ASSERT_EQUAL_INT(4, per_slot[s]);
    TEST_ASSERT_EQUAL_INT(count - 4 * TIMER_EXTRA_SLOTS, per_slot[0]);

    /* Keys that only LOOK like a per-slot row. */
    const ha_entity_t fake[] = {
        {.key = "day_runs_0"},   {.key = "day_runs_5"},       {.key = "day_runs_12"}, {.key = "day_runs_"},
        {.key = "completions_"}, {.key = "completions_1x"},   {.key = "day_runs"},    {.key = "limit_9"},
        {.key = "day_type"},     {.key = "screen_remaining"},
    };
    for (size_t i = 0; i < sizeof(fake) / sizeof(fake[0]); i++) {
        const char *suffix = NULL;
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, stats_json_slot_of(&fake[i], &suffix), fake[i].key);
    }
}

/* One binary_sensor per POSSIBLE chore, chore_1..chore_CHORE_MAX, each
   reading its own position of chore_ack — a row reading its neighbour's
   index would report the wrong chore's history for ever. */
void test_discovery_one_binary_sensor_per_chore(void) {
    char buf[STATS_JSON_PAYLOAD_MAX];
    for (int i = 0; i < CHORE_MAX; i++) {
        char key[16], tpl[64], defid[64];
        snprintf(key, sizeof(key), "chore_%d", i + 1);
        const ha_entity_t *e = find_entity(key);
        TEST_ASSERT_NOT_NULL_MESSAGE(e, key);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("binary_sensor", e->component, key);
        TEST_ASSERT_TRUE_MESSAGE(e->binary, key);
        snprintf(tpl, sizeof(tpl), "{{ 'ON' if value_json.chore_ack[%d] else 'OFF' }}", i);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(tpl, e->tpl, key);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("stat", e->topic_suffix, key);
        TEST_ASSERT_NULL_MESSAGE(e->ent_cat, key); /* primary: the per-chore audit trail */
        TEST_ASSERT_EQUAL_INT_MESSAGE(STAT_EXPIRE_SEC, e->expire_after, key);
        TEST_ASSERT_EQUAL_INT_MESSAGE(i, stats_json_chore_index(e), key);
        stats_json_discovery(buf, sizeof(buf), "magtag-a1b2c3", "Kitchen", "fw", e);
        snprintf(defid, sizeof(defid), "\"def_ent_id\":\"binary_sensor.magtag-a1b2c3_%s\"", key);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, defid), key);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "\"pl_on\":\"ON\",\"pl_off\":\"OFF\""), key);
    }
    /* ...and not one more: a chore_4 row would publish a sensor no ack
       button can ever drive. */
    char extra[16];
    snprintf(extra, sizeof(extra), "chore_%d", CHORE_MAX + 1);
    TEST_ASSERT_NULL(find_entity(extra));
}

/* mqtt_ha.c names each chore row from the list, through the runtime
   override, and the name is escaped on the way: chore names arrive from
   an MQTT document. */
void test_discovery_chore_sensor_takes_the_runtime_chore_name(void) {
    char buf[STATS_JSON_PAYLOAD_MAX];
    const ha_entity_t *e = find_entity("chore_2");
    TEST_ASSERT_NOT_NULL(e);
    stats_json_discovery_named(buf, sizeof(buf), "magtag-a1b2c3", "Kitchen", "fw", e, "Homework done");
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"name\":\"Homework done\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "value_json.chore_ack[1]"));
    /* The entity id does not follow the name: a rename is a new name on
       the same entity, not a new entity. */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"def_ent_id\":\"binary_sensor.magtag-a1b2c3_chore_2\""));
    stats_json_discovery_named(buf, sizeof(buf), "magtag-a1b2c3", "Kitchen", "fw", e, "Say \"hi\" done");
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"name\":\"Say \\\"hi\\\" done\""));
}

/* The index is an EXACT match on the whole key. chores_left and
   chores_done share "chore" with the per-chore rows, and a prefix test
   would have mqtt_ha.c rename or retire them as if they were chores. */
void test_chore_index_matches_only_the_per_chore_rows(void) {
    int count = 0;
    const ha_entity_t *ents = stats_json_entities(&count);
    int chores = 0;
    for (int i = 0; i < count; i++) {
        const int idx = stats_json_chore_index(&ents[i]);
        if (idx >= 0) {
            chores++;
            TEST_ASSERT_EQUAL_STRING_MESSAGE("binary_sensor", ents[i].component, ents[i].key);
            TEST_ASSERT_TRUE_MESSAGE(idx < CHORE_MAX, ents[i].key);
        }
    }
    TEST_ASSERT_EQUAL_INT(CHORE_MAX, chores);
    TEST_ASSERT_EQUAL_INT(-1, stats_json_chore_index(find_entity("chores_left")));
    TEST_ASSERT_EQUAL_INT(-1, stats_json_chore_index(find_entity("chores_done")));
    TEST_ASSERT_EQUAL_INT(-1, stats_json_chore_index(find_entity("config_warning")));
    TEST_ASSERT_EQUAL_INT(-1, stats_json_chore_index(find_entity("battery")));
    /* Keys that only LOOK like a chore row. */
    const ha_entity_t fake[] = {
        {.key = "chore_0"}, {.key = "chore_4"}, {.key = "chore_12"}, {.key = "chore_"}, {.key = "chore"},
    };
    for (size_t i = 0; i < sizeof(fake) / sizeof(fake[0]); i++)
        TEST_ASSERT_EQUAL_INT_MESSAGE(-1, stats_json_chore_index(&fake[i]), fake[i].key);
}

/* None of the new rows may collide with the per-slot prefixes mqtt_ha.c
   matches to attach a timer name — it would rename or retire them after a
   timer that has nothing to do with them. */
void test_chore_entities_are_not_mistaken_for_per_slot_sensors(void) {
    const char *keys[] = {"chores_left", "chores_done", "config_warning", "chore_1", "chore_2", "chore_3"};
    for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        const ha_entity_t *e = find_entity(keys[k]);
        TEST_ASSERT_NOT_NULL_MESSAGE(e, keys[k]);
        const char *suffix = NULL;
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, stats_json_slot_of(e, &suffix), keys[k]);
    }
}

/* M2-D6's warning: one DIAGNOSTIC text sensor, fed by cfg_warn, with no
   unit (the logbook would skip it, and the activity-log line is the whole
   point) and no expiry (it reports a stored configuration, which stays
   broken while the device is silent). */
void test_discovery_config_warning_entity(void) {
    char buf[STATS_JSON_PAYLOAD_MAX];
    const ha_entity_t *w = find_entity("config_warning");
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_EQUAL_STRING("sensor", w->component);
    TEST_ASSERT_EQUAL_STRING("Config warning", w->name);
    TEST_ASSERT_EQUAL_STRING("{{ value_json.cfg_warn }}", w->tpl);
    TEST_ASSERT_EQUAL_STRING("stat", w->topic_suffix);
    TEST_ASSERT_EQUAL_STRING("diagnostic", w->ent_cat);
    TEST_ASSERT_NULL(w->unit);
    TEST_ASSERT_NULL(w->dev_class);
    TEST_ASSERT_FALSE(w->binary);
    TEST_ASSERT_EQUAL_INT(0, w->expire_after);
    TEST_ASSERT_NULL(w->state_class); /* a state_class would drop it from the logbook too */
    stats_json_discovery(buf, sizeof(buf), "magtag-a1b2c3", "Kitchen", "fw", w);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"def_ent_id\":\"sensor.magtag-a1b2c3_config_warning\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ent_cat\":\"diagnostic\""));
    TEST_ASSERT_NULL(strstr(buf, "expire_after"));
    TEST_ASSERT_NULL(strstr(buf, "stat_cla"));
}

/* ---- the chore_N discovery decision (stats_json_chore_discovery) --------
   mqtt_ha.c's discovery pass only carries this out, and no host suite
   compiles mqtt_ha.c, so this is where name / retire / skip is pinned. */

/* Three DISTINCT names, so a row named from its neighbour's slot reads
   as the wrong string rather than passing by coincidence. */
static const char CHORE_NAMES3[CHORE_MAX][CHORE_NAME_BUF] = {"Dishes", "Homework", "Trash"};

static const ha_entity_t *chore_row(int i) {
    char key[16];
    snprintf(key, sizeof(key), "chore_%d", i + 1);
    const ha_entity_t *e = find_entity(key);
    TEST_ASSERT_NOT_NULL_MESSAGE(e, key);
    return e;
}

/* Every configured count, every chore row: below the count PUBLISH under
   "<that row's name> done", at or past it RETIRE. The n == index cell is
   the boundary — the first row that must retire. */
void test_chore_discovery_names_configured_rows_and_retires_the_rest(void) {
    for (int n = 0; n <= CHORE_MAX; n++) {
        for (int i = 0; i < CHORE_MAX; i++) {
            char out[STATS_JSON_CHORE_ENTITY_NAME_BUF] = "untouched";
            char msg[32], want[STATS_JSON_CHORE_ENTITY_NAME_BUF];
            snprintf(msg, sizeof(msg), "n=%d chore_%d", n, i + 1);
            const stats_chore_disc_t got = stats_json_chore_discovery(chore_row(i), CHORE_NAMES3, n, out, sizeof(out));
            if (i < n) {
                TEST_ASSERT_EQUAL_INT_MESSAGE(STATS_CHORE_DISC_PUBLISH, got, msg);
                snprintf(want, sizeof(want), "%s done", CHORE_NAMES3[i]);
                TEST_ASSERT_EQUAL_STRING_MESSAGE(want, out, msg);
            } else {
                TEST_ASSERT_EQUAL_INT_MESSAGE(STATS_CHORE_DISC_RETIRE, got, msg);
                TEST_ASSERT_EQUAL_STRING_MESSAGE("untouched", out, msg); /* written only for PUBLISH */
            }
        }
    }
}

/* A failed read (n < 0): every chore row is SKIPPED — not retired, which
   would delete the owner's entities over a transient flash error, and not
   published under a default name either, which would rename them. */
void test_chore_discovery_skips_every_chore_row_when_the_list_is_unknown(void) {
    for (int i = 0; i < CHORE_MAX; i++) {
        char out[STATS_JSON_CHORE_ENTITY_NAME_BUF] = "untouched";
        TEST_ASSERT_EQUAL_INT(STATS_CHORE_DISC_SKIP,
                              stats_json_chore_discovery(chore_row(i), CHORE_NAMES3, -1, out, sizeof(out)));
        TEST_ASSERT_EQUAL_STRING("untouched", out);
    }
}

/* Every other row is NOT_CHORE whatever the list says, including the
   list being unknown — a failed chore read must not hold back the
   battery sensor — and its name buffer is left alone. */
void test_chore_discovery_leaves_every_other_row_to_the_caller(void) {
    int count = 0;
    const ha_entity_t *ents = stats_json_entities(&count);
    for (int i = 0; i < count; i++) {
        if (stats_json_chore_index(&ents[i]) >= 0)
            continue;
        for (int n = -1; n <= CHORE_MAX; n++) {
            char out[STATS_JSON_CHORE_ENTITY_NAME_BUF] = "untouched";
            TEST_ASSERT_EQUAL_INT_MESSAGE(STATS_CHORE_DISC_NOT_CHORE,
                                          stats_json_chore_discovery(&ents[i], CHORE_NAMES3, n, out, sizeof(out)),
                                          ents[i].key);
            TEST_ASSERT_EQUAL_STRING_MESSAGE("untouched", out, ents[i].key);
        }
    }
}

/* The longest name the config path accepts (CHORE_NAME_MAX bytes, the
   bound apply_chores enforces) fits the declared buffer with its suffix,
   exactly — and a row that filled all CHORE_NAME_BUF bytes with no NUL
   (only a hand-built array can) is read to CHORE_NAME_MAX and no further. */
_Static_assert(STATS_JSON_CHORE_ENTITY_NAME_BUF == CHORE_NAME_MAX + sizeof(" done"),
               "the chore display-name buffer is sized from the name cap");
void test_chore_discovery_longest_name_fits_its_buffer(void) {
    char names[CHORE_MAX][CHORE_NAME_BUF];
    memset(names, 'W', sizeof(names)); /* every row unterminated */
    char out[STATS_JSON_CHORE_ENTITY_NAME_BUF];
    char want[STATS_JSON_CHORE_ENTITY_NAME_BUF];
    memset(want, 'W', CHORE_NAME_MAX);
    memcpy(want + CHORE_NAME_MAX, " done", sizeof(" done"));
    TEST_ASSERT_EQUAL_INT(STATS_CHORE_DISC_PUBLISH,
                          stats_json_chore_discovery(chore_row(CHORE_MAX - 1), names, CHORE_MAX, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(want, out);
    TEST_ASSERT_EQUAL_size_t(sizeof(out) - 1, strlen(out));
}

/* An empty row inside the count ("3 chores, the middle one empty" — the
   blob can hold it though the config path refuses it) keeps the table's
   default name rather than becoming an entity called " done". A zeroed
   list with n == 0 retires everything. */
void test_chore_discovery_empty_rows(void) {
    const char holey[CHORE_MAX][CHORE_NAME_BUF] = {"Dishes", "", "Trash"};
    char out[STATS_JSON_CHORE_ENTITY_NAME_BUF];
    TEST_ASSERT_EQUAL_INT(STATS_CHORE_DISC_PUBLISH,
                          stats_json_chore_discovery(chore_row(1), holey, CHORE_MAX, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Chore 2 done", out);
    TEST_ASSERT_EQUAL_INT(STATS_CHORE_DISC_PUBLISH,
                          stats_json_chore_discovery(chore_row(2), holey, CHORE_MAX, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Trash done", out);

    char zeroed[CHORE_MAX][CHORE_NAME_BUF];
    memset(zeroed, 0, sizeof(zeroed));
    for (int i = 0; i < CHORE_MAX; i++)
        TEST_ASSERT_EQUAL_INT(STATS_CHORE_DISC_RETIRE,
                              stats_json_chore_discovery(chore_row(i), zeroed, 0, out, sizeof(out)));
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
   their switches off overnight. def_ent_id pins the entity_id to the
   MAC-derived device id instead. The rule these tests hold is that EVERY
   discovery payload carries def_ent_id, and that its value is the
   entity's component, a dot, and its uniq_id.

   The field is def_ent_id and not obj_id because HA removed obj_id from
   MQTT discovery in 2026.4.0; def_ent_id has existed since 2025.10. */
void test_discovery_default_entity_id_is_the_component_and_unique_id(void) {
    char buf[STATS_JSON_PAYLOAD_MAX];
    char want[128];
    int count = 0;
    const ha_entity_t *ents = stats_json_entities(&count);
    TEST_ASSERT_TRUE(count > 0);
    for (int i = 0; i < count; i++) {
        stats_json_discovery(buf, sizeof(buf), "magtag-a1b2c3", "Kitchen MagTag", "fw", &ents[i]);
        snprintf(want, sizeof(want), "\"def_ent_id\":\"%s.magtag-a1b2c3_%s\"", ents[i].component, ents[i].key);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, want), ents[i].key);
        snprintf(want, sizeof(want), "\"uniq_id\":\"magtag-a1b2c3_%s\"", ents[i].key);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, want), ents[i].key);
        /* The payload's component must be the one the topic routes on,
           or HA registers the entity under one platform and names it for
           another. */
        char topic[128];
        stats_json_discovery_topic(topic, sizeof(topic), "magtag-a1b2c3", &ents[i]);
        snprintf(want, sizeof(want), "homeassistant/%s/", ents[i].component);
        TEST_ASSERT_EQUAL_STRING_LEN_MESSAGE(want, topic, strlen(want), ents[i].key);
    }
}

void test_discovery_default_entity_id_survives_the_ha_dot_partition(void) {
    /* HA does not use def_ent_id whole. It does
       `_, _, object_id = default_entity_id.partition(".")` and generates
       from the TAIL, and Python's partition returns an empty tail when
       there is no dot — so a value without the "<component>." prefix
       registers an EMPTY object id, which is worse than omitting the
       field. This walks every entity through that exact split. */
    char buf[STATS_JSON_PAYLOAD_MAX];
    int count = 0;
    const ha_entity_t *ents = stats_json_entities(&count);
    for (int i = 0; i < count; i++) {
        stats_json_discovery(buf, sizeof(buf), "magtag-a1b2c3", "Kitchen MagTag", "fw", &ents[i]);
        const char *v = strstr(buf, "\"def_ent_id\":\"");
        TEST_ASSERT_NOT_NULL_MESSAGE(v, ents[i].key);
        v += strlen("\"def_ent_id\":\"");
        const char *end = strchr(v, '"');
        TEST_ASSERT_NOT_NULL_MESSAGE(end, ents[i].key);
        const char *dot = strchr(v, '.');
        TEST_ASSERT_NOT_NULL_MESSAGE(dot, ents[i].key); /* dotless -> empty object id */
        TEST_ASSERT_TRUE_MESSAGE(dot < end, ents[i].key);
        char want[128];
        snprintf(want, sizeof(want), "magtag-a1b2c3_%s", ents[i].key);
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)strlen(want), (int)(end - dot - 1), ents[i].key);
        TEST_ASSERT_EQUAL_STRING_LEN_MESSAGE(want, dot + 1, strlen(want), ents[i].key);
    }
}

void test_discovery_default_entity_id_does_not_move_when_the_device_is_renamed(void) {
    /* The actual regression, stated as a test: two device names that HA
       would slug differently must produce the same def_ent_id. The name
       itself still changes, which is what stops this passing vacuously. */
    char a[STATS_JSON_PAYLOAD_MAX], b[STATS_JSON_PAYLOAD_MAX];
    int count = 0;
    const ha_entity_t *ents = stats_json_entities(&count);
    stats_json_discovery(a, sizeof(a), "magtag-a1b2c3", "Testing Timer", "fw", &ents[0]);
    stats_json_discovery(b, sizeof(b), "magtag-a1b2c3", "Julia's Timer", "fw", &ents[0]);

    char want[128];
    snprintf(want, sizeof(want), "\"def_ent_id\":\"%s.magtag-a1b2c3_%s\"", ents[0].component, ents[0].key);
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
       the transport carries, and the per-slot sensors' runtime name
       override at its own cap.

       fw is 31 characters, not the 2 this test used to pass. The device
       hands over esp_app_get_description()->version, which is char[32],
       and this project derives it from `git describe` — so the realistic
       input is a full tag+offset+hash string, not "fw". A 2-char stub
       understated the worst case by 29 bytes and would have kept
       understating it as tags grew. */
    char buf[STATS_JSON_PAYLOAD_MAX];
    char longname[64];
    memset(longname, 'W', sizeof(longname) - 1); /* widest glyph, 63 chars */
    longname[sizeof(longname) - 1] = '\0';
    char override[48];
    memset(override, 'W', sizeof(override) - 1);
    override[sizeof(override) - 1] = '\0';
    char fw[32]; /* == sizeof(esp_app_desc_t.version): 31 chars + NUL */
    memset(fw, 'W', sizeof(fw) - 1);
    fw[sizeof(fw) - 1] = '\0';

    int count = 0, worst = 0;
    const ha_entity_t *ents = stats_json_entities(&count);
    for (int i = 0; i < count; i++) {
        const int n = stats_json_discovery_named(buf, sizeof(buf), "magtag-a1b2c3", longname, fw, &ents[i], override);
        if (n > worst)
            worst = n;
        TEST_ASSERT_TRUE_MESSAGE(n < (int)sizeof(buf), ents[i].key);
    }
    /* Not an equality assert — this is a headroom report that fails only
       if the margin is gone. Kept loose so an entity can be added without
       editing a magic number, and tight enough to notice a doubling. */
    TEST_ASSERT_TRUE_MESSAGE(worst < (int)sizeof(buf) - 128, "discovery headroom below 128 B");
}

/* ---- act state (the Screen-adjust confirmation HA renders) ---------------
   The number entity's value comes from this payload, and `optimistic` does
   NOT protect the value the user typed: HA's MQTT number subscribes to
   state_topic regardless, so whatever lands here overwrites the box. The
   apply is deferred to net_apply_finish(), one join AFTER this payload is
   built, so reporting the snapshot's bonus_applied here is what snapped a
   fresh -45 back to 0 on the very sync that applied it. */

void test_act_reports_the_target_arriving_this_window(void) {
    char buf[96];
    act_state_t a = {.applied_s = 0, .target_s = -2700, .target_pending = true};
    int n = stats_json_act(buf, sizeof(buf), &a);
    TEST_ASSERT_TRUE(n < (int)sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("{\"screen_bonus\":-45,\"locate\":\"OFF\"}", buf);
}

void test_act_reports_the_applied_value_when_nothing_arrived(void) {
    /* Steady state: no set this window, so the confirmed figure stands
       and a replayed retained target reads back unchanged. */
    char buf[96];
    act_state_t a = {.applied_s = -2700, .target_pending = false};
    stats_json_act(buf, sizeof(buf), &a);
    TEST_ASSERT_EQUAL_STRING("{\"screen_bonus\":-45,\"locate\":\"OFF\"}", buf);
}

void test_act_rollover_clear_outranks_a_stale_retained_target(void) {
    /* The rollover window still receives yesterday's retained set and
       still carries yesterday's applied figure in its snapshot, but the
       day resets the moment the window closes: HA must be told 0, not
       yesterday's number. */
    char buf[96];
    act_state_t a = {.applied_s = -2700, .target_s = -2700, .target_pending = true, .day_cleared = true};
    stats_json_act(buf, sizeof(buf), &a);
    TEST_ASSERT_EQUAL_STRING("{\"screen_bonus\":0,\"locate\":\"OFF\"}", buf);
}

void test_act_truncation_is_reported_not_written_past(void) {
    char buf[8] = {0};
    act_state_t a = {.applied_s = 900, .target_pending = false};
    int n = stats_json_act(buf, sizeof(buf), &a);
    TEST_ASSERT_TRUE(n >= (int)sizeof(buf));
    TEST_ASSERT_EQUAL_CHAR('\0', buf[sizeof(buf) - 1]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_act_reports_the_target_arriving_this_window);
    RUN_TEST(test_act_reports_the_applied_value_when_nothing_arrived);
    RUN_TEST(test_act_rollover_clear_outranks_a_stale_retained_target);
    RUN_TEST(test_act_truncation_is_reported_not_written_past);
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
    RUN_TEST(test_summary_payload_chore_fields_are_not_swapped);
    RUN_TEST(test_summary_payload_omits_the_chore_fields_when_the_list_is_unknown);
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
    RUN_TEST(test_discovery_default_entity_id_is_the_component_and_unique_id);
    RUN_TEST(test_discovery_default_entity_id_survives_the_ha_dot_partition);
    RUN_TEST(test_discovery_default_entity_id_does_not_move_when_the_device_is_renamed);
    RUN_TEST(test_every_discovery_payload_fits_the_publish_buffer);
    RUN_TEST(test_discovery_binary_sensor_has_payload_states);
    RUN_TEST(test_discovery_completions_use_runtime_slot_names);
    RUN_TEST(test_stat_payload_reports_a_running_break);
    RUN_TEST(test_discovery_screen_break_entities);
    RUN_TEST(test_screen_exposure_entity);
    RUN_TEST(test_break_entities_are_not_mistaken_for_per_slot_sensors);

    RUN_TEST(test_stat_payload_carries_the_chore_counts_and_acks);
    RUN_TEST(test_stat_payload_chore_counts_are_not_interchangeable);
    RUN_TEST(test_stat_payload_chore_ack_ignores_bits_past_the_configured_count);
    RUN_TEST(test_stat_payload_no_chores_reports_zeroes);
    RUN_TEST(test_stat_payload_config_warning_is_ok_when_healthy);
    RUN_TEST(test_stat_payload_config_warning_names_each_day_type);
    RUN_TEST(test_stat_payload_config_warning_names_several_day_types);
    RUN_TEST(test_stat_payload_config_warning_ignores_bits_past_the_day_types);
    RUN_TEST(test_discovery_chore_count_entities);
    RUN_TEST(test_discovery_one_binary_sensor_per_chore);
    RUN_TEST(test_discovery_chore_sensor_takes_the_runtime_chore_name);
    RUN_TEST(test_chore_index_matches_only_the_per_chore_rows);
    RUN_TEST(test_chore_entities_are_not_mistaken_for_per_slot_sensors);
    RUN_TEST(test_discovery_config_warning_entity);
    RUN_TEST(test_only_the_graphed_rows_declare_a_state_class);
    RUN_TEST(test_only_the_summary_rows_carry_a_last_reset);
    RUN_TEST(test_discovery_completions_keep_no_state_class);
    RUN_TEST(test_discovery_screen_used_day_payload);
    RUN_TEST(test_discovery_day_runs_payloads);
    RUN_TEST(test_summary_rows_read_fields_the_summary_writes);
    RUN_TEST(test_screen_used_day_key_avoids_the_retired_and_per_slot_keys);
    RUN_TEST(test_discovery_day_chores_payload);
    RUN_TEST(test_day_chores_key_avoids_the_per_slot_chore_and_retired_keys);
    RUN_TEST(test_slot_of_matches_only_the_per_slot_rows);
    RUN_TEST(test_chore_discovery_names_configured_rows_and_retires_the_rest);
    RUN_TEST(test_chore_discovery_skips_every_chore_row_when_the_list_is_unknown);
    RUN_TEST(test_chore_discovery_leaves_every_other_row_to_the_caller);
    RUN_TEST(test_chore_discovery_longest_name_fits_its_buffer);
    RUN_TEST(test_chore_discovery_empty_rows);
    return UNITY_END();
}
