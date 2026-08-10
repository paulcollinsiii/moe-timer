#include <stdlib.h>
#include <time.h>
#include <unity.h>

/* Header-only unit: the inline itself is the whole translation unit. */
#include "time_util.h"

/* 2026-01-01 00:00:00 UTC — the epoch the mock clock also defaults to. */
#define T_NEW_YEAR 1767225600

void setUp(void) {
    /* UTC keeps the arithmetic below readable; the TZ-sensitivity case
       overrides it deliberately. */
    setenv("TZ", "UTC0", 1);
    tzset();
}

void tearDown(void) {}

void test_midnight_is_zero(void) {
    TEST_ASSERT_EQUAL_INT(0, time_util_minutes_of_day(T_NEW_YEAR));
}

void test_hour_and_minute_fields_combine(void) {
    TEST_ASSERT_EQUAL_INT(1, time_util_minutes_of_day(T_NEW_YEAR + 60));
    TEST_ASSERT_EQUAL_INT(60, time_util_minutes_of_day(T_NEW_YEAR + 3600));
    TEST_ASSERT_EQUAL_INT(720, time_util_minutes_of_day(T_NEW_YEAR + 12 * 3600));
    TEST_ASSERT_EQUAL_INT(1350, time_util_minutes_of_day(T_NEW_YEAR + 22 * 3600 + 30 * 60)); /* 22:30 */
}

void test_seconds_truncate_rather_than_round(void) {
    /* Callers compare against whole-minute schedule values, so 12:00:59
       must still read 720 — rounding up would fire the bedtime and
       quiet-hours gates a minute early. */
    TEST_ASSERT_EQUAL_INT(720, time_util_minutes_of_day(T_NEW_YEAR + 12 * 3600 + 59));
}

void test_last_minute_of_day(void) {
    TEST_ASSERT_EQUAL_INT(1439, time_util_minutes_of_day(T_NEW_YEAR + 86400 - 60));
    TEST_ASSERT_EQUAL_INT(1439, time_util_minutes_of_day(T_NEW_YEAR + 86400 - 1));
}

void test_wraps_to_zero_at_next_midnight(void) {
    TEST_ASSERT_EQUAL_INT(0, time_util_minutes_of_day(T_NEW_YEAR + 86400));
}

void test_uses_local_time_not_utc(void) {
    /* The device runs on the NVS-configured TZ and every caller means
       local wall-clock minutes. Fixed-offset zone, so no DST ambiguity:
       midnight UTC is 19:00 the previous day at UTC-5. */
    setenv("TZ", "EST5", 1);
    tzset();
    TEST_ASSERT_EQUAL_INT(19 * 60, time_util_minutes_of_day(T_NEW_YEAR));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_midnight_is_zero);
    RUN_TEST(test_hour_and_minute_fields_combine);
    RUN_TEST(test_seconds_truncate_rather_than_round);
    RUN_TEST(test_last_minute_of_day);
    RUN_TEST(test_wraps_to_zero_at_next_midnight);
    RUN_TEST(test_uses_local_time_not_utc);
    return UNITY_END();
}
