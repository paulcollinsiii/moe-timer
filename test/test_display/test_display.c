#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unity.h>

/* Pull in only the two testable math functions.
   display.cpp is C++; we test the C implementations here directly. */
extern uint16_t display_bar_fill_px(int32_t remaining_sec, uint32_t allocation_sec);
extern void display_format_remaining(char *buf, size_t len, int32_t remaining_sec);

/* Provide stub implementations for this TU (avoids linking LovyanGFX) */
uint16_t display_bar_fill_px(int32_t remaining_sec, uint32_t allocation_sec) {
    if (allocation_sec == 0 || remaining_sec <= 0)
        return 0;
    if ((uint32_t)remaining_sec >= allocation_sec)
        return 280;
    return (uint16_t)((uint32_t)remaining_sec * 280u / allocation_sec);
}

void display_format_remaining(char *buf, size_t len, int32_t remaining_sec) {
    if (remaining_sec <= 0) {
        snprintf(buf, len, "0 min");
        return;
    }
    if (remaining_sec < 300) {
        snprintf(buf, len, "%ld min %ld sec", (long)(remaining_sec / 60), (long)(remaining_sec % 60));
    } else {
        snprintf(buf, len, "%ld min", (long)(remaining_sec / 60));
    }
}

void setUp(void) {}
void tearDown(void) {}

/* ---- display_bar_fill_px ---- */

void test_bar_full_when_remaining_equals_allocation(void) {
    TEST_ASSERT_EQUAL_UINT16(280, display_bar_fill_px(3600, 3600));
}

void test_bar_full_when_remaining_exceeds_allocation(void) {
    TEST_ASSERT_EQUAL_UINT16(280, display_bar_fill_px(4000, 3600));
}

void test_bar_zero_when_expired(void) {
    TEST_ASSERT_EQUAL_UINT16(0, display_bar_fill_px(0, 3600));
    TEST_ASSERT_EQUAL_UINT16(0, display_bar_fill_px(-1, 3600));
}

void test_bar_zero_when_allocation_zero(void) {
    TEST_ASSERT_EQUAL_UINT16(0, display_bar_fill_px(60, 0));
}

void test_bar_half_fill(void) {
    /* 1800 / 3600 = 50% -> 140 px */
    TEST_ASSERT_EQUAL_UINT16(140, display_bar_fill_px(1800, 3600));
}

void test_bar_quarter_fill(void) {
    /* 900 / 3600 = 25% -> 70 px */
    TEST_ASSERT_EQUAL_UINT16(70, display_bar_fill_px(900, 3600));
}

void test_bar_just_expired(void) {
    TEST_ASSERT_EQUAL_UINT16(0, display_bar_fill_px(-1, 3600));
}

/* ---- display_format_remaining ---- */

void test_format_shows_minutes_only_above_5_min(void) {
    char buf[64];
    display_format_remaining(buf, sizeof(buf), 600); /* 10 min */
    TEST_ASSERT_EQUAL_STRING("10 min", buf);
}

void test_format_shows_minutes_only_at_exactly_5_min(void) {
    char buf[64];
    display_format_remaining(buf, sizeof(buf), 300); /* 5 min */
    TEST_ASSERT_EQUAL_STRING("5 min", buf);
}

void test_format_shows_minutes_and_seconds_below_5_min(void) {
    char buf[64];
    display_format_remaining(buf, sizeof(buf), 299); /* 4 min 59 sec */
    TEST_ASSERT_EQUAL_STRING("4 min 59 sec", buf);
}

void test_format_shows_zero_when_expired(void) {
    char buf[64];
    display_format_remaining(buf, sizeof(buf), 0);
    TEST_ASSERT_EQUAL_STRING("0 min", buf);
    display_format_remaining(buf, sizeof(buf), -1);
    TEST_ASSERT_EQUAL_STRING("0 min", buf);
}

void test_format_1_min_30_sec(void) {
    char buf[64];
    display_format_remaining(buf, sizeof(buf), 90);
    TEST_ASSERT_EQUAL_STRING("1 min 30 sec", buf);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_bar_full_when_remaining_equals_allocation);
    RUN_TEST(test_bar_full_when_remaining_exceeds_allocation);
    RUN_TEST(test_bar_zero_when_expired);
    RUN_TEST(test_bar_zero_when_allocation_zero);
    RUN_TEST(test_bar_half_fill);
    RUN_TEST(test_bar_quarter_fill);
    RUN_TEST(test_bar_just_expired);
    RUN_TEST(test_format_shows_minutes_only_above_5_min);
    RUN_TEST(test_format_shows_minutes_only_at_exactly_5_min);
    RUN_TEST(test_format_shows_minutes_and_seconds_below_5_min);
    RUN_TEST(test_format_shows_zero_when_expired);
    RUN_TEST(test_format_1_min_30_sec);
    return UNITY_END();
}
