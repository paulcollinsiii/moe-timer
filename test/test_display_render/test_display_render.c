/* Golden-render tests: build the real LVGL screens on the host, capture
   the I1 framebuffer, and compare byte-for-byte against committed golden
   files. Regenerate after an intentional layout change with:
       MAGTAG_WRITE_GOLDEN=1 ctest --test-dir test/build -R test_display_render
   then eyeball the change (scripts or a hexdump diff) and commit the .bin. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unity.h>

#include "lvgl.h"

/* Single-TU compilation of the layout math + screen builders */
// clang-format off
#include "../../main/display_layout.c"
#include "../../main/display_screens.c"
// clang-format on

#define HOR 296
#define VER 128
#define FB_BYTES (HOR * VER / 8)

static uint8_t s_lvbuf[8 + FB_BYTES]; /* 8-byte I1 palette header */
static uint8_t s_captured[FB_BYTES];
static lv_display_t *s_disp;

static uint32_t tick_cb(void) {
    static uint32_t t;
    return t += 10;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    (void)area; /* RENDER_MODE_FULL: whole frame */
    memcpy(s_captured, px_map + 8, FB_BYTES);
    lv_display_flush_ready(disp);
}

void setUp(void) {
    /* Deterministic header text regardless of host TZ */
    setenv("TZ", "UTC0", 1);
    tzset();
}

void tearDown(void) {}

/* Monday 2026-01-05 15:04:05 UTC — fixed header date/time */
#define WALL ((time_t)1767625445)
#define SYNC ((time_t)1767624000) /* 14:40 */

static display_state_t base_state(void) {
    return (display_state_t){
        .remaining_sec = 3600,
        .allocation_sec = 3600,
        .timer_state = TIMER_IDLE,
        .day_type = DAY_WEEKDAY,
        .wall_time = WALL,
        .last_sync_time = SYNC,
        .battery_pct = 87,
        .break_duration_sec = 900,
        .swap_available = true,
    };
}

static void assert_matches_golden(const char *name) {
    lv_refr_now(s_disp); /* render + flush into s_captured */

    /* A blank frame means the render silently failed — catch it even in
       golden-write mode. */
    int blank = 1;
    for (size_t i = 0; i < FB_BYTES && blank; i++)
        blank = (s_captured[i] == s_captured[0]);
    TEST_ASSERT_FALSE_MESSAGE(blank, "captured framebuffer is uniform - render produced nothing");

    char path[512];
    snprintf(path, sizeof(path), "%s/%s.bin", GOLDEN_DIR, name);

    if (getenv("MAGTAG_WRITE_GOLDEN") != NULL) {
        FILE *f = fopen(path, "wb");
        TEST_ASSERT_NOT_NULL_MESSAGE(f, "cannot open golden file for writing");
        fwrite(s_captured, 1, FB_BYTES, f);
        fclose(f);
        return; /* written = passed */
    }

    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "golden file missing - run with MAGTAG_WRITE_GOLDEN=1 to create");
    static uint8_t golden[FB_BYTES];
    size_t n = fread(golden, 1, FB_BYTES, f);
    fclose(f);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(FB_BYTES, n, "golden file has wrong size");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(golden, s_captured, FB_BYTES,
                                     "render differs from golden - if the layout change is intentional, "
                                     "regenerate with MAGTAG_WRITE_GOLDEN=1 and review");
}

/* ---- scenarios ---- */

void test_main_idle_weekday(void) {
    display_state_t st = base_state();
    display_screens_build_main(&st);
    assert_matches_golden("main_idle_weekday");
}

void test_main_running_meditation_x2(void) {
    display_state_t st = base_state();
    st.timer_state = TIMER_RUNNING;
    st.timer_name = "Meditation";
    st.completions = 2;
    st.reloadable = true;
    st.allocation_sec = 600;
    st.remaining_sec = 400;
    st.swap_available = false; /* RUNNING: swap and reload disabled */
    st.reload_available = false;
    display_screens_build_main(&st);
    assert_matches_golden("main_running_meditation_x2");
}

void test_main_paused_reloadable(void) {
    display_state_t st = base_state();
    st.timer_state = TIMER_PAUSED;
    st.timer_name = "Piano";
    st.reloadable = true;
    st.allocation_sec = 900;
    st.remaining_sec = 700;
    st.reload_available = true; /* Reset label visible */
    display_screens_build_main(&st);
    assert_matches_golden("main_paused_reloadable");
}

void test_main_expired(void) {
    display_state_t st = base_state();
    st.timer_state = TIMER_EXPIRED;
    st.remaining_sec = 0;
    display_screens_build_main(&st);
    assert_matches_golden("main_expired");
}

void test_break_screen(void) {
    display_state_t st = base_state();
    st.timer_state = TIMER_BREAK;
    st.remaining_sec = 1800;
    st.break_remaining_sec = 700;
    display_screens_build_break(&st);
    assert_matches_golden("break_screen");
}

void test_main_low_battery_warn_badge(void) {
    /* <= 15%: the progress bar carries the Charge Me!!! badge */
    display_state_t st = base_state();
    st.battery_pct = 15;
    st.charge_warn = true;
    display_screens_build_main(&st);
    assert_matches_golden("main_warn_badge");
}

void test_charge_me_screen(void) {
    /* <= 10%: full stop — the panel says only Charge Me! */
    display_screens_build_charge_me();
    assert_matches_golden("charge_me");
}

void test_timesup_screen(void) {
    display_screens_build_timesup();
    assert_matches_golden("timesup");
}

void test_sync_failed_screen(void) {
    display_screens_build_sync_failed();
    assert_matches_golden("sync_failed");
}

int main(void) {
    lv_init();
    lv_tick_set_cb(tick_cb);
    s_disp = lv_display_create(HOR, VER);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_I1);
    lv_display_set_buffers(s_disp, s_lvbuf, NULL, sizeof(s_lvbuf), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(s_disp, flush_cb);

    UNITY_BEGIN();
    RUN_TEST(test_main_idle_weekday);
    RUN_TEST(test_main_running_meditation_x2);
    RUN_TEST(test_main_paused_reloadable);
    RUN_TEST(test_main_expired);
    RUN_TEST(test_break_screen);
    RUN_TEST(test_main_low_battery_warn_badge);
    RUN_TEST(test_charge_me_screen);
    RUN_TEST(test_timesup_screen);
    RUN_TEST(test_sync_failed_screen);
    return UNITY_END();
}
