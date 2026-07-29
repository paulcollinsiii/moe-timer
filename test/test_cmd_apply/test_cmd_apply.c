#include <string.h>
#include <unity.h>

/* Single-TU: cJSON + the command parser over mock NVS (dedup store) and
   the real timer (name->slot resolution + defs). */
// clang-format off
#include "cJSON.h"
#include "mock_hal_nvs.c"
#include "mock_hal_time.c"
#include "../../main/timer.c"
#include "../../main/nvs_config.c"
#include "../../main/cmd_apply.c"
// clang-format on

static const timer_def_t DEFS[TIMER_SLOT_COUNT] = {
    {"Screen", 0, false, false},     {"Piano", 900, true, true}, {"", 0, false, false},
    {"Meditation", 600, true, true}, {"", 0, false, false},
};

void setUp(void) {
    mock_nvs_reset();
    timer_set_defs(DEFS, TIMER_SLOT_COUNT);
    timer_reset();
}
void tearDown(void) {}

/* ---- grant parsing ---- */

void test_grant_default_screen(void) {
    cmd_action_t a;
    char ack[128];
    TEST_ASSERT_EQUAL(CMD_GRANT, cmd_apply("{\"id\":\"x1\",\"grant\":{\"min\":15}}", &a, ack, sizeof(ack)));
    TEST_ASSERT_EQUAL_INT(0, a.slot); /* omitted timer = Screen */
    TEST_ASSERT_EQUAL_INT32(900, a.sec);
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"ok\":true"));
}

void test_grant_named_timer(void) {
    cmd_action_t a;
    char ack[128];
    TEST_ASSERT_EQUAL(
        CMD_GRANT, cmd_apply("{\"id\":\"x1\",\"grant\":{\"timer\":\"Meditation\",\"min\":5}}", &a, ack, sizeof(ack)));
    TEST_ASSERT_EQUAL_INT(3, a.slot);
    TEST_ASSERT_EQUAL_INT32(300, a.sec);
}

void test_grant_unknown_timer_rejected(void) {
    cmd_action_t a;
    char ack[128];
    TEST_ASSERT_EQUAL(CMD_INVALID,
                      cmd_apply("{\"id\":\"x1\",\"grant\":{\"timer\":\"Guitar\",\"min\":5}}", &a, ack, sizeof(ack)));
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"ok\":false"));
}

void test_grant_out_of_range_rejected(void) {
    cmd_action_t a;
    char ack[128];
    TEST_ASSERT_EQUAL(CMD_INVALID, cmd_apply("{\"id\":\"x1\",\"grant\":{\"min\":0}}", &a, ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(CMD_INVALID, cmd_apply("{\"id\":\"x2\",\"grant\":{\"min\":9999}}", &a, ack, sizeof(ack)));
}

void test_grant_boundary_minutes_accepted(void) {
    cmd_action_t a;
    char ack[128];
    TEST_ASSERT_EQUAL(CMD_GRANT, cmd_apply("{\"id\":\"lo\",\"grant\":{\"min\":1}}", &a, ack, sizeof(ack)));
    TEST_ASSERT_EQUAL_INT32(60, a.sec);
    TEST_ASSERT_EQUAL(CMD_GRANT, cmd_apply("{\"id\":\"hi\",\"grant\":{\"min\":240}}", &a, ack, sizeof(ack)));
    TEST_ASSERT_EQUAL_INT32(14400, a.sec);
}

void test_grant_non_object_rejected(void) {
    cmd_action_t a;
    char ack[128];
    /* "grant":15 — not an object; the min lookup finds nothing → reject */
    TEST_ASSERT_EQUAL(CMD_INVALID, cmd_apply("{\"id\":\"x\",\"grant\":15}", &a, ack, sizeof(ack)));
}

void test_grant_wins_when_both_present(void) {
    cmd_action_t a;
    char ack[128];
    /* Documented precedence: grant is handled before locate */
    TEST_ASSERT_EQUAL(CMD_GRANT,
                      cmd_apply("{\"id\":\"x\",\"grant\":{\"min\":15},\"locate\":true}", &a, ack, sizeof(ack)));
}

/* ---- locate ---- */

void test_locate(void) {
    cmd_action_t a;
    char ack[128];
    TEST_ASSERT_EQUAL(CMD_LOCATE, cmd_apply("{\"id\":\"x1\",\"locate\":true}", &a, ack, sizeof(ack)));
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"ok\":true"));
}

/* ---- dedupe (apply-once) ---- */

void test_duplicate_id_skipped(void) {
    cmd_action_t a;
    char ack[128];
    TEST_ASSERT_EQUAL(CMD_GRANT, cmd_apply("{\"id\":\"abc\",\"grant\":{\"min\":15}}", &a, ack, sizeof(ack)));
    /* Same id again (retained message re-delivered next window) → skip */
    TEST_ASSERT_EQUAL(CMD_DUP, cmd_apply("{\"id\":\"abc\",\"grant\":{\"min\":15}}", &a, ack, sizeof(ack)));
    /* New id → applied again */
    TEST_ASSERT_EQUAL(CMD_GRANT, cmd_apply("{\"id\":\"def\",\"grant\":{\"min\":15}}", &a, ack, sizeof(ack)));
}

/* ---- malformed ---- */

void test_missing_id_invalid(void) {
    cmd_action_t a;
    char ack[128];
    TEST_ASSERT_EQUAL(CMD_INVALID, cmd_apply("{\"grant\":{\"min\":15}}", &a, ack, sizeof(ack)));
}

void test_malformed_json_invalid(void) {
    cmd_action_t a;
    char ack[128];
    TEST_ASSERT_EQUAL(CMD_INVALID, cmd_apply("not json", &a, ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(CMD_INVALID, cmd_apply("", &a, ack, sizeof(ack)));
}

void test_empty_retained_payload_is_noop(void) {
    /* The device clears the cmd topic by publishing empty retained; a
       delivery of that must be a clean no-op, not an error. */
    cmd_action_t a;
    char ack[128];
    TEST_ASSERT_EQUAL(CMD_NONE, cmd_apply("{}", &a, ack, sizeof(ack)));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_grant_default_screen);
    RUN_TEST(test_grant_named_timer);
    RUN_TEST(test_grant_unknown_timer_rejected);
    RUN_TEST(test_grant_out_of_range_rejected);
    RUN_TEST(test_grant_boundary_minutes_accepted);
    RUN_TEST(test_grant_non_object_rejected);
    RUN_TEST(test_grant_wins_when_both_present);
    RUN_TEST(test_locate);
    RUN_TEST(test_duplicate_id_skipped);
    RUN_TEST(test_missing_id_invalid);
    RUN_TEST(test_malformed_json_invalid);
    RUN_TEST(test_empty_retained_payload_is_noop);
    return UNITY_END();
}
