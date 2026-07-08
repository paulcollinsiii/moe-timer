#include <unity.h>

/* Single-TU compilation of the pure SoC curve + low-battery policy */
// clang-format off
#include "../../main/battery_soc.c"
#include "../../main/battery_policy.c"
// clang-format on

void setUp(void) {}
void tearDown(void) {}

/* Expectations computed from the piecewise-linear curve in battery_soc.c:
   (3300,0) (3500,15) (3650,35) (3800,55) (3950,80) (4150,100) */

void test_full_at_and_above_4150(void) {
    TEST_ASSERT_EQUAL_INT(100, battery_percent_from_mv(4150));
    TEST_ASSERT_EQUAL_INT(100, battery_percent_from_mv(4200));
    /* USB-powered boards read above true full — clamp */
    TEST_ASSERT_EQUAL_INT(100, battery_percent_from_mv(4350));
}

void test_top_segment_interpolation(void) {
    /* 3950..4150 maps 80..100: 4060 -> 80 + 110*20/200 = 91 */
    TEST_ASSERT_EQUAL_INT(91, battery_percent_from_mv(4060));
}

void test_mid_segments_interpolation(void) {
    /* 3800..3950 maps 55..80: 3820 -> 55 + 20*25/150 = 58 */
    TEST_ASSERT_EQUAL_INT(58, battery_percent_from_mv(3820));
    /* 3500..3650 maps 15..35: 3600 -> 15 + 100*20/150 = 28 */
    TEST_ASSERT_EQUAL_INT(28, battery_percent_from_mv(3600));
}

void test_bottom_segment_interpolation(void) {
    /* 3300..3500 maps 0..15: 3400 -> 100*15/200 = 7 */
    TEST_ASSERT_EQUAL_INT(7, battery_percent_from_mv(3400));
}

void test_empty_and_garbage_clamp_to_zero(void) {
    TEST_ASSERT_EQUAL_INT(0, battery_percent_from_mv(3300));
    TEST_ASSERT_EQUAL_INT(0, battery_percent_from_mv(3000));
    TEST_ASSERT_EQUAL_INT(0, battery_percent_from_mv(0));
    TEST_ASSERT_EQUAL_INT(0, battery_percent_from_mv(-100));
}

/* ---- low-battery policy: 15% warn badge, 10% charge lock ---- */

void test_policy_ok_above_warn_threshold(void) {
    TEST_ASSERT_EQUAL(BATT_OK, battery_policy_evaluate(100, false));
    TEST_ASSERT_EQUAL(BATT_OK, battery_policy_evaluate(16, false));
}

void test_policy_warn_at_15_down_to_11(void) {
    TEST_ASSERT_EQUAL(BATT_WARN, battery_policy_evaluate(15, false));
    TEST_ASSERT_EQUAL(BATT_WARN, battery_policy_evaluate(11, false));
}

void test_policy_lock_at_10_and_below(void) {
    TEST_ASSERT_EQUAL(BATT_LOCK, battery_policy_evaluate(10, false));
    TEST_ASSERT_EQUAL(BATT_LOCK, battery_policy_evaluate(0, false));
}

void test_policy_lock_hysteresis_releases_above_warn(void) {
    /* Once locked, readings bouncing around 10% must not flap the screen:
       stay locked through the whole warn band, release only above 15%. */
    TEST_ASSERT_EQUAL(BATT_LOCK, battery_policy_evaluate(11, true));
    TEST_ASSERT_EQUAL(BATT_LOCK, battery_policy_evaluate(15, true));
    TEST_ASSERT_EQUAL(BATT_OK, battery_policy_evaluate(16, true));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_full_at_and_above_4150);
    RUN_TEST(test_top_segment_interpolation);
    RUN_TEST(test_mid_segments_interpolation);
    RUN_TEST(test_bottom_segment_interpolation);
    RUN_TEST(test_empty_and_garbage_clamp_to_zero);
    RUN_TEST(test_policy_ok_above_warn_threshold);
    RUN_TEST(test_policy_warn_at_15_down_to_11);
    RUN_TEST(test_policy_lock_at_10_and_below);
    RUN_TEST(test_policy_lock_hysteresis_releases_above_warn);
    return UNITY_END();
}
