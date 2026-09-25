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

/* ---- time_util_clock_plausible (BUG-11) ---------------------------------- */

/* The floor is a literal, and the literal is the date its comment names.
   Checked through gmtime_r rather than restated as a number, so an edit to
   the constant that forgets the prose (or the reverse) fails here. */
void test_the_floor_is_new_year_2026_utc(void) {
    const time_t floor_t = TIME_UTIL_CLOCK_FLOOR;
    struct tm tm;
    gmtime_r(&floor_t, &tm);
    TEST_ASSERT_EQUAL_INT(2026, tm.tm_year + 1900);
    TEST_ASSERT_EQUAL_INT(0, tm.tm_mon);
    TEST_ASSERT_EQUAL_INT(1, tm.tm_mday);
    TEST_ASSERT_EQUAL_INT(0, tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec);
}

void test_the_floor_itself_is_plausible(void) {
    TEST_ASSERT_TRUE(time_util_clock_plausible(TIME_UTIL_CLOCK_FLOOR));
}

void test_one_second_below_the_floor_is_not(void) {
    TEST_ASSERT_FALSE(time_util_clock_plausible(TIME_UTIL_CLOCK_FLOOR - 1));
}

/* What a power-on reset actually produces: the epoch plus uptime. A day
   of uptime is far more than any wake takes to reach NTP. */
void test_a_near_epoch_clock_is_not_plausible(void) {
    TEST_ASSERT_FALSE(time_util_clock_plausible(0));
    TEST_ASSERT_FALSE(time_util_clock_plausible(90));
    TEST_ASSERT_FALSE(time_util_clock_plausible(86400));
    TEST_ASSERT_FALSE(time_util_clock_plausible(-1));
}

void test_a_set_clock_is_plausible(void) {
    TEST_ASSERT_TRUE(time_util_clock_plausible(1785283200)); /* 2026-07-29 */
    TEST_ASSERT_TRUE(time_util_clock_plausible(T_NEW_YEAR + 10 * 365 * 86400));
}

/* UTC in, no TZ applied: the floor is an instant, not a local date, so a
   zone change cannot move it. */
void test_plausibility_ignores_the_time_zone(void) {
    setenv("TZ", "<-02>2", 1);
    tzset();
    TEST_ASSERT_FALSE(time_util_clock_plausible(TIME_UTIL_CLOCK_FLOOR - 1));
    TEST_ASSERT_TRUE(time_util_clock_plausible(TIME_UTIL_CLOCK_FLOOR));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_midnight_is_zero);
    RUN_TEST(test_hour_and_minute_fields_combine);
    RUN_TEST(test_seconds_truncate_rather_than_round);
    RUN_TEST(test_last_minute_of_day);
    RUN_TEST(test_wraps_to_zero_at_next_midnight);
    RUN_TEST(test_uses_local_time_not_utc);
    RUN_TEST(test_the_floor_is_new_year_2026_utc);
    RUN_TEST(test_the_floor_itself_is_plausible);
    RUN_TEST(test_one_second_below_the_floor_is_not);
    RUN_TEST(test_a_near_epoch_clock_is_not_plausible);
    RUN_TEST(test_a_set_clock_is_plausible);
    RUN_TEST(test_plausibility_ignores_the_time_zone);
    return UNITY_END();
}
