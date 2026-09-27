#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* ---- time_util_day_plausible (BUG-14) ----------------------------------- */

/* The dates an unset clock writes into last_date: the epoch's own day,
   and the day before it west of UTC. Never plausible. */
void test_a_day_an_unset_clock_dated_is_not_plausible(void) {
    TEST_ASSERT_FALSE(time_util_day_plausible("1970-01-01"));
    TEST_ASSERT_FALSE(time_util_day_plausible("1969-12-31"));
    setenv("TZ", "EST5", 1);
    tzset();
    TEST_ASSERT_FALSE(time_util_day_plausible("1970-01-01"));
    TEST_ASSERT_FALSE(time_util_day_plausible("1969-12-31"));
}

/* No day recorded is not a plausible day, and neither is anything that is
   not a ten-character ISO date. */
void test_an_empty_or_malformed_day_is_not_plausible(void) {
    TEST_ASSERT_FALSE(time_util_day_plausible(""));
    TEST_ASSERT_FALSE(time_util_day_plausible(NULL));
    TEST_ASSERT_FALSE(time_util_day_plausible("9"));
    TEST_ASSERT_FALSE(time_util_day_plausible("2026-07-29x"));
}

void test_a_set_clocks_day_is_plausible(void) {
    TEST_ASSERT_TRUE(time_util_day_plausible("2026-01-01"));
    TEST_ASSERT_TRUE(time_util_day_plausible("2026-09-25"));
    TEST_ASSERT_TRUE(time_util_day_plausible("2036-01-01"));
    TEST_ASSERT_FALSE(time_util_day_plausible("2025-12-31")); /* UTC0: wholly below the floor */
}

/* THE ONE-THRESHOLD PROPERTY: a local day is plausible exactly when some
   instant in it passes time_util_clock_plausible(). Sampled hourly on a
   grid that contains the floor instant itself, so "some sampled instant
   of the day is plausible" is exactly "the day reaches the floor", in
   whole-hour, half-hour and both-sign zones. */
void test_a_day_is_plausible_exactly_when_one_of_its_instants_is(void) {
    static const char *const zones[] = {"UTC0", "EST5", "<+14>-14", "<-12>12", "<+0530>-5:30"};
    for (size_t z = 0; z < sizeof zones / sizeof zones[0]; z++) {
        setenv("TZ", zones[z], 1);
        tzset();
        char days[8][11];
        bool any_plausible[8] = {false};
        int n = 0;
        for (int k = -60; k <= 60; k++) {
            const time_t t = TIME_UTIL_CLOCK_FLOOR + (time_t)k * 3600;
            struct tm tm;
            localtime_r(&t, &tm);
            char day[11];
            date_fmt_iso(day, sizeof day, &tm);
            if (n == 0 || strcmp(days[n - 1], day) != 0) {
                TEST_ASSERT_TRUE(n < 8);
                memcpy(days[n], day, sizeof day);
                n++;
            }
            any_plausible[n - 1] = any_plausible[n - 1] || time_util_clock_plausible(t);
        }
        for (int d = 0; d < n; d++) {
            char msg[40];
            snprintf(msg, sizeof msg, "%s %s", zones[z], days[d]);
            TEST_ASSERT_EQUAL_MESSAGE(any_plausible[d], time_util_day_plausible(days[d]), msg);
        }
    }
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
    RUN_TEST(test_a_day_an_unset_clock_dated_is_not_plausible);
    RUN_TEST(test_an_empty_or_malformed_day_is_not_plausible);
    RUN_TEST(test_a_set_clocks_day_is_plausible);
    RUN_TEST(test_a_day_is_plausible_exactly_when_one_of_its_instants_is);
    return UNITY_END();
}
