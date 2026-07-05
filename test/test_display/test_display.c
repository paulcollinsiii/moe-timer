#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unity.h>

/* Single-TU compilation of the real layout math */
#include "../../main/display_layout.c"

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

/* ---- display_format_remaining: HH:MM:SS always ---- */

void test_format_hours_minutes_seconds(void) {
    char buf[64];
    display_format_remaining(buf, sizeof(buf), 3600); /* 1 h */
    TEST_ASSERT_EQUAL_STRING("01:00:00", buf);
    display_format_remaining(buf, sizeof(buf), 7325); /* 2 h 2 min 5 s */
    TEST_ASSERT_EQUAL_STRING("02:02:05", buf);
}

void test_format_minutes_and_seconds(void) {
    char buf[64];
    display_format_remaining(buf, sizeof(buf), 600);
    TEST_ASSERT_EQUAL_STRING("00:10:00", buf);
    display_format_remaining(buf, sizeof(buf), 299);
    TEST_ASSERT_EQUAL_STRING("00:04:59", buf);
    display_format_remaining(buf, sizeof(buf), 145);
    TEST_ASSERT_EQUAL_STRING("00:02:25", buf);
}

void test_format_shows_zero_when_expired(void) {
    char buf[64];
    display_format_remaining(buf, sizeof(buf), 0);
    TEST_ASSERT_EQUAL_STRING("00:00:00", buf);
    display_format_remaining(buf, sizeof(buf), -1);
    TEST_ASSERT_EQUAL_STRING("00:00:00", buf);
}

void test_format_1_min_30_sec(void) {
    char buf[64];
    display_format_remaining(buf, sizeof(buf), 90);
    TEST_ASSERT_EQUAL_STRING("00:01:30", buf);
}

/* display_fb_invert_byte_cols: inverts byte columns [b0..b1] of every row —
   used to build the inverse pass of the ghost-cleaning double partial. */

void test_invert_byte_cols_middle_column(void) {
    uint8_t fb[2][4] = {{0x00, 0xFF, 0xA5, 0x00}, {0x11, 0x22, 0x33, 0x44}};
    display_fb_invert_byte_cols(&fb[0][0], 2, 4, 1, 2);
    TEST_ASSERT_EQUAL_HEX8(0x00, fb[0][0]); /* outside band untouched */
    TEST_ASSERT_EQUAL_HEX8(0x00, fb[0][1]);
    TEST_ASSERT_EQUAL_HEX8(0x5A, fb[0][2]);
    TEST_ASSERT_EQUAL_HEX8(0x00, fb[0][3]);
    TEST_ASSERT_EQUAL_HEX8(0x11, fb[1][0]);
    TEST_ASSERT_EQUAL_HEX8(0xDD, fb[1][1]);
    TEST_ASSERT_EQUAL_HEX8(0xCC, fb[1][2]);
    TEST_ASSERT_EQUAL_HEX8(0x44, fb[1][3]);
}

void test_invert_byte_cols_full_width(void) {
    uint8_t fb[1][2] = {{0x0F, 0xF0}};
    display_fb_invert_byte_cols(&fb[0][0], 1, 2, 0, 1);
    TEST_ASSERT_EQUAL_HEX8(0xF0, fb[0][0]);
    TEST_ASSERT_EQUAL_HEX8(0x0F, fb[0][1]);
}

void test_invert_byte_cols_clamps_out_of_range(void) {
    uint8_t fb[1][2] = {{0xAA, 0xBB}};
    display_fb_invert_byte_cols(&fb[0][0], 1, 2, -3, 7); /* clamps to 0..1 */
    TEST_ASSERT_EQUAL_HEX8(0x55, fb[0][0]);
    TEST_ASSERT_EQUAL_HEX8(0x44, fb[0][1]);
}

/* ---- display_battery_icon_level: 0=empty .. 4=full ---- */

void test_battery_icon_boundaries(void) {
    TEST_ASSERT_EQUAL_INT(0, display_battery_icon_level(0));
    TEST_ASSERT_EQUAL_INT(0, display_battery_icon_level(10));
    TEST_ASSERT_EQUAL_INT(1, display_battery_icon_level(11));
    TEST_ASSERT_EQUAL_INT(1, display_battery_icon_level(35));
    TEST_ASSERT_EQUAL_INT(2, display_battery_icon_level(36));
    TEST_ASSERT_EQUAL_INT(2, display_battery_icon_level(60));
    TEST_ASSERT_EQUAL_INT(3, display_battery_icon_level(61));
    TEST_ASSERT_EQUAL_INT(3, display_battery_icon_level(85));
    TEST_ASSERT_EQUAL_INT(4, display_battery_icon_level(86));
    TEST_ASSERT_EQUAL_INT(4, display_battery_icon_level(100));
}

/* ---- display_button_a_label: A shows the action a press will take ---- */

void test_button_a_shows_play_when_idle(void) {
    TEST_ASSERT_EQUAL(DISPLAY_BTN_LABEL_PLAY, display_button_a_label(TIMER_IDLE));
}

void test_button_a_shows_play_when_paused(void) {
    TEST_ASSERT_EQUAL(DISPLAY_BTN_LABEL_PLAY, display_button_a_label(TIMER_PAUSED));
}

void test_button_a_shows_pause_when_running(void) {
    TEST_ASSERT_EQUAL(DISPLAY_BTN_LABEL_PAUSE, display_button_a_label(TIMER_RUNNING));
}

void test_button_a_hidden_when_expired(void) {
    /* A press does nothing in EXPIRED — advertising one would mislead */
    TEST_ASSERT_EQUAL(DISPLAY_BTN_LABEL_NONE, display_button_a_label(TIMER_EXPIRED));
}

/* display_fb_invert_dirty_rows: inverts band bytes only in rows where the
   frame differs from the previous frame — so the cleaning flash covers just
   the changed characters, not the whole band. */

void test_invert_dirty_rows_only_touches_changed_rows(void) {
    uint8_t fb[3][4] = {{0x01, 0x02, 0x03, 0x04}, {0x11, 0x22, 0x33, 0x44}, {0xAA, 0xBB, 0xCC, 0xDD}};
    uint8_t prev[3][4] = {{0x01, 0x02, 0x03, 0x04},  /* row 0: identical */
                          {0x11, 0xFF, 0x33, 0x44},  /* row 1: differs in band */
                          {0xAA, 0xBB, 0xCC, 0x00}}; /* row 2: differs OUTSIDE band */
    int dirty = display_fb_invert_dirty_rows(&fb[0][0], &prev[0][0], 3, 4, 1, 2);
    TEST_ASSERT_EQUAL_INT(1, dirty);
    TEST_ASSERT_EQUAL_HEX8(0x02, fb[0][1]); /* clean row untouched */
    TEST_ASSERT_EQUAL_HEX8(0xDD, fb[1][1]); /* dirty row: band bytes inverted */
    TEST_ASSERT_EQUAL_HEX8(0xCC, fb[1][2]);
    TEST_ASSERT_EQUAL_HEX8(0x11, fb[1][0]); /* dirty row: outside band untouched */
    TEST_ASSERT_EQUAL_HEX8(0xBB, fb[2][1]); /* diff outside band = not dirty */
}

void test_invert_dirty_rows_clamps_range(void) {
    uint8_t fb[1][2] = {{0xAA, 0xBB}};
    uint8_t prev[1][2] = {{0xAA, 0x00}};
    int dirty = display_fb_invert_dirty_rows(&fb[0][0], &prev[0][0], 1, 2, -5, 9);
    TEST_ASSERT_EQUAL_INT(1, dirty);
    TEST_ASSERT_EQUAL_HEX8(0x55, fb[0][0]);
    TEST_ASSERT_EQUAL_HEX8(0x44, fb[0][1]);
}

void test_invert_dirty_rows_no_change_returns_zero(void) {
    uint8_t fb[2][2] = {{0x12, 0x34}, {0x56, 0x78}};
    uint8_t prev[2][2] = {{0x12, 0x34}, {0x56, 0x78}};
    int dirty = display_fb_invert_dirty_rows(&fb[0][0], &prev[0][0], 2, 2, 0, 1);
    TEST_ASSERT_EQUAL_INT(0, dirty);
    TEST_ASSERT_EQUAL_HEX8(0x12, fb[0][0]);
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
    RUN_TEST(test_format_hours_minutes_seconds);
    RUN_TEST(test_format_minutes_and_seconds);
    RUN_TEST(test_format_shows_zero_when_expired);
    RUN_TEST(test_format_1_min_30_sec);
    RUN_TEST(test_invert_byte_cols_middle_column);
    RUN_TEST(test_invert_byte_cols_full_width);
    RUN_TEST(test_invert_byte_cols_clamps_out_of_range);
    RUN_TEST(test_battery_icon_boundaries);
    RUN_TEST(test_button_a_shows_play_when_idle);
    RUN_TEST(test_button_a_shows_play_when_paused);
    RUN_TEST(test_button_a_shows_pause_when_running);
    RUN_TEST(test_button_a_hidden_when_expired);
    RUN_TEST(test_invert_dirty_rows_only_touches_changed_rows);
    RUN_TEST(test_invert_dirty_rows_clamps_range);
    RUN_TEST(test_invert_dirty_rows_no_change_returns_zero);
    return UNITY_END();
}
