#include <unity.h>

/* Single-TU compilation of the pure sleep planner */
#include "../../main/sleep_plan.c"

void setUp(void) {}
void tearDown(void) {}

static int32_t plan(timer_state_t st, int sec, int32_t event_rem, bool sync_due) {
    sleep_plan_in_t in = {
        .state = st,
        .sec_into_minute = sec,
        .event_remaining_sec = event_rem,
        .sync_due_by_next_wake = sync_due,
    };
    return sleep_plan_seconds(&in);
}

void test_clock_states_align_to_minute_boundary(void) {
    TEST_ASSERT_EQUAL_INT32(43, plan(TIMER_IDLE, 17, 0, false));
    TEST_ASSERT_EQUAL_INT32(60, plan(TIMER_IDLE, 0, 0, false));
    TEST_ASSERT_EQUAL_INT32(43, plan(TIMER_PAUSED, 17, 0, false));
    TEST_ASSERT_EQUAL_INT32(43, plan(TIMER_EXPIRED, 17, 0, false));
}

void test_boundary_too_close_takes_following_minute(void) {
    TEST_ASSERT_EQUAL_INT32(63, plan(TIMER_IDLE, 57, 0, false)); /* 3 < MIN -> +60 */
    TEST_ASSERT_EQUAL_INT32(5, plan(TIMER_IDLE, 55, 0, false));  /* exactly MIN stays */
}

void test_running_aligns_when_no_sync_and_expiry_far(void) {
    TEST_ASSERT_EQUAL_INT32(43, plan(TIMER_RUNNING, 17, 3600, false));
}

void test_running_sync_due_wakes_early(void) {
    TEST_ASSERT_EQUAL_INT32(23, plan(TIMER_RUNNING, 17, 3600, true)); /* 43 - 20 */
    TEST_ASSERT_EQUAL_INT32(44, plan(TIMER_RUNNING, 56, 3600, true)); /* 64 - 20 */
    TEST_ASSERT_EQUAL_INT32(42, plan(TIMER_RUNNING, 58, 3600, true)); /* 62 - 20 */
}

void test_running_sync_lead_clamps_to_min(void) {
    /* base 20 - lead 20 = 0 -> clamp */
    TEST_ASSERT_EQUAL_INT32(5, plan(TIMER_RUNNING, 40, 3600, true));
}

void test_event_wake_lands_before_expiry(void) {
    /* expiry in 100 s: 100 - 70 = 30 beats the 43 s boundary sleep */
    TEST_ASSERT_EQUAL_INT32(30, plan(TIMER_RUNNING, 17, 100, false));
    /* boundary still wins when it is sooner than the event lead */
    TEST_ASSERT_EQUAL_INT32(43, plan(TIMER_RUNNING, 17, 200, false)); /* 130 > 43 */
}

void test_event_wake_clamps_when_event_imminent(void) {
    TEST_ASSERT_EQUAL_INT32(5, plan(TIMER_RUNNING, 17, 72, false)); /* 72-70=2 -> 5 */
}

void test_break_event_wake(void) {
    TEST_ASSERT_EQUAL_INT32(30, plan(TIMER_BREAK, 17, 100, false));
    TEST_ASSERT_EQUAL_INT32(43, plan(TIMER_BREAK, 17, 900, false)); /* far: align */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_clock_states_align_to_minute_boundary);
    RUN_TEST(test_boundary_too_close_takes_following_minute);
    RUN_TEST(test_running_aligns_when_no_sync_and_expiry_far);
    RUN_TEST(test_running_sync_due_wakes_early);
    RUN_TEST(test_running_sync_lead_clamps_to_min);
    RUN_TEST(test_event_wake_lands_before_expiry);
    RUN_TEST(test_event_wake_clamps_when_event_imminent);
    RUN_TEST(test_break_event_wake);
    return UNITY_END();
}
