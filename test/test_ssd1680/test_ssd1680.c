#include <stdbool.h>
#include <stdint.h>
#include <unity.h>

/* Single-TU compilation of the pure guard logic (no ESP-IDF headers) */
#include "../../components/ssd1680/ssd1680_guard.c"

void setUp(void) {}
void tearDown(void) {}

void test_allowed_on_first_refresh(void) {
    TEST_ASSERT_TRUE(ssd1680_refresh_allowed(1000, 0, 1));
}

void test_blocked_within_min_interval(void) {
    TEST_ASSERT_FALSE(ssd1680_refresh_allowed(1000, 1000, 1));
}

void test_allowed_at_exactly_min_interval(void) {
    TEST_ASSERT_TRUE(ssd1680_refresh_allowed(1001, 1000, 1));
}

void test_allowed_after_min_interval(void) {
    TEST_ASSERT_TRUE(ssd1680_refresh_allowed(1055, 1000, 1));
}

void test_allowed_when_clock_steps_backwards(void) {
    /* NTP corrected the clock backwards past the last-refresh stamp;
       blocking here could wedge refreshes for a long time — allow. */
    TEST_ASSERT_TRUE(ssd1680_refresh_allowed(900, 1000, 1));
}

void test_blocked_with_larger_interval(void) {
    TEST_ASSERT_FALSE(ssd1680_refresh_allowed(1029, 1000, 30));
    TEST_ASSERT_TRUE(ssd1680_refresh_allowed(1030, 1000, 30));
}

/* Wait derivation: how long a too-soon caller must sleep so the frame is
   accepted instead of silently dropped (display.c absorbs this). */

void test_wait_zero_when_allowed(void) {
    TEST_ASSERT_EQUAL_INT32(0, ssd1680_refresh_wait_sec(1000, 0, 1));    /* cold boot */
    TEST_ASSERT_EQUAL_INT32(0, ssd1680_refresh_wait_sec(1001, 1000, 1)); /* interval elapsed */
    TEST_ASSERT_EQUAL_INT32(0, ssd1680_refresh_wait_sec(900, 1000, 1));  /* clock stepped back */
}

void test_wait_covers_remaining_interval(void) {
    TEST_ASSERT_EQUAL_INT32(1, ssd1680_refresh_wait_sec(1000, 1000, 1)); /* same second */
    TEST_ASSERT_EQUAL_INT32(30, ssd1680_refresh_wait_sec(1000, 1000, 30));
    TEST_ASSERT_EQUAL_INT32(1, ssd1680_refresh_wait_sec(1029, 1000, 30));
}

void test_wait_then_allowed(void) {
    int64_t now = 1000, last = 1000;
    int32_t w = ssd1680_refresh_wait_sec(now, last, 1);
    TEST_ASSERT_TRUE(ssd1680_refresh_allowed(now + w, last, 1));
}

/* Mode resolution: 0 = FULL, 1 = PARTIAL (matches ssd1680_refresh_mode_t).
   A partial diff against previous-frame RAM that was never written this
   power cycle (panel power loss = garbage RAM) must promote to full. */

void test_partial_promoted_to_full_without_valid_prev_frame(void) {
    TEST_ASSERT_EQUAL_INT(0, ssd1680_resolve_refresh_mode(1, false));
}

void test_partial_kept_with_valid_prev_frame(void) {
    TEST_ASSERT_EQUAL_INT(1, ssd1680_resolve_refresh_mode(1, true));
}

void test_full_stays_full_regardless_of_prev_frame(void) {
    TEST_ASSERT_EQUAL_INT(0, ssd1680_resolve_refresh_mode(0, false));
    TEST_ASSERT_EQUAL_INT(0, ssd1680_resolve_refresh_mode(0, true));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_allowed_on_first_refresh);
    RUN_TEST(test_blocked_within_min_interval);
    RUN_TEST(test_allowed_at_exactly_min_interval);
    RUN_TEST(test_allowed_after_min_interval);
    RUN_TEST(test_allowed_when_clock_steps_backwards);
    RUN_TEST(test_blocked_with_larger_interval);
    RUN_TEST(test_wait_zero_when_allowed);
    RUN_TEST(test_wait_covers_remaining_interval);
    RUN_TEST(test_wait_then_allowed);
    RUN_TEST(test_partial_promoted_to_full_without_valid_prev_frame);
    RUN_TEST(test_partial_kept_with_valid_prev_frame);
    RUN_TEST(test_full_stays_full_regardless_of_prev_frame);
    return UNITY_END();
}
