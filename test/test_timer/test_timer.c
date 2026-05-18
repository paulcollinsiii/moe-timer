#include <string.h>
#include <time.h>
#include <unity.h>

/* Single-TU compilation */
#include "../../src/timer.c"
#include "mock_hal_time.c"

/* Base timestamp: 2026-01-05 00:00:00 UTC (Monday) */
#define T0 ((time_t)1767571200)

void setUp(void) {
    timer_reset();
    mock_time_set(T0);
}

void tearDown(void) {}

void test_reset_state_is_idle(void) {
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state());
}

void test_reset_expiry_is_zero(void) {
    TEST_ASSERT_EQUAL_INT64(0, g_rtc_state.expiry_wall_time);
}

void test_reset_remaining_at_pause_is_zero(void) {
    TEST_ASSERT_EQUAL_INT32(0, g_rtc_state.remaining_at_pause);
}

void test_start_sets_state_running(void) {
    timer_start(T0, 3600);
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
}

void test_start_sets_expiry_wall_time(void) {
    timer_start(T0, 3600);
    TEST_ASSERT_EQUAL_INT64((int64_t)T0 + 3600, g_rtc_state.expiry_wall_time);
}

void test_start_sets_allocation_sec(void) {
    timer_start(T0, 3600);
    TEST_ASSERT_EQUAL_INT32(3600, g_rtc_state.allocation_sec);
}

void test_tick_does_not_modify_expiry_wall_time(void) {
    timer_start(T0, 3600);
    int64_t expiry_before = g_rtc_state.expiry_wall_time;

    mock_time_set(T0 + 600);
    timer_tick(T0 + 600);
    mock_time_set(T0 + 1200);
    timer_tick(T0 + 1200);

    TEST_ASSERT_EQUAL_INT64(expiry_before, g_rtc_state.expiry_wall_time);
}

void test_tick_returns_correct_remaining_seconds(void) {
    timer_start(T0, 3600);
    int32_t remaining = timer_tick(T0 + 1000);
    TEST_ASSERT_EQUAL_INT32(2600, remaining);
}

void test_tick_returns_remaining_near_zero(void) {
    timer_start(T0, 3600);
    int32_t remaining = timer_tick(T0 + 3599);
    TEST_ASSERT_EQUAL_INT32(1, remaining);
}

void test_tick_transitions_to_expired_when_time_elapsed(void) {
    timer_start(T0, 3600);
    timer_tick(T0 + 3601);
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
}

void test_tick_returns_non_positive_when_expired(void) {
    timer_start(T0, 3600);
    int32_t remaining = timer_tick(T0 + 3601);
    // cppcheck-suppress knownConditionTrueFalse
    TEST_ASSERT_TRUE(remaining <= 0);
}

void test_tick_at_exact_expiry_transitions(void) {
    timer_start(T0, 3600);
    timer_tick(T0 + 3600);
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
}

void test_pause_sets_state_paused(void) {
    timer_start(T0, 3600);
    timer_pause(T0 + 1000);
    TEST_ASSERT_EQUAL(TIMER_PAUSED, timer_get_state());
}

void test_pause_saves_remaining_at_pause(void) {
    timer_start(T0, 3600);
    timer_pause(T0 + 1000);
    TEST_ASSERT_EQUAL_INT32(2600, g_rtc_state.remaining_at_pause);
}

void test_pause_clears_expiry_wall_time(void) {
    timer_start(T0, 3600);
    timer_pause(T0 + 1000);
    TEST_ASSERT_EQUAL_INT64(0, g_rtc_state.expiry_wall_time);
}

void test_resume_sets_state_running(void) {
    timer_start(T0, 3600);
    timer_pause(T0 + 1000);
    timer_resume(T0 + 2000);
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
}

void test_resume_sets_expiry_from_remaining_at_pause(void) {
    timer_start(T0, 3600);
    timer_pause(T0 + 1000);
    timer_resume(T0 + 2000);
    int64_t expected = (int64_t)(T0 + 2000) + 2600;
    TEST_ASSERT_EQUAL_INT64(expected, g_rtc_state.expiry_wall_time);
}

void test_resume_preserves_remaining_within_one_second(void) {
    timer_start(T0, 3600);
    timer_pause(T0 + 1000);
    time_t resume_time = T0 + 5000;
    timer_resume(resume_time);
    int32_t remaining = timer_tick(resume_time);
    TEST_ASSERT_INT_WITHIN(1, 2600, remaining);
}

void test_full_cycle_idle_run_pause_resume_expire(void) {
    timer_start(T0, 100);
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());

    timer_pause(T0 + 40);
    TEST_ASSERT_EQUAL_INT32(60, g_rtc_state.remaining_at_pause);

    timer_resume(T0 + 100);
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());

    int32_t remaining = timer_tick(T0 + 159);
    TEST_ASSERT_INT_WITHIN(1, 1, remaining);

    timer_tick(T0 + 161);
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
}

void test_is_new_day_false_when_same_date(void) {
    timer_record_date(T0);
    TEST_ASSERT_FALSE(timer_is_new_day(T0 + 3600));
}

void test_is_new_day_true_after_midnight(void) {
    timer_record_date(T0);
    TEST_ASSERT_TRUE(timer_is_new_day(T0 + 86400));
}

void test_needs_ntp_sync_false_immediately_after_sync(void) {
    timer_record_ntp_sync(T0);
    TEST_ASSERT_FALSE(timer_needs_ntp_sync(T0 + 1));
}

void test_needs_ntp_sync_true_after_10_minutes(void) {
    timer_record_ntp_sync(T0);
    TEST_ASSERT_TRUE(timer_needs_ntp_sync(T0 + 601));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_reset_state_is_idle);
    RUN_TEST(test_reset_expiry_is_zero);
    RUN_TEST(test_reset_remaining_at_pause_is_zero);
    RUN_TEST(test_start_sets_state_running);
    RUN_TEST(test_start_sets_expiry_wall_time);
    RUN_TEST(test_start_sets_allocation_sec);
    RUN_TEST(test_tick_does_not_modify_expiry_wall_time);
    RUN_TEST(test_tick_returns_correct_remaining_seconds);
    RUN_TEST(test_tick_returns_remaining_near_zero);
    RUN_TEST(test_tick_transitions_to_expired_when_time_elapsed);
    RUN_TEST(test_tick_returns_non_positive_when_expired);
    RUN_TEST(test_tick_at_exact_expiry_transitions);
    RUN_TEST(test_pause_sets_state_paused);
    RUN_TEST(test_pause_saves_remaining_at_pause);
    RUN_TEST(test_pause_clears_expiry_wall_time);
    RUN_TEST(test_resume_sets_state_running);
    RUN_TEST(test_resume_sets_expiry_from_remaining_at_pause);
    RUN_TEST(test_resume_preserves_remaining_within_one_second);
    RUN_TEST(test_full_cycle_idle_run_pause_resume_expire);
    RUN_TEST(test_is_new_day_false_when_same_date);
    RUN_TEST(test_is_new_day_true_after_midnight);
    RUN_TEST(test_needs_ntp_sync_false_immediately_after_sync);
    RUN_TEST(test_needs_ntp_sync_true_after_10_minutes);
    return UNITY_END();
}
