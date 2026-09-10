#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unity.h>

/* Single-TU compilation: mocks, then the modules under test */
// clang-format off
#include "mock_hal_time.c"
#include "mock_hal_nvs.c"
#include "../../main/timer.c"
#include "../../main/schedule.c"
#include "../../main/button_actions.c"
// clang-format on

/* 2026-01-05 00:00:00 UTC (Monday) */
#define T0 ((time_t)1767571200)

static const timer_def_t TEST_DEFS[TIMER_SLOT_COUNT] = {
    {"Screen", 0, false, false},   {"Piano", 900, true, true},   {"", 0, false, false}, /* disabled */
    {"Laundry", 600, true, false}, {"Violin", 900, false, true},
};

void setUp(void) {
    mock_nvs_reset();
    schedule_cache_invalidate();
    setenv("TZ", "UTC0", 1);
    tzset();
    hal_nvs_write_u16("weekday_min", 60);
    timer_set_defs(TEST_DEFS, TIMER_SLOT_COUNT);
    timer_reset();
    mock_time_set(T0);
}

void tearDown(void) {}

void test_idle_screen_starts_with_schedule_allocation(void) {
    TEST_ASSERT_EQUAL(BTN_B_STARTED, button_b_apply(T0));
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
    TEST_ASSERT_EQUAL_INT32(3600, g_rtc_state.slots[0].allocation_sec);
}

void test_idle_extra_slot_starts_with_def_duration(void) {
    g_rtc_state.active_slot = 1; /* Piano, 900 s */
    TEST_ASSERT_EQUAL(BTN_B_STARTED, button_b_apply(T0));
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
    TEST_ASSERT_EQUAL_INT32(900, g_rtc_state.slots[1].allocation_sec);
}

void test_running_pauses(void) {
    timer_start(T0, 3600);
    TEST_ASSERT_EQUAL(BTN_B_PAUSED, button_b_apply(T0 + 60));
    TEST_ASSERT_EQUAL(TIMER_PAUSED, timer_get_state());
}

void test_paused_resumes(void) {
    timer_start(T0, 3600);
    timer_pause(T0 + 60);
    TEST_ASSERT_EQUAL(BTN_B_RESUMED, button_b_apply(T0 + 120));
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
}

void test_break_is_none_and_stays_break(void) {
    timer_start(T0, 3600);
    timer_start_break(T0 + 60, 300);
    TEST_ASSERT_EQUAL(BTN_B_NONE, button_b_apply(T0 + 90));
    TEST_ASSERT_EQUAL(TIMER_BREAK, timer_get_state());
}

/* Screen (slot 0) carries no def, so timer_reload_allowed() is false for
   it and the EXPIRED->reload leg can never fire: a kid cannot reset their
   own screen timer. This is the property PARENT_TESTING=n used to protect
   with a build flag; it is now structural. */
void test_expired_screen_never_reloads(void) {
    timer_start(T0, 60);
    timer_tick(T0 + 120); /* RUNNING -> EXPIRED */
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
    TEST_ASSERT_FALSE(timer_reload_allowed());
    TEST_ASSERT_EQUAL(BTN_B_NONE, button_b_apply(T0 + 130));
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
}

/* EXPIRED is the one state where B has no start/pause/resume job, so it
   is where Reload lives (design 2.3). */
void test_expired_reloadable_extra_reloads_to_full_duration(void) {
    g_rtc_state.active_slot = 1; /* Piano, 900 s, reloadable */
    timer_start(T0, 60);
    timer_tick(T0 + 120); /* RUNNING -> EXPIRED */
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
    TEST_ASSERT_EQUAL(BTN_B_RELOADED, button_b_apply(T0 + 130));
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state());
    /* "Full duration" is the def's, realized at the next start — the same
       press again now starts a whole fresh 900 s Piano. */
    TEST_ASSERT_EQUAL(BTN_B_STARTED, button_b_apply(T0 + 140));
    TEST_ASSERT_EQUAL_INT32(900, g_rtc_state.slots[1].allocation_sec);
}

void test_expired_non_reloadable_extra_is_none(void) {
    g_rtc_state.active_slot = 4; /* Violin, 900 s, NOT reloadable */
    timer_start(T0, 60);
    timer_tick(T0 + 120); /* RUNNING -> EXPIRED */
    TEST_ASSERT_EQUAL(BTN_B_NONE, button_b_apply(T0 + 130));
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
    TEST_ASSERT_EQUAL_INT32(60, g_rtc_state.slots[4].allocation_sec); /* untouched */
}

/* Ordering: the reload leg is evaluated BEFORE the timer_start_allowed()
   break gate, because a reload is not a start. Laundry is reloadable but
   NOT break-eligible, so the gate would refuse it — and today's direct
   wake_flow dispatch, gated only by timer_reload_allowed(), reloads it
   anyway. That behaviour has to survive the remap. */
void test_expired_reloadable_extra_reloads_during_a_screen_break(void) {
    timer_start(T0, 3600);           /* Screen RUNNING */
    timer_start_break(T0 + 60, 900); /* Screen Break, ends T0+960 */
    g_rtc_state.active_slot = 3;     /* Laundry, 600 s, reloadable, not break-eligible */
    timer_start(T0 + 70, 60);
    timer_tick(T0 + 200); /* RUNNING -> EXPIRED */
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
    TEST_ASSERT_FALSE(timer_start_allowed()); /* the start gate WOULD refuse */
    TEST_ASSERT_EQUAL(BTN_B_RELOADED, button_b_apply(T0 + 210));
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state());
    TEST_ASSERT_EQUAL(TIMER_BREAK, timer_slot_state(0)); /* the break is untouched */
}

void test_start_allocation_prefers_def_duration(void) {
    g_rtc_state.active_slot = 3; /* Laundry, 600 s */
    TEST_ASSERT_EQUAL_INT32(600, button_b_start_allocation(T0));
    g_rtc_state.active_slot = 0; /* Screen: schedule-fed */
    TEST_ASSERT_EQUAL_INT32(3600, button_b_start_allocation(T0));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_idle_screen_starts_with_schedule_allocation);
    RUN_TEST(test_idle_extra_slot_starts_with_def_duration);
    RUN_TEST(test_running_pauses);
    RUN_TEST(test_paused_resumes);
    RUN_TEST(test_break_is_none_and_stays_break);
    RUN_TEST(test_expired_screen_never_reloads);
    RUN_TEST(test_expired_reloadable_extra_reloads_to_full_duration);
    RUN_TEST(test_expired_non_reloadable_extra_is_none);
    RUN_TEST(test_expired_reloadable_extra_reloads_during_a_screen_break);
    RUN_TEST(test_start_allocation_prefers_def_duration);
    return UNITY_END();
}
