#include <unity.h>

/* Single-TU compilation */
#include "../../main/bedtime.c"
#include "../../main/quiet_hours.c"

void setUp(void) {}
void tearDown(void) {}

#define MIN_OF(h, m) ((h)*60 + (m))

/* ---- config validation ---- */

void test_hhmm_zero_is_valid_disabled(void) {
    TEST_ASSERT_TRUE(bedtime_hhmm_valid(0));
    TEST_ASSERT_EQUAL_INT(-1, bedtime_minutes(0));
}

void test_hhmm_evening_range_accepted(void) {
    TEST_ASSERT_TRUE(bedtime_hhmm_valid(1800));
    TEST_ASSERT_TRUE(bedtime_hhmm_valid(2200));
    TEST_ASSERT_TRUE(bedtime_hhmm_valid(2359));
    TEST_ASSERT_EQUAL_INT(MIN_OF(22, 0), bedtime_minutes(2200));
}

void test_hhmm_outside_evening_rejected(void) {
    TEST_ASSERT_FALSE(bedtime_hhmm_valid(1759)); /* before the floor */
    TEST_ASSERT_FALSE(bedtime_hhmm_valid(900));  /* daytime would brick the device until midnight */
    TEST_ASSERT_FALSE(bedtime_hhmm_valid(1));
    TEST_ASSERT_FALSE(bedtime_hhmm_valid(-2200));
    TEST_ASSERT_FALSE(bedtime_hhmm_valid(2400)); /* not a clock time */
    TEST_ASSERT_FALSE(bedtime_hhmm_valid(1860)); /* minute > 59 */
    TEST_ASSERT_EQUAL_INT(-1, bedtime_minutes(1860));
}

/* ---- active window ---- */

void test_active_at_and_after_threshold(void) {
    int bed = bedtime_minutes(2200);
    TEST_ASSERT_FALSE(bedtime_active(MIN_OF(21, 59), bed));
    TEST_ASSERT_TRUE(bedtime_active(MIN_OF(22, 0), bed)); /* exactly bedtime */
    TEST_ASSERT_TRUE(bedtime_active(MIN_OF(23, 59), bed));
}

void test_inactive_after_midnight_and_daytime(void) {
    int bed = bedtime_minutes(2200);
    TEST_ASSERT_FALSE(bedtime_active(MIN_OF(0, 0), bed)); /* rollover territory */
    TEST_ASSERT_FALSE(bedtime_active(MIN_OF(7, 30), bed));
    TEST_ASSERT_FALSE(bedtime_active(MIN_OF(17, 59), bed));
}

void test_disabled_is_never_active(void) {
    int bed = bedtime_minutes(0);
    TEST_ASSERT_FALSE(bedtime_active(MIN_OF(23, 0), bed));
    TEST_ASSERT_FALSE(bedtime_break_would_cross(MIN_OF(21, 50), 15, bed));
}

/* ---- break crossing ---- */

void test_break_crossing_boundaries(void) {
    int bed = bedtime_minutes(2200);
    /* 21:50 + 15 min ends 22:05 - crosses */
    TEST_ASSERT_TRUE(bedtime_break_would_cross(MIN_OF(21, 50), 15, bed));
    /* 21:45 + 15 min lands exactly on 22:00 - counts as crossing */
    TEST_ASSERT_TRUE(bedtime_break_would_cross(MIN_OF(21, 45), 15, bed));
    /* 21:44 + 15 min ends 21:59 - does not cross */
    TEST_ASSERT_FALSE(bedtime_break_would_cross(MIN_OF(21, 44), 15, bed));
    /* already past bedtime: the active check owns that case, not this one */
    TEST_ASSERT_FALSE(bedtime_break_would_cross(MIN_OF(22, 10), 15, bed));
}

/* ---- alert matrix ---- */

void test_alert_only_for_running_and_break(void) {
    TEST_ASSERT_TRUE(bedtime_should_alert(TIMER_RUNNING));
    TEST_ASSERT_TRUE(bedtime_should_alert(TIMER_BREAK));
    TEST_ASSERT_FALSE(bedtime_should_alert(TIMER_IDLE));
    TEST_ASSERT_FALSE(bedtime_should_alert(TIMER_PAUSED));
    TEST_ASSERT_FALSE(bedtime_should_alert(TIMER_EXPIRED));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_hhmm_zero_is_valid_disabled);
    RUN_TEST(test_hhmm_evening_range_accepted);
    RUN_TEST(test_hhmm_outside_evening_rejected);
    RUN_TEST(test_active_at_and_after_threshold);
    RUN_TEST(test_inactive_after_midnight_and_daytime);
    RUN_TEST(test_disabled_is_never_active);
    RUN_TEST(test_break_crossing_boundaries);
    RUN_TEST(test_alert_only_for_running_and_break);
    return UNITY_END();
}
