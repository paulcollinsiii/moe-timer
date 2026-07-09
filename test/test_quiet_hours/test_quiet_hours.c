#include <stdbool.h>
#include <unity.h>

/* Single-TU compilation of the pure quiet-hours logic */
#include "../../main/quiet_hours.c"

void setUp(void) {}
void tearDown(void) {}

/* Times are minutes since local midnight. Default window 22:30 -> 08:00
   (1350 -> 480) wraps midnight. */

void test_active_late_evening(void) {
    TEST_ASSERT_TRUE(quiet_hours_active(1350, 1350, 480)); /* 22:30 exactly */
    TEST_ASSERT_TRUE(quiet_hours_active(1439, 1350, 480)); /* 23:59 */
}

void test_active_early_morning(void) {
    TEST_ASSERT_TRUE(quiet_hours_active(0, 1350, 480));   /* midnight */
    TEST_ASSERT_TRUE(quiet_hours_active(479, 1350, 480)); /* 07:59 */
}

void test_inactive_daytime(void) {
    TEST_ASSERT_FALSE(quiet_hours_active(480, 1350, 480));  /* 08:00 exactly */
    TEST_ASSERT_FALSE(quiet_hours_active(720, 1350, 480));  /* noon */
    TEST_ASSERT_FALSE(quiet_hours_active(1349, 1350, 480)); /* 22:29 */
}

void test_non_wrapping_window(void) {
    /* e.g. 13:00 -> 15:00 quiet (nap time) */
    TEST_ASSERT_FALSE(quiet_hours_active(779, 780, 900));
    TEST_ASSERT_TRUE(quiet_hours_active(780, 780, 900));
    TEST_ASSERT_TRUE(quiet_hours_active(899, 780, 900));
    TEST_ASSERT_FALSE(quiet_hours_active(900, 780, 900));
}

void test_equal_start_end_disables(void) {
    TEST_ASSERT_FALSE(quiet_hours_active(0, 480, 480));
    TEST_ASSERT_FALSE(quiet_hours_active(480, 480, 480));
}

void test_hhmm_to_minutes(void) {
    TEST_ASSERT_EQUAL_INT(1350, quiet_hhmm_to_minutes(2230));
    TEST_ASSERT_EQUAL_INT(480, quiet_hhmm_to_minutes(800));
    TEST_ASSERT_EQUAL_INT(0, quiet_hhmm_to_minutes(0));
    TEST_ASSERT_EQUAL_INT(1439, quiet_hhmm_to_minutes(2359));
}

void test_hhmm_valid(void) {
    TEST_ASSERT_TRUE(quiet_hhmm_valid(0));    /* 00:00 */
    TEST_ASSERT_TRUE(quiet_hhmm_valid(2359)); /* 23:59 */
    TEST_ASSERT_TRUE(quiet_hhmm_valid(2230));
    TEST_ASSERT_TRUE(quiet_hhmm_valid(800));
    /* A 0..2359 range check alone lets these through, but they aren't
       real times — the minutes/hour fields are out of range. */
    TEST_ASSERT_FALSE(quiet_hhmm_valid(2260)); /* minute 60 */
    TEST_ASSERT_FALSE(quiet_hhmm_valid(2299)); /* minute 99 */
    TEST_ASSERT_FALSE(quiet_hhmm_valid(2400)); /* hour 24 */
    TEST_ASSERT_FALSE(quiet_hhmm_valid(-1));
    TEST_ASSERT_FALSE(quiet_hhmm_valid(9999));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_active_late_evening);
    RUN_TEST(test_active_early_morning);
    RUN_TEST(test_inactive_daytime);
    RUN_TEST(test_non_wrapping_window);
    RUN_TEST(test_equal_start_end_disables);
    RUN_TEST(test_hhmm_to_minutes);
    RUN_TEST(test_hhmm_valid);
    return UNITY_END();
}
