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
    {"Screen", 0, false},      {"Piano", 900, true},   {"", 0, false}, /* disabled */
    {"Meditation", 600, true}, {"Violin", 900, false},
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
    TEST_ASSERT_EQUAL(BTN_A_STARTED, button_a_apply(T0));
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
    TEST_ASSERT_EQUAL_INT32(3600, g_rtc_state.slots[0].allocation_sec);
}

void test_idle_extra_slot_starts_with_def_duration(void) {
    g_rtc_state.active_slot = 1; /* Piano, 900 s */
    TEST_ASSERT_EQUAL(BTN_A_STARTED, button_a_apply(T0));
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
    TEST_ASSERT_EQUAL_INT32(900, g_rtc_state.slots[1].allocation_sec);
}

void test_running_pauses(void) {
    timer_start(T0, 3600);
    TEST_ASSERT_EQUAL(BTN_A_PAUSED, button_a_apply(T0 + 60));
    TEST_ASSERT_EQUAL(TIMER_PAUSED, timer_get_state());
}

void test_paused_resumes(void) {
    timer_start(T0, 3600);
    timer_pause(T0 + 60);
    TEST_ASSERT_EQUAL(BTN_A_RESUMED, button_a_apply(T0 + 120));
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
}

void test_break_is_none_and_stays_break(void) {
    timer_start(T0, 3600);
    timer_start_break(T0 + 60, 300);
    TEST_ASSERT_EQUAL(BTN_A_NONE, button_a_apply(T0 + 90));
    TEST_ASSERT_EQUAL(TIMER_BREAK, timer_get_state());
}

void test_expired_is_none(void) {
    timer_start(T0, 60);
    timer_tick(T0 + 120); /* RUNNING -> EXPIRED */
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
    TEST_ASSERT_EQUAL(BTN_A_NONE, button_a_apply(T0 + 130));
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
}

void test_start_allocation_prefers_def_duration(void) {
    g_rtc_state.active_slot = 3; /* Meditation, 600 s */
    TEST_ASSERT_EQUAL_INT32(600, button_a_start_allocation(T0));
    g_rtc_state.active_slot = 0; /* Screen: schedule-fed */
    TEST_ASSERT_EQUAL_INT32(3600, button_a_start_allocation(T0));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_idle_screen_starts_with_schedule_allocation);
    RUN_TEST(test_idle_extra_slot_starts_with_def_duration);
    RUN_TEST(test_running_pauses);
    RUN_TEST(test_paused_resumes);
    RUN_TEST(test_break_is_none_and_stays_break);
    RUN_TEST(test_expired_is_none);
    RUN_TEST(test_start_allocation_prefers_def_duration);
    return UNITY_END();
}
