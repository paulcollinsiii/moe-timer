#include <unity.h>

/* Single-TU compilation */
#include "../../main/config_validate.c"

void setUp(void) {}
void tearDown(void) {}

void test_accepts_valid_dates(void) {
    TEST_ASSERT_TRUE(config_is_iso_date("2026-08-20"));
    TEST_ASSERT_TRUE(config_is_iso_date("2026-01-01"));
    TEST_ASSERT_TRUE(config_is_iso_date("2026-12-31"));
    TEST_ASSERT_TRUE(config_is_iso_date("2028-02-29")); /* leap year */
}

void test_rejects_bad_shape(void) {
    TEST_ASSERT_FALSE(config_is_iso_date(NULL));
    TEST_ASSERT_FALSE(config_is_iso_date(""));
    TEST_ASSERT_FALSE(config_is_iso_date("2026-8-20"));  /* not zero-padded */
    TEST_ASSERT_FALSE(config_is_iso_date("2026/08/20")); /* wrong separators */
    TEST_ASSERT_FALSE(config_is_iso_date("not-a-date"));
    TEST_ASSERT_FALSE(config_is_iso_date("2026-08-2x"));
}

void test_rejects_impossible_calendar_dates(void) {
    TEST_ASSERT_FALSE(config_is_iso_date("2026-13-45"));
    TEST_ASSERT_FALSE(config_is_iso_date("2026-00-10"));
    TEST_ASSERT_FALSE(config_is_iso_date("2026-02-00"));
    TEST_ASSERT_FALSE(config_is_iso_date("2026-02-31")); /* Feb has no 31st */
    TEST_ASSERT_FALSE(config_is_iso_date("2026-04-31")); /* April has 30 days */
    TEST_ASSERT_FALSE(config_is_iso_date("2026-02-29")); /* not a leap year */
    TEST_ASSERT_FALSE(config_is_iso_date("2100-02-29")); /* century non-leap */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_accepts_valid_dates);
    RUN_TEST(test_rejects_bad_shape);
    RUN_TEST(test_rejects_impossible_calendar_dates);
    return UNITY_END();
}
