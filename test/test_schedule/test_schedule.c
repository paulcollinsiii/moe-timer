#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unity.h>

/* Single-TU compilation: pull in mocks then source under test */
// clang-format off
#include "mock_hal_time.c"
#include "mock_hal_nvs.c"
#include "../../main/schedule.c"
// clang-format on

/* ------------------------------------------------------------------ */
/* setUp / tearDown                                                     */
/* ------------------------------------------------------------------ */

void setUp(void) {
    mock_nvs_reset();
    /* Use UTC so timestamps map to predictable dates regardless of host TZ */
    setenv("TZ", "UTC0", 1);
    tzset();
    /* Default: weekday allocation 60 min, weekend 120, holiday 120 */
    hal_nvs_write_u16("weekday_min", 60);
    hal_nvs_write_u16("weekend_min", 120);
    hal_nvs_write_u16("holiday_min", 120);
    /* Default holiday blob: just New Year's Day 2026 */
    const char *holidays = "2026-01-01\n";
    hal_nvs_write_blob("holidays", holidays, strlen(holidays));
    /* Default time: Monday 2026-01-05 */
    mock_time_set(1767571200);
}

void tearDown(void) {}

/* ------------------------------------------------------------------ */
/* schedule_is_holiday — pure string search, no HAL                    */
/* ------------------------------------------------------------------ */

void test_is_holiday_match(void) {
    const char *blob = "2026-01-01\n2026-07-04\n2026-12-25\n";
    TEST_ASSERT_TRUE(schedule_is_holiday("2026-01-01", blob, strlen(blob)));
    TEST_ASSERT_TRUE(schedule_is_holiday("2026-07-04", blob, strlen(blob)));
    TEST_ASSERT_TRUE(schedule_is_holiday("2026-12-25", blob, strlen(blob)));
}

void test_is_holiday_no_match(void) {
    const char *blob = "2026-01-01\n2026-07-04\n";
    TEST_ASSERT_FALSE(schedule_is_holiday("2026-06-15", blob, strlen(blob)));
    TEST_ASSERT_FALSE(schedule_is_holiday("2025-01-01", blob, strlen(blob)));
}

void test_is_holiday_partial_date_not_matched(void) {
    /* "2026-01-0" must NOT match "2026-01-01" */
    const char *blob = "2026-01-01\n";
    TEST_ASSERT_FALSE(schedule_is_holiday("2026-01-0", blob, strlen(blob)));
}

void test_is_holiday_windows_line_endings(void) {
    /* \r\n blobs should still match full dates */
    const char *blob = "2026-01-01\r\n2026-07-04\r\n";
    TEST_ASSERT_TRUE(schedule_is_holiday("2026-01-01", blob, strlen(blob)));
}

void test_is_holiday_trailing_newline(void) {
    const char *blob = "2026-12-25\n";
    TEST_ASSERT_TRUE(schedule_is_holiday("2026-12-25", blob, strlen(blob)));
}

/* ------------------------------------------------------------------ */
/* schedule_get_day_type                                               */
/* ------------------------------------------------------------------ */

void test_weekday_monday(void) {
    mock_time_set(1767571200); /* 2026-01-05 Mon */
    TEST_ASSERT_EQUAL(DAY_WEEKDAY, schedule_get_day_type(hal_time_now()));
}

void test_weekday_friday(void) {
    mock_time_set(1767830400); /* 2026-01-09 Fri */
    TEST_ASSERT_EQUAL(DAY_WEEKDAY, schedule_get_day_type(hal_time_now()));
}

void test_weekend_saturday(void) {
    mock_time_set(1767398400); /* 2026-01-03 Sat */
    TEST_ASSERT_EQUAL(DAY_WEEKEND, schedule_get_day_type(hal_time_now()));
}

void test_weekend_sunday(void) {
    mock_time_set(1767484800); /* 2026-01-04 Sun */
    TEST_ASSERT_EQUAL(DAY_WEEKEND, schedule_get_day_type(hal_time_now()));
}

void test_holiday_new_years_day(void) {
    mock_time_set(1767225600); /* 2026-01-01 Thu — in holiday blob */
    TEST_ASSERT_EQUAL(DAY_HOLIDAY, schedule_get_day_type(hal_time_now()));
}

void test_holiday_takes_priority_over_weekend(void) {
    /* Put a Saturday in the holiday blob; should return HOLIDAY not WEEKEND */
    const char *blob = "2026-01-03\n"; /* Sat */
    hal_nvs_write_blob("holidays", blob, strlen(blob));
    mock_time_set(1767398400); /* 2026-01-03 Sat */
    TEST_ASSERT_EQUAL(DAY_HOLIDAY, schedule_get_day_type(hal_time_now()));
}

void test_non_holiday_weekday_not_holiday(void) {
    mock_time_set(1767571200); /* 2026-01-05 Mon — not in blob */
    TEST_ASSERT_NOT_EQUAL(DAY_HOLIDAY, schedule_get_day_type(hal_time_now()));
}

/* ------------------------------------------------------------------ */
/* schedule_get_allocation_sec                                          */
/* ------------------------------------------------------------------ */

void test_allocation_weekday(void) {
    hal_nvs_write_u16("weekday_min", 45);
    TEST_ASSERT_EQUAL_UINT32(2700, schedule_get_allocation_sec(DAY_WEEKDAY));
}

void test_allocation_weekend(void) {
    hal_nvs_write_u16("weekend_min", 90);
    TEST_ASSERT_EQUAL_UINT32(5400, schedule_get_allocation_sec(DAY_WEEKEND));
}

void test_allocation_holiday(void) {
    hal_nvs_write_u16("holiday_min", 150);
    TEST_ASSERT_EQUAL_UINT32(9000, schedule_get_allocation_sec(DAY_HOLIDAY));
}

void test_allocation_missing_key_falls_back_to_default(void) {
    /* Don't write weekday_min — should fall back to NVS_DEFAULT_WEEKDAY_MIN */
    mock_nvs_reset();
    uint32_t alloc = schedule_get_allocation_sec(DAY_WEEKDAY);
    TEST_ASSERT_EQUAL_UINT32(NVS_DEFAULT_WEEKDAY_MIN * 60, alloc);
}

void test_holiday_falls_back_to_compile_time_defaults(void) {
    /* Clear NVS entirely — no holidays blob written.
       2026-01-01 is in NVS_DEFAULT_HOLIDAYS so must still classify as DAY_HOLIDAY. */
    mock_nvs_reset();
    mock_time_set(1767225600); /* 2026-01-01 Thu */
    TEST_ASSERT_EQUAL(DAY_HOLIDAY, schedule_get_day_type(hal_time_now()));
}

/* ------------------------------------------------------------------ */
/* Runner                                                               */
/* ------------------------------------------------------------------ */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_is_holiday_match);
    RUN_TEST(test_is_holiday_no_match);
    RUN_TEST(test_is_holiday_partial_date_not_matched);
    RUN_TEST(test_is_holiday_windows_line_endings);
    RUN_TEST(test_is_holiday_trailing_newline);
    RUN_TEST(test_weekday_monday);
    RUN_TEST(test_weekday_friday);
    RUN_TEST(test_weekend_saturday);
    RUN_TEST(test_weekend_sunday);
    RUN_TEST(test_holiday_new_years_day);
    RUN_TEST(test_holiday_takes_priority_over_weekend);
    RUN_TEST(test_non_holiday_weekday_not_holiday);
    RUN_TEST(test_holiday_falls_back_to_compile_time_defaults);
    RUN_TEST(test_allocation_weekday);
    RUN_TEST(test_allocation_weekend);
    RUN_TEST(test_allocation_holiday);
    RUN_TEST(test_allocation_missing_key_falls_back_to_default);
    return UNITY_END();
}
