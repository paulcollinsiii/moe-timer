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
        .break_remaining_sec = 0,
    };
    return sleep_plan_seconds(&in);
}

/* Same, with the optional secondary event: a background break running
   behind another state (main.c populates it only when the break end will
   actually chime). */
static int32_t plan_brk(timer_state_t st, int sec, int32_t event_rem, int32_t break_rem) {
    sleep_plan_in_t in = {
        .state = st,
        .sec_into_minute = sec,
        .event_remaining_sec = event_rem,
        .sync_due_by_next_wake = false,
        .break_remaining_sec = break_rem,
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

/* ---- secondary event: a Screen Break running behind another state ----
   Slot 0 can hold a break while an extra timer is selected, so a wake may
   have TWO future events. main.c fills break_remaining_sec only when the
   break end will chime (nothing RUNNING); a suppressed end needs no wake
   and lands at whatever the next tick wake is. */

void test_break_secondary_pulls_the_wake_in(void) {
    /* Piano PAUSED (wall grid: 43 s) but the break ends in 100 s: wake at
       100-70=30 so the awake watch owns the chime. */
    TEST_ASSERT_EQUAL_INT32(30, plan_brk(TIMER_PAUSED, 17, 0, 100));
    /* IDLE too — the state does not matter, the break end does */
    TEST_ASSERT_EQUAL_INT32(30, plan_brk(TIMER_IDLE, 17, 0, 100));
}

void test_break_secondary_ignored_when_later_than_the_primary(void) {
    /* Break end 10 min out: the ordinary wall-grid wake is sooner */
    TEST_ASSERT_EQUAL_INT32(43, plan_brk(TIMER_PAUSED, 17, 0, 600));
}

void test_break_secondary_ignored_when_zero(void) {
    /* No break (or a suppressed one): today's plan, unchanged */
    TEST_ASSERT_EQUAL_INT32(43, plan_brk(TIMER_PAUSED, 17, 0, 0));
    TEST_ASSERT_EQUAL_INT32(43, plan_brk(TIMER_IDLE, 17, 0, -5));
}

void test_break_secondary_never_naps_below_the_minimum(void) {
    /* Break end already inside the lead: clamp, never 0 or negative */
    TEST_ASSERT_EQUAL_INT32(SLEEP_PLAN_MIN_SEC, plan_brk(TIMER_PAUSED, 17, 0, 72));
    TEST_ASSERT_EQUAL_INT32(SLEEP_PLAN_MIN_SEC, plan_brk(TIMER_PAUSED, 17, 0, 1));
}

void test_break_secondary_can_beat_a_running_expiry(void) {
    /* Screen paused behind... no: RUNNING extra with a chiming break is
       impossible (a RUNNING extra suppresses the chime), but the planner
       must still take the sooner of the two if main.c ever passes both. */
    TEST_ASSERT_EQUAL_INT32(20, plan_brk(TIMER_RUNNING, 0, 300, 90));
    TEST_ASSERT_EQUAL_INT32(30, plan_brk(TIMER_RUNNING, 0, 100, 600));
}

void test_break_as_primary_state_is_unchanged(void) {
    /* Screen selected during its own break: BREAK is the state and the
       break end is the PRIMARY event — the secondary field stays 0 and
       today's cases must not move. */
    TEST_ASSERT_EQUAL_INT32(20, plan(TIMER_BREAK, 17, 500, false));
    TEST_ASSERT_EQUAL_INT32(30, plan(TIMER_BREAK, 17, 100, false));
    TEST_ASSERT_EQUAL_INT32(60, plan(TIMER_BREAK, 17, 900, false));
}

/* A break becomes DUE (rather than ending) while a non-eligible extra
   runs — e.g. Laundry 60 min, balance crossing at 30. That moment needs
   no dedicated wake event: a RUNNING slot always sleeps on the countdown
   minute grid, so it is capped at 60 s whatever the expiry horizon, and
   the per-wake maybe_start_break check catches the crossing within a
   minute. Exactly the fidelity the Screen timer has always had.

   This is a CONTRACT, not an observation: raise the RUNNING cap above a
   minute and a laundry-driven break silently starts arriving late.

   The bound is 64 s, not 60: when the next grid point (or the grid point
   less the sync lead) falls inside SLEEP_PLAN_MIN_SEC the planner takes
   the PREVIOUS grid minute instead, which can add up to MIN_SEC-1. */
#define RUNNING_SLEEP_MAX (60 + SLEEP_PLAN_MIN_SEC - 1)
void test_running_never_sleeps_past_the_minute_grid(void) {
    for (int32_t rem = 61; rem <= 7200; rem++) {
        TEST_ASSERT_TRUE_MESSAGE(plan(TIMER_RUNNING, 17, rem, false) <= RUNNING_SLEEP_MAX,
                                 "RUNNING slept past a minute");
        TEST_ASSERT_TRUE_MESSAGE(plan(TIMER_RUNNING, 17, rem, true) <= RUNNING_SLEEP_MAX,
                                 "RUNNING slept past a minute (sync due)");
    }
}

/* The balance is frozen unless something is RUNNING, so the clock-only
   states cannot cross the interval while asleep however long they nap —
   and they are minute-aligned anyway. */
void test_clock_only_states_stay_minute_aligned(void) {
    for (int sec = 0; sec < 60; sec++) {
        TEST_ASSERT_TRUE(plan(TIMER_IDLE, sec, 0, false) <= RUNNING_SLEEP_MAX);
        TEST_ASSERT_TRUE(plan(TIMER_PAUSED, sec, 0, false) <= RUNNING_SLEEP_MAX);
        TEST_ASSERT_TRUE(plan(TIMER_EXPIRED, sec, 0, false) <= RUNNING_SLEEP_MAX);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_running_never_sleeps_past_the_minute_grid);
    RUN_TEST(test_clock_only_states_stay_minute_aligned);
    RUN_TEST(test_break_secondary_pulls_the_wake_in);
    RUN_TEST(test_break_secondary_ignored_when_later_than_the_primary);
    RUN_TEST(test_break_secondary_ignored_when_zero);
    RUN_TEST(test_break_secondary_never_naps_below_the_minimum);
    RUN_TEST(test_break_secondary_can_beat_a_running_expiry);
    RUN_TEST(test_break_as_primary_state_is_unchanged);
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
