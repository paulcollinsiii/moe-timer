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

/* RUNNING/BREAK align to the COUNTDOWN's minute grid, not the wall clock:
   wake when event_remaining hits a round minute so the displayed value is
   truly 1:11:00 (user flow: resume shows 1:12:23 precise, next render
   1:12:00, then 1:11:00...). sec_into_minute is ignored for these. */

void test_running_aligns_to_countdown_grid(void) {
    /* remaining 1:12:23 -> the round value is 23 s away */
    TEST_ASSERT_EQUAL_INT32(23, plan(TIMER_RUNNING, 17, 4343, false));
    /* already on the grid -> full minute */
    TEST_ASSERT_EQUAL_INT32(60, plan(TIMER_RUNNING, 17, 3600, false));
    /* grid point too close -> take the following one */
    TEST_ASSERT_EQUAL_INT32(63, plan(TIMER_RUNNING, 17, 3603, false));
}

void test_running_sync_due_wakes_early_on_countdown_grid(void) {
    /* remaining X:XX:40 -> grid in 40 s, minus 20 s sync lead */
    TEST_ASSERT_EQUAL_INT32(20, plan(TIMER_RUNNING, 17, 3640, true));
    /* lead would leave <5 s (23-20=3): take the previous grid minute —
       wake at remaining 3560, i.e. exactly lead(20) before the 3540 grid */
    TEST_ASSERT_EQUAL_INT32(63, plan(TIMER_RUNNING, 17, 3623, true)); /* 23-20+60 */
    /* on-grid with sync: 60-20 */
    TEST_ASSERT_EQUAL_INT32(40, plan(TIMER_RUNNING, 17, 3600, true));
}

void test_event_wake_lands_before_expiry(void) {
    /* expiry in 100 s: event lead (100-70=30) beats the grid sleep (40) */
    TEST_ASSERT_EQUAL_INT32(30, plan(TIMER_RUNNING, 17, 100, false));
    /* grid wins when it is sooner: remaining 130 -> grid in 10, event in 60 */
    TEST_ASSERT_EQUAL_INT32(10, plan(TIMER_RUNNING, 17, 130, false));
}

void test_final_countdown_handoff_sequence(void) {
    /* corner: successive planner calls walk cleanly into the watch window.
       rem 190 -> grid 10 (wake at 180 = 3:00) */
    TEST_ASSERT_EQUAL_INT32(10, plan(TIMER_RUNNING, 0, 190, false));
    /* rem 180 (just rendered 3:00) -> grid 60 beats event 110 */
    TEST_ASSERT_EQUAL_INT32(60, plan(TIMER_RUNNING, 0, 180, false));
    /* rem 120 (rendered 2:00) -> event 50 beats grid 60: wake at 70 */
    TEST_ASSERT_EQUAL_INT32(50, plan(TIMER_RUNNING, 0, 120, false));
    /* rem 90 -> event 20 beats grid 30: wake at 70, watch shows the 60 step */
    TEST_ASSERT_EQUAL_INT32(20, plan(TIMER_RUNNING, 0, 90, false));
}

void test_event_wake_clamps_when_event_imminent(void) {
    TEST_ASSERT_EQUAL_INT32(5, plan(TIMER_RUNNING, 17, 72, false)); /* 72-70=2 -> 5 */
}

void test_break_aligns_to_break_grid_no_sync_lead(void) {
    TEST_ASSERT_EQUAL_INT32(20, plan(TIMER_BREAK, 17, 500, false)); /* 8:20 -> 8:00 */
    TEST_ASSERT_EQUAL_INT32(30, plan(TIMER_BREAK, 17, 100, false)); /* event lead wins */
    TEST_ASSERT_EQUAL_INT32(60, plan(TIMER_BREAK, 17, 900, false)); /* on-grid */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_clock_states_align_to_minute_boundary);
    RUN_TEST(test_boundary_too_close_takes_following_minute);
    RUN_TEST(test_running_aligns_to_countdown_grid);
    RUN_TEST(test_running_sync_due_wakes_early_on_countdown_grid);
    RUN_TEST(test_event_wake_lands_before_expiry);
    RUN_TEST(test_final_countdown_handoff_sequence);
    RUN_TEST(test_event_wake_clamps_when_event_imminent);
    RUN_TEST(test_break_aligns_to_break_grid_no_sync_lead);
    return UNITY_END();
}
