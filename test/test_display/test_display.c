#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

/* ---- display_button_b_label: B shows the action a press will take ----

   One function, three inputs, exactly one label: the whole decision lives
   here rather than half here and half in the screen builder, because the
   builder has no unit test of its own (only goldens). Every leg below is
   a distinct row of the truth table, including the two "gate says no"
   legs that draw nothing. */

void test_button_b_shows_play_when_idle(void) {
    TEST_ASSERT_EQUAL(DISPLAY_BTN_LABEL_PLAY, display_button_b_label(TIMER_IDLE, true, false));
}

void test_button_b_shows_play_when_paused(void) {
    TEST_ASSERT_EQUAL(DISPLAY_BTN_LABEL_PLAY, display_button_b_label(TIMER_PAUSED, true, false));
}

void test_button_b_resumes_a_paused_reloadable_slot(void) {
    /* reload_available is the raw timer_reload_allowed() gate, which is
       true for a PAUSED reloadable slot — but B resumes it, it does not
       reload it. Only EXPIRED offers the reload. */
    TEST_ASSERT_EQUAL(DISPLAY_BTN_LABEL_PLAY, display_button_b_label(TIMER_PAUSED, true, true));
}

void test_button_b_shows_play_on_the_screen_after_a_reload(void) {
    /* The screen painted one press AFTER a Reload. timer_reload() returns
       the slot to IDLE at full duration, and timer_reload_allowed() is
       still true there (not RUNNING, def still reloadable) — so this
       combination is not only reachable, it is what the device shows the
       instant a Reload lands. B must offer PLAY: leaving "Reload" on
       screen would advertise a press that no longer does anything.
       reload_available true does NOT imply EXPIRED; do not delete this as
       a duplicate of test_button_b_shows_play_when_idle. */
    TEST_ASSERT_EQUAL(DISPLAY_BTN_LABEL_PLAY, display_button_b_label(TIMER_IDLE, true, true));
}

void test_button_b_hidden_when_idle_and_start_refused(void) {
    /* Screen Break, slot not break_eligible: a press would do nothing */
    TEST_ASSERT_EQUAL(DISPLAY_BTN_LABEL_NONE, display_button_b_label(TIMER_IDLE, false, false));
}

void test_button_b_hidden_when_paused_and_start_refused(void) {
    TEST_ASSERT_EQUAL(DISPLAY_BTN_LABEL_NONE, display_button_b_label(TIMER_PAUSED, false, true));
}

void test_button_b_shows_pause_when_running(void) {
    TEST_ASSERT_EQUAL(DISPLAY_BTN_LABEL_PAUSE, display_button_b_label(TIMER_RUNNING, true, false));
}

void test_button_b_pause_is_never_gated(void) {
    /* A RUNNING slot during a break is break_eligible by construction, so
       start_available cannot legitimately be false here — but pausing is
       unconditional, and the label must not depend on the gate. */
    TEST_ASSERT_EQUAL(DISPLAY_BTN_LABEL_PAUSE, display_button_b_label(TIMER_RUNNING, false, false));
}

void test_button_b_shows_reload_when_expired_and_reloadable(void) {
    TEST_ASSERT_EQUAL(DISPLAY_BTN_LABEL_RELOAD, display_button_b_label(TIMER_EXPIRED, true, true));
}

void test_button_b_reload_ignores_the_start_gate(void) {
    /* Reload is not a start: a break refusing to start this slot must not
       hide the reload it would still accept. */
    TEST_ASSERT_EQUAL(DISPLAY_BTN_LABEL_RELOAD, display_button_b_label(TIMER_EXPIRED, false, true));
}

void test_button_b_hidden_when_expired_and_not_reloadable(void) {
    /* The Screen slot has no def and is never reloadable — a press does
       nothing, so advertising one would mislead. */
    TEST_ASSERT_EQUAL(DISPLAY_BTN_LABEL_NONE, display_button_b_label(TIMER_EXPIRED, true, false));
}

void test_button_b_hidden_during_break(void) {
    /* Cannot resume early during an enforced break */
    TEST_ASSERT_EQUAL(DISPLAY_BTN_LABEL_NONE, display_button_b_label(TIMER_BREAK, true, true));
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

/* ---- display_format_mode_line (extra timers) ---- */

void test_mode_line_extra_timer_without_completions(void) {
    char buf[48];
    display_format_mode_line(buf, sizeof(buf), "Meditation", 0, true, 600, 0);
    TEST_ASSERT_EQUAL_STRING("Meditation - 10 min", buf);
}

void test_mode_line_extra_timer_with_completions(void) {
    char buf[48];
    display_format_mode_line(buf, sizeof(buf), "Meditation", 1, true, 600, 0);
    TEST_ASSERT_EQUAL_STRING("Meditation (x1) - 10 min", buf);
    display_format_mode_line(buf, sizeof(buf), "Meditation", 2, true, 600, 0);
    TEST_ASSERT_EQUAL_STRING("Meditation (x2) - 10 min", buf);
}

void test_mode_line_non_reloadable_never_shows_counter(void) {
    char buf[48];
    display_format_mode_line(buf, sizeof(buf), "Violin", 3, false, 900, 0);
    TEST_ASSERT_EQUAL_STRING("Violin - 15 min", buf);
}

void test_mode_line_truncates_cleanly_in_small_buffer(void) {
    /* Kconfig help asks for short names but cannot enforce it — a long
       name must truncate with a terminator, never overflow. Full text is
       "ExtraLongTimerName (x12) - 90 min". */
    char buf[20];
    memset(buf, (char)0xAA, sizeof(buf));
    display_format_mode_line(buf, sizeof(buf), "ExtraLongTimerName", 12, true, 5400, 0);
    TEST_ASSERT_EQUAL_UINT(19, strlen(buf));
    TEST_ASSERT_EQUAL_MEMORY("ExtraLongTimerName ", buf, 19);
}

/* An adjustment reaches the extra timers too (a cmd-topic grant can bank
   on any slot), through the same suffix the Screen day line uses. */
void test_mode_line_carries_the_days_adjustment(void) {
    char buf[64];
    display_format_mode_line(buf, sizeof(buf), "Piano", 0, false, 900, 600);
    TEST_ASSERT_EQUAL_STRING("Piano - 15 min (+10 min today)", buf);
    display_format_mode_line(buf, sizeof(buf), "Piano", 2, true, 900, -300);
    TEST_ASSERT_EQUAL_STRING("Piano (x2) - 15 min (-5 min today)", buf);
}

/* ---- display_format_day_line (Screen, slot 0) ----

   The line reports the DAY'S DEFAULT plus what today's adjustment did to
   it, separately. Folding the adjustment into the first number (which is
   what the allocation carried once the banked bonus was added to it) made
   a 60-minute weekday with -30 applied read "Weekday - 30 min" — a figure
   that is neither the day's default nor anything the family configured. */

void test_day_line_without_an_adjustment_is_unchanged(void) {
    /* The unadjusted line must stay byte-identical to the pre-feature
       one: it is the overwhelmingly common case, and the render goldens
       pin it. */
    char buf[64];
    display_format_day_line(buf, sizeof(buf), "Weekday", 3600, 0);
    TEST_ASSERT_EQUAL_STRING("Weekday - 60 min", buf);
}

void test_day_line_reports_a_deduction_against_the_days_default(void) {
    char buf[64];
    display_format_day_line(buf, sizeof(buf), "Weekday", 3600, -1800);
    TEST_ASSERT_EQUAL_STRING("Weekday - 60 min (-30 min today)", buf);
}

void test_day_line_reports_a_grant_with_an_explicit_plus(void) {
    /* The sign is the whole point of a SIGNED adjustment control: "15 min
       today" would not say which way it went. */
    char buf[64];
    display_format_day_line(buf, sizeof(buf), "Weekend", 7200, 900);
    TEST_ASSERT_EQUAL_STRING("Weekend - 120 min (+15 min today)", buf);
}

void test_day_line_never_renders_an_empty_parenthetical(void) {
    /* Sub-minute adjustments round away rather than printing "(+0 min
       today)". Every path that sets one works in whole minutes (HA's
       number and the cmd topic are both min * 60), so this is a
       floor, not a rounding policy. */
    char buf[64];
    display_format_day_line(buf, sizeof(buf), "Weekday", 3600, 30);
    TEST_ASSERT_EQUAL_STRING("Weekday - 60 min", buf);
    display_format_day_line(buf, sizeof(buf), "Weekday", 3600, -30);
    TEST_ASSERT_EQUAL_STRING("Weekday - 60 min", buf);
}

void test_day_line_truncates_cleanly_in_a_small_buffer(void) {
    /* Same contract as the mode line: a suffix that does not fit
       terminates, never overflows. Full text is
       "Weekday - 60 min (-30 min today)". */
    char buf[20];
    memset(buf, (char)0xAA, sizeof(buf));
    display_format_day_line(buf, sizeof(buf), "Weekday", 3600, -1800);
    TEST_ASSERT_EQUAL_UINT(19, strlen(buf));
    TEST_ASSERT_EQUAL_MEMORY("Weekday - 60 min (-", buf, 19);
}

void test_day_line_suffix_does_not_fit_at_all(void) {
    /* Exactly full before the suffix: nothing is appended, and nothing is
       written past the length the caller declared. The trailing canary is
       what makes the second half of that sentence a test rather than a
       claim — the head is 16 chars plus its NUL, so the suffix has a
       one-byte window to misuse. */
    char buf[32];
    memset(buf, (char)0xAA, sizeof(buf));
    display_format_day_line(buf, 17, "Weekday", 3600, -1800);
    TEST_ASSERT_EQUAL_STRING("Weekday - 60 min", buf);
    for (size_t i = 17; i < sizeof(buf); i++)
        TEST_ASSERT_EQUAL_HEX8_MESSAGE((char)0xAA, buf[i], "wrote past the declared length");
}

/* Both formatters accepted len == 0 before the adjustment suffix existed:
   snprintf writes nothing and returns. The suffix reintroduced an
   unconditional strlen(), which on a zero-length call reads a buffer
   nothing has written to — off the end of the allocation, and past
   whatever the caller actually owns. No in-tree caller passes 0, but the
   contract held before and has to keep holding. Heap-allocated and
   deliberately unterminated so the sanitizer sees the read. */
void test_format_lines_with_a_zero_length_buffer_touch_nothing(void) {
    char *buf = malloc(8);
    TEST_ASSERT_NOT_NULL(buf);
    memset(buf, 'x', 8);
    display_format_day_line(buf, 0, "Weekday", 3600, -1800);
    display_format_mode_line(buf, 0, "Piano", 0, false, 900, 600);
    for (size_t i = 0; i < 8; i++)
        TEST_ASSERT_EQUAL_HEX8_MESSAGE('x', buf[i], "a zero-length format wrote to the buffer");
    free(buf);
}

/* ---- the bar against a BASE denominator ----

   The bar's denominator is now routinely the day's default rather than
   the effective limit, so remaining can legitimately sit ABOVE it (a
   grant) or at zero against a non-zero base (a deduction that emptied the
   day). Both ends were already clamped; these pin that they stay clamped,
   because the cases stopped being pathological. */

void test_bar_is_full_when_a_grant_pushes_remaining_past_the_base(void) {
    /* 75 min remaining against a 60 min base default. */
    TEST_ASSERT_EQUAL_UINT16(280, display_bar_fill_px(4500, 3600));
}

void test_bar_is_empty_when_a_deduction_emptied_the_day(void) {
    /* -60 applied to a 60 min day: nothing left, but the base is still 60. */
    TEST_ASSERT_EQUAL_UINT16(0, display_bar_fill_px(0, 3600));
}

/* ---- display_format_break_chip: the header chip shown while a Screen
   Break runs behind another selected timer ---- */

void test_break_chip_is_minutes_and_seconds(void) {
    char buf[24];
    display_format_break_chip(buf, sizeof(buf), 754); /* 12 min 34 s */
    TEST_ASSERT_EQUAL_STRING("BREAK 12:34", buf);
    display_format_break_chip(buf, sizeof(buf), 59);
    TEST_ASSERT_EQUAL_STRING("BREAK 0:59", buf);
}

void test_break_chip_stays_in_minutes_past_an_hour(void) {
    /* Break durations are bounded (minutes, configured in HA) — rolling
       over to H:MM:SS would only cost width in a chip that has none. */
    char buf[24];
    display_format_break_chip(buf, sizeof(buf), 3600);
    TEST_ASSERT_EQUAL_STRING("BREAK 60:00", buf);
    display_format_break_chip(buf, sizeof(buf), 7200); /* the 120 min bound */
    TEST_ASSERT_EQUAL_STRING("BREAK 120:00", buf);
}

void test_break_chip_clamps_at_zero(void) {
    char buf[24];
    display_format_break_chip(buf, sizeof(buf), 0);
    TEST_ASSERT_EQUAL_STRING("BREAK 0:00", buf);
    display_format_break_chip(buf, sizeof(buf), -30);
    TEST_ASSERT_EQUAL_STRING("BREAK 0:00", buf);
}

/* ---- display_format_hm: the frozen screen time on the break screen.
   Seconds are dropped on purpose — the screen timer is frozen for the
   whole break, so they would never change, and the bottom row has to fit
   three 16 pt items. ---- */

void test_format_hm_drops_the_seconds(void) {
    char buf[16];
    display_format_hm(buf, sizeof(buf), 5400); /* 1 h 30 min */
    TEST_ASSERT_EQUAL_STRING("1:30", buf);
    display_format_hm(buf, sizeof(buf), 5459); /* truncates, never rounds up */
    TEST_ASSERT_EQUAL_STRING("1:30", buf);
    display_format_hm(buf, sizeof(buf), 600);
    TEST_ASSERT_EQUAL_STRING("0:10", buf);
}

void test_format_hm_clamps_at_zero(void) {
    char buf[16];
    display_format_hm(buf, sizeof(buf), 0);
    TEST_ASSERT_EQUAL_STRING("0:00", buf);
    display_format_hm(buf, sizeof(buf), -90);
    TEST_ASSERT_EQUAL_STRING("0:00", buf);
}

/* ---- display_format_swap_hint: Button C's label on the break screen ---- */

void test_swap_hint_truncates_to_the_width_budget(void) {
    /* The break screen's bottom row packs three 16 pt items; the name
       gets DISPLAY_SWAP_HINT_MAX chars and no more. */
    char buf[24];
    display_format_swap_hint(buf, sizeof(buf), "Meditation");
    TEST_ASSERT_EQUAL_STRING("Meditati", buf);
    TEST_ASSERT_EQUAL_UINT(DISPLAY_SWAP_HINT_MAX, strlen(buf));
}

void test_swap_hint_passes_short_names_through(void) {
    char buf[24];
    display_format_swap_hint(buf, sizeof(buf), "Piano");
    TEST_ASSERT_EQUAL_STRING("Piano", buf);
    display_format_swap_hint(buf, sizeof(buf), "Exercise"); /* exactly the budget */
    TEST_ASSERT_EQUAL_STRING("Exercise", buf);
}

void test_swap_hint_handles_no_name(void) {
    char buf[24];
    memset(buf, (char)0xAA, sizeof(buf));
    display_format_swap_hint(buf, sizeof(buf), NULL);
    TEST_ASSERT_EQUAL_STRING("", buf);
    display_format_swap_hint(buf, sizeof(buf), "");
    TEST_ASSERT_EQUAL_STRING("", buf);
}

void test_swap_hint_respects_a_small_buffer(void) {
    char buf[4];
    memset(buf, (char)0xAA, sizeof(buf));
    display_format_swap_hint(buf, sizeof(buf), "Meditation");
    TEST_ASSERT_EQUAL_STRING("Med", buf);
}

/* ---- display_format_version: the battery row and the update screen ---- */

/* Shares copy_bounded() with the swap hint, so these four cases mirror the
   four above deliberately: they are what would catch the shared helper
   being changed for one caller's benefit at the other's expense. */
void test_version_truncates_to_the_display_budget(void) {
    char buf[64];
    /* A real build-id version: 28 characters, well past the budget. */
    display_format_version(buf, sizeof(buf), "1.5.0-dirty-20260811-abcdef0");
    TEST_ASSERT_EQUAL_STRING("1.5.0-dirty-", buf);
    TEST_ASSERT_EQUAL_size_t(DISPLAY_VERSION_MAX, strlen(buf));
}

void test_version_passes_short_strings_through(void) {
    char buf[64];
    display_format_version(buf, sizeof(buf), "1.5.0");
    TEST_ASSERT_EQUAL_STRING("1.5.0", buf);
    display_format_version(buf, sizeof(buf), "10.20.30-rc4"); /* exactly the budget */
    TEST_ASSERT_EQUAL_STRING("10.20.30-rc4", buf);
}

void test_version_handles_no_version(void) {
    char buf[64];
    memset(buf, (char)0xAA, sizeof(buf));
    display_format_version(buf, sizeof(buf), NULL);
    TEST_ASSERT_EQUAL_STRING("", buf);
    display_format_version(buf, sizeof(buf), "");
    TEST_ASSERT_EQUAL_STRING("", buf);
}

void test_version_respects_a_small_buffer(void) {
    /* The buffer wins over the budget when it is the tighter of the two. */
    char buf[4];
    memset(buf, (char)0xAA, sizeof(buf));
    display_format_version(buf, sizeof(buf), "1.5.0-rc1");
    TEST_ASSERT_EQUAL_STRING("1.5", buf);
}

/* ---- display_screen_for: which full-screen layout a paint selects -------

   This is the whole precedence rule, and the golden suite cannot test it:
   the goldens call the three builders directly, so nothing over there ever
   exercises the dispatch. These do. ---- */

/* CHORES over BREAK, the fork M2-T4 had to settle. Design §2.6 puts an
   "A -> Chores" cell on the break screen and §4.2 lets chore mode in
   throughout a break (a BREAK is not RUNNING); if BREAK outranked CHORES,
   that press would repaint the break screen and the prompt would lead
   nowhere. */
void test_chore_mode_outranks_the_break_screen(void) {
    TEST_ASSERT_EQUAL_INT(DISPLAY_SCREEN_CHORES, display_screen_for(TIMER_BREAK, APP_MODE_CHORES, 3));
    /* The negative control: the same break in Timers mode is still the
       break screen — the screen M2-T6 decorates. */
    TEST_ASSERT_EQUAL_INT(DISPLAY_SCREEN_BREAK, display_screen_for(TIMER_BREAK, APP_MODE_TIMERS, 3));
}

/* §4.2: "you cannot tick off dishes away while the TV clock ticks". Falls
   back to the timer screen rather than painting a checklist whose Timers
   label sits on a button button_a_toggle_allowed() would refuse. */
void test_a_running_timer_falls_back_to_the_timer_screen(void) {
    TEST_ASSERT_EQUAL_INT(DISPLAY_SCREEN_MAIN, display_screen_for(TIMER_RUNNING, APP_MODE_CHORES, 3));
    /* Every other state chore mode is reachable from still paints it, so
       the fallback is RUNNING's alone and not a blanket refusal. */
    TEST_ASSERT_EQUAL_INT(DISPLAY_SCREEN_CHORES, display_screen_for(TIMER_IDLE, APP_MODE_CHORES, 3));
    TEST_ASSERT_EQUAL_INT(DISPLAY_SCREEN_CHORES, display_screen_for(TIMER_PAUSED, APP_MODE_CHORES, 3));
    TEST_ASSERT_EQUAL_INT(DISPLAY_SCREEN_CHORES, display_screen_for(TIMER_EXPIRED, APP_MODE_CHORES, 3));
}

/* Row C1's off switch. An emptied or unreadable list arrives here as
   count 0 (chore_store_load_names zeroes the count on any failure), and
   must degrade to the timer screen, not to an empty checklist. */
void test_no_configured_chores_never_paints_the_checklist(void) {
    TEST_ASSERT_EQUAL_INT(DISPLAY_SCREEN_MAIN, display_screen_for(TIMER_IDLE, APP_MODE_CHORES, 0));
    TEST_ASSERT_EQUAL_INT(DISPLAY_SCREEN_BREAK, display_screen_for(TIMER_BREAK, APP_MODE_CHORES, 0));
    TEST_ASSERT_EQUAL_INT(DISPLAY_SCREEN_CHORES, display_screen_for(TIMER_IDLE, APP_MODE_CHORES, 1));
}

/* The stored mode byte is NOT clamped (app_mode_t, timer.h), so anything
   that is not APP_MODE_CHORES has to paint Timers rather than fall through
   a switch into the checklist. */
void test_an_out_of_range_mode_byte_paints_the_timer_screen(void) {
    TEST_ASSERT_EQUAL_INT(DISPLAY_SCREEN_MAIN, display_screen_for(TIMER_IDLE, (app_mode_t)7, 3));
    TEST_ASSERT_EQUAL_INT(DISPLAY_SCREEN_BREAK, display_screen_for(TIMER_BREAK, (app_mode_t)7, 3));
}

/* ---- display_chore_row_ticked ---- */

void test_chore_rows_draw_their_own_ack_bits(void) {
    TEST_ASSERT_TRUE(display_chore_row_ticked(0x01, 3, 0));
    TEST_ASSERT_FALSE(display_chore_row_ticked(0x01, 3, 1));
    TEST_ASSERT_FALSE(display_chore_row_ticked(0x01, 3, 2));
    TEST_ASSERT_TRUE(display_chore_row_ticked(0x06, 3, 1));
    TEST_ASSERT_TRUE(display_chore_row_ticked(0x06, 3, 2));
}

/* THE trap this helper exists for: the stored mask keeps bits at and above
   the configured count RAW, so a list shortened from three to two leaves
   bit 2 set. Testing it directly would render a tick against a row that is
   not a chore any more. */
void test_a_stale_bit_above_the_count_never_draws_a_tick(void) {
    TEST_ASSERT_FALSE(display_chore_row_ticked(0x07, 2, 2));
    TEST_ASSERT_FALSE(display_chore_row_ticked(0x07, 0, 0));
    TEST_ASSERT_FALSE(display_chore_row_ticked(0xFF, 1, 1));
    /* and the rows that ARE configured still tick */
    TEST_ASSERT_TRUE(display_chore_row_ticked(0x07, 2, 0));
    TEST_ASSERT_TRUE(display_chore_row_ticked(0x07, 2, 1));
}

void test_a_row_index_past_the_hardware_cap_is_refused(void) {
    TEST_ASSERT_FALSE(display_chore_row_ticked(0xFF, CHORE_MAX, CHORE_MAX));
    TEST_ASSERT_FALSE(display_chore_row_ticked(0xFF, 200, 200));
}

/* ---- display_format_chore_count ---- */

void test_chore_count_reads_n_of_the_configured_length(void) {
    char buf[16];
    display_format_chore_count(buf, sizeof(buf), 0x00, 3);
    TEST_ASSERT_EQUAL_STRING("0 of 3", buf);
    display_format_chore_count(buf, sizeof(buf), 0x01, 3);
    TEST_ASSERT_EQUAL_STRING("1 of 3", buf);
    display_format_chore_count(buf, sizeof(buf), 0x05, 3);
    TEST_ASSERT_EQUAL_STRING("2 of 3", buf);
    /* §2.4: on the last ack the header becomes "3 of 3". */
    display_format_chore_count(buf, sizeof(buf), 0x07, 3);
    TEST_ASSERT_EQUAL_STRING("3 of 3", buf);
}

void test_chore_count_follows_a_shorter_list(void) {
    char buf[16];
    display_format_chore_count(buf, sizeof(buf), 0x01, 2);
    TEST_ASSERT_EQUAL_STRING("1 of 2", buf);
    /* the stale high bit is not counted either — the header would
       otherwise read "2 of 2" over a screen showing one tick */
    display_format_chore_count(buf, sizeof(buf), 0x05, 2);
    TEST_ASSERT_EQUAL_STRING("1 of 2", buf);
}

void test_chore_count_with_a_zero_length_buffer_touches_nothing(void) {
    char buf[4];
    memset(buf, (char)0xAA, sizeof(buf));
    display_format_chore_count(buf, 0, 0x07, 3);
    TEST_ASSERT_EQUAL_HEX8((char)0xAA, buf[0]);
}

/* ---- display_chore_unlocked ---- */

void test_unlocked_on_the_last_ack_before_the_latch_is_written(void) {
    TEST_ASSERT_TRUE(display_chore_unlocked(0x07, 3, false));
    TEST_ASSERT_FALSE(display_chore_unlocked(0x03, 3, false));
    TEST_ASSERT_FALSE(display_chore_unlocked(0x00, 3, false));
}

/* C8: acks toggle, the release latches. Un-ticking a chore after the day
   released puts a row back outstanding while the screen time stays
   granted — the line must not claim the gate re-shut. */
void test_unlocked_stays_true_after_an_ack_is_toggled_back_off(void) {
    TEST_ASSERT_TRUE(display_chore_unlocked(0x03, 3, true));
    TEST_ASSERT_TRUE(display_chore_unlocked(0x00, 3, true));
}

/* With no chores nothing was ever locked, so nothing is unlocked — and a
   stale mask against an emptied list must not announce otherwise. */
void test_no_chores_is_never_unlocked(void) {
    TEST_ASSERT_FALSE(display_chore_unlocked(0x00, 0, false));
    TEST_ASSERT_FALSE(display_chore_unlocked(0x07, 0, false));
    TEST_ASSERT_FALSE(display_chore_unlocked(0x07, 0, true));
}

/* A shortened list: bits above the count are raw, so all-acked has to be
   decided over the configured rows only. */
void test_unlocked_ignores_bits_above_the_configured_count(void) {
    TEST_ASSERT_TRUE(display_chore_unlocked(0x07, 2, false)); /* both real rows acked */
    TEST_ASSERT_FALSE(display_chore_unlocked(0x04, 2, false));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_break_chip_is_minutes_and_seconds);
    RUN_TEST(test_break_chip_stays_in_minutes_past_an_hour);
    RUN_TEST(test_break_chip_clamps_at_zero);
    RUN_TEST(test_format_hm_drops_the_seconds);
    RUN_TEST(test_format_hm_clamps_at_zero);
    RUN_TEST(test_swap_hint_truncates_to_the_width_budget);
    RUN_TEST(test_swap_hint_passes_short_names_through);
    RUN_TEST(test_swap_hint_handles_no_name);
    RUN_TEST(test_swap_hint_respects_a_small_buffer);
    RUN_TEST(test_version_truncates_to_the_display_budget);
    RUN_TEST(test_version_passes_short_strings_through);
    RUN_TEST(test_version_handles_no_version);
    RUN_TEST(test_version_respects_a_small_buffer);
    RUN_TEST(test_mode_line_extra_timer_without_completions);
    RUN_TEST(test_mode_line_extra_timer_with_completions);
    RUN_TEST(test_mode_line_non_reloadable_never_shows_counter);
    RUN_TEST(test_mode_line_truncates_cleanly_in_small_buffer);
    RUN_TEST(test_mode_line_carries_the_days_adjustment);
    RUN_TEST(test_day_line_without_an_adjustment_is_unchanged);
    RUN_TEST(test_day_line_reports_a_deduction_against_the_days_default);
    RUN_TEST(test_day_line_reports_a_grant_with_an_explicit_plus);
    RUN_TEST(test_day_line_never_renders_an_empty_parenthetical);
    RUN_TEST(test_day_line_truncates_cleanly_in_a_small_buffer);
    RUN_TEST(test_day_line_suffix_does_not_fit_at_all);
    RUN_TEST(test_format_lines_with_a_zero_length_buffer_touch_nothing);
    RUN_TEST(test_bar_is_full_when_a_grant_pushes_remaining_past_the_base);
    RUN_TEST(test_bar_is_empty_when_a_deduction_emptied_the_day);
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
    RUN_TEST(test_button_b_shows_play_when_idle);
    RUN_TEST(test_button_b_shows_play_when_paused);
    RUN_TEST(test_button_b_resumes_a_paused_reloadable_slot);
    RUN_TEST(test_button_b_shows_play_on_the_screen_after_a_reload);
    RUN_TEST(test_button_b_hidden_when_idle_and_start_refused);
    RUN_TEST(test_button_b_hidden_when_paused_and_start_refused);
    RUN_TEST(test_button_b_shows_pause_when_running);
    RUN_TEST(test_button_b_pause_is_never_gated);
    RUN_TEST(test_button_b_shows_reload_when_expired_and_reloadable);
    RUN_TEST(test_button_b_reload_ignores_the_start_gate);
    RUN_TEST(test_button_b_hidden_when_expired_and_not_reloadable);
    RUN_TEST(test_button_b_hidden_during_break);
    RUN_TEST(test_invert_dirty_rows_only_touches_changed_rows);
    RUN_TEST(test_invert_dirty_rows_clamps_range);
    RUN_TEST(test_invert_dirty_rows_no_change_returns_zero);
    RUN_TEST(test_chore_mode_outranks_the_break_screen);
    RUN_TEST(test_a_running_timer_falls_back_to_the_timer_screen);
    RUN_TEST(test_no_configured_chores_never_paints_the_checklist);
    RUN_TEST(test_an_out_of_range_mode_byte_paints_the_timer_screen);
    RUN_TEST(test_chore_rows_draw_their_own_ack_bits);
    RUN_TEST(test_a_stale_bit_above_the_count_never_draws_a_tick);
    RUN_TEST(test_a_row_index_past_the_hardware_cap_is_refused);
    RUN_TEST(test_chore_count_reads_n_of_the_configured_length);
    RUN_TEST(test_chore_count_follows_a_shorter_list);
    RUN_TEST(test_chore_count_with_a_zero_length_buffer_touches_nothing);
    RUN_TEST(test_unlocked_on_the_last_ack_before_the_latch_is_written);
    RUN_TEST(test_unlocked_stays_true_after_an_ack_is_toggled_back_off);
    RUN_TEST(test_no_chores_is_never_unlocked);
    RUN_TEST(test_unlocked_ignores_bits_above_the_configured_count);
    return UNITY_END();
}
