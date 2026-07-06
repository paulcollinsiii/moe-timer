#include <unity.h>

/* Single-TU compilation of the pure SoC curve (no ESP-IDF headers) */
#include "../../main/battery_soc.c"

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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_full_at_and_above_4150);
    RUN_TEST(test_top_segment_interpolation);
    RUN_TEST(test_mid_segments_interpolation);
    RUN_TEST(test_bottom_segment_interpolation);
    RUN_TEST(test_empty_and_garbage_clamp_to_zero);
    return UNITY_END();
}
