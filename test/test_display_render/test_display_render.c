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
        .start_available = true,
        /* Fixed injected version — display_screens.c never reads the app
           descriptor, which is what keeps these goldens deterministic. */
        .fw_version = "1.5.0",
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
    /* Break screen with extras configured: the bottom row offers the swap
       (Button A stays unlabelled — the break is still enforced). */
    display_state_t st = base_state();
    st.timer_state = TIMER_BREAK;
    st.remaining_sec = 5400; /* 1:30:00 of screen time frozen */
    st.break_remaining_sec = 700;
    st.swap_next_name = "Piano";
    display_screens_build_break(&st);
    assert_matches_golden("break_screen");
}

void test_break_screen_no_extras(void) {
    /* No extra timers configured: nothing to swap to, so the screen keeps
       its centred "Timer paused" footer and no button row. */
    display_state_t st = base_state();
    st.timer_state = TIMER_BREAK;
    st.remaining_sec = 5400;
    st.break_remaining_sec = 700;
    st.swap_next_name = NULL;
    display_screens_build_break(&st);
    assert_matches_golden("break_screen_no_extras");
}

/* Landscape rows that must stay empty between the header band (rows 3..17,
   framebuffer bytes 0..2) and the progress-bar band (rows 26..49, bytes
   3..6). CLEAN_BANDS in display.c must not share a framebuffer byte — a
   shared byte is inverted twice by the ghost-cleaning double partial and
   cancels out — and byte 3 starts at row 24. The chip is the only widget
   that could grow down into it. */
static void assert_rows_blank(int row0, int row1) {
    for (int r = row0; r <= row1; r++) {
        for (int b = 0; b < HOR / 8; b++) {
            char msg[64];
            snprintf(msg, sizeof(msg), "row %d byte %d is not blank", r, b);
            /* LVGL I1: 1 = white */
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xFF, s_captured[r * (HOR / 8) + b], msg);
        }
    }
}

void test_main_break_chip(void) {
    /* Break running behind a selected Piano: the header's Last sync is
       replaced by the inverted BREAK chip. Piano is RUNNING, so the swap
       is refused and C carries no label (state truth table). */
    display_state_t st = base_state();
    st.timer_state = TIMER_RUNNING;
    st.timer_name = "Piano";
    st.allocation_sec = 600;
    st.remaining_sec = 450;
    st.swap_available = false;
    st.reload_available = false;
    st.break_banner = true;
    st.break_remaining_sec = 754; /* 12:34 */
    display_screens_build_main(&st);
    assert_matches_golden("main_break_chip");
    assert_rows_blank(24, 25); /* chip stays inside the header band's bytes */
}

void test_main_break_chip_no_start(void) {
    /* A break running behind a selected chore (not break_eligible):
       Button A carries no play glyph, because a press would be refused.

       This is a DIFFERENT scenario from test_main_break_chip, not a
       one-field variant of it — the two states differ in the timer name,
       state, allocation, remaining and reload label as well, so do not
       read a diff of the two goldens as "what start_available does".
       test_start_available_only_changes_button_a below is what isolates
       that. */
    display_state_t st = base_state();
    st.timer_state = TIMER_PAUSED;
    st.timer_name = "Laundry folding";
    st.allocation_sec = 1500;
    st.remaining_sec = 750;
    st.reload_available = true;
    st.break_banner = true;
    st.break_remaining_sec = 372; /* 6:12 */
    st.start_available = false;
    display_screens_build_main(&st);
    assert_matches_golden("main_break_chip_no_start");
    assert_rows_blank(24, 25);
}

void test_break_screen_no_eligible(void) {
    /* Extras exist but none is break-eligible, so the break has nothing
       to offer: app_state suppresses the hint (swap_next_name NULL) and
       the screen falls back to the centred footer — the pre-non-blocking
       locking break, which is the right behaviour here. */
    display_state_t st = base_state();
    st.timer_state = TIMER_BREAK;
    st.remaining_sec = 5400;
    st.break_remaining_sec = 700;
    st.swap_next_name = NULL;
    display_screens_build_break(&st);
    assert_matches_golden("break_screen_no_eligible");
}

/* Isolates the flag itself: one state rendered twice, differing only in
   start_available, must differ only inside Button A's cell. Catches a
   layout that reflows when the glyph disappears — which a golden pair of
   two different scenarios cannot. */
void test_start_available_only_changes_button_a(void) {
    static uint8_t with_glyph[FB_BYTES];
    display_state_t st = base_state();
    st.timer_state = TIMER_PAUSED;
    st.timer_name = "Laundry folding";
    st.allocation_sec = 1500;
    st.remaining_sec = 750;
    st.reload_available = true;
    st.break_banner = true;
    st.break_remaining_sec = 372;

    st.start_available = true;
    display_screens_build_main(&st);
    lv_refr_now(s_disp);
    memcpy(with_glyph, s_captured, FB_BYTES);

    st.start_available = false;
    display_screens_build_main(&st);
    lv_refr_now(s_disp);

    /* Button A's label is centred on x=17 (BTN_X0), so it lives in the
       first four byte columns of the bottom label rows. */
    int differing = 0;
    for (int r = 0; r < VER; r++) {
        for (int b = 0; b < HOR / 8; b++) {
            int i = r * (HOR / 8) + b;
            if (with_glyph[i] == s_captured[i])
                continue;
            differing++;
            char msg[80];
            snprintf(msg, sizeof(msg), "row %d byte %d changed outside Button A's cell", r, b);
            TEST_ASSERT_TRUE_MESSAGE(r >= 112 && b < 4, msg);
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(differing > 0, "start_available changed nothing at all");
}

void test_main_low_battery_warn_badge(void) {
    /* <= 15%: the progress bar carries the Charge Me!!! badge */
    display_state_t st = base_state();
    st.battery_pct = 15;
    st.charge_warn = true;
    display_screens_build_main(&st);
    assert_matches_golden("main_warn_badge");
}

/* The version rides the battery label, so the only two things that can go
   wrong are a buffer overflow (bounded by the format's own arithmetic) and
   a collision with the 28 pt remaining-time label sharing the {58,87}
   band. Measure both labels' real extents rather than eyeballing the
   golden. The battery label is the one aligned at y=66, the remaining time
   the one at y=58 — unambiguous within this screen. */
static void measure_battery_row(const display_state_t *st, int32_t *batt_right, int32_t *rem_left) {
    display_screens_build_main(st);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_update_layout(scr);
    *batt_right = -1;
    *rem_left = -1;
    uint32_t n = lv_obj_get_child_count(scr);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *o = lv_obj_get_child(scr, i);
        if (!lv_obj_check_type(o, &lv_label_class))
            continue;
        if (lv_obj_get_y(o) == 66)
            *batt_right = lv_obj_get_x(o) + lv_obj_get_width(o);
        else if (lv_obj_get_y(o) == 58)
            *rem_left = lv_obj_get_x(o);
    }
    TEST_ASSERT_NOT_EQUAL_MESSAGE(-1, *batt_right, "battery label (y=66) not found");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(-1, *rem_left, "remaining-time label (y=58) not found");
}

/* Every version this row can be asked to render, against the widest the
   28 pt remaining time ever gets. OTA_VERSION_MAX (ota_policy.h) is 32, so
   31 characters is publishable and therefore reachable — untruncated it
   overstruck the time label by 106 px, which on 1 bpp e-ink is two black
   strings on top of each other, not a graceful clip.

   The pathological rows are the point: a character budget alone cannot
   bound the rendered width (a 12 pt digit advances ~8 px, 'W' ~14), which
   is why the label also carries a hard width cap. */
static const struct {
    const char *ver;
    const char *what;
} VERSION_CASES[] = {
    {"", "empty"},
    {"1.5.0", "typical release"},
    {"1.10.10-rc1", "pre-release"},
    {"1.5.0-dirty-20260811-abcdef0", "28-char build id"},
    {"0000000000000000000000000000000", "31 digits (OTA_VERSION_MAX-1)"},
    {"WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW", "31 'W' - widest glyph in the font"},
    {"mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm", "31 'm' - widest lowercase"},
};

void test_version_fits_the_battery_row(void) {
    display_state_t st = base_state();
    st.battery_pct = 100;      /* three digits + the full-battery glyph */
    st.remaining_sec = 359999; /* 99:59:59, the widest time the panel renders */
    for (size_t i = 0; i < sizeof(VERSION_CASES) / sizeof(VERSION_CASES[0]); i++) {
        st.fw_version = VERSION_CASES[i].ver;
        int32_t batt_right, rem_left;
        measure_battery_row(&st, &batt_right, &rem_left);
        printf("battery row [%2d ch, %-32s] ends x=%3d, time starts x=%d, gap=%d px\n",
               (int)strlen(VERSION_CASES[i].ver), VERSION_CASES[i].what, (int)batt_right, (int)rem_left,
               (int)(rem_left - batt_right));
        char msg[160];
        snprintf(msg, sizeof(msg), "%s: battery+version ends at x=%d, remaining-time label starts at x=%d",
                 VERSION_CASES[i].what, (int)batt_right, (int)rem_left);
        TEST_ASSERT_TRUE_MESSAGE(batt_right < rem_left, msg);
        TEST_ASSERT_TRUE_MESSAGE(batt_right <= HOR, "battery+version label runs off the right edge");
    }
}

/* Settles, against the real LVGL this suite links, that the geometric cap
   is not merely advisory. lv_obj_set_style_max_width() bounds the OBJECT;
   this asserts the DRAWING is bounded too, which is the property the row
   actually depends on and the one that could differ between LVGL versions.

   Renders the pathological 31-'W' version — 502 px of text uncapped, i.e.
   past the panel's right edge and straight through the remaining-time
   label — and requires every pixel from the cap's right edge (x=172) up to
   the time label's left edge to be white. LV_LABEL_LONG_CLIP is also under
   test here: LV_LABEL_LONG_DOT wraps a content-sized label to three lines
   (h 15 -> 45), which this catches as ink below the battery row's band. */
/* display_screens.c aligns the battery label at x=4 and caps it at 168 px
   (BATT_ROW_MAX_W), so its right edge cannot pass 172; the 28 pt time
   label's left edge bottoms out at 180. Mirrored here as literals on
   purpose — a test that recomputed them from the source constants would
   move in lockstep with a mistake in them. */
#define BATT_ROW_CAP_RIGHT 172
#define TIME_LABEL_LEFT 180

void test_version_cap_clips_the_drawing_not_just_the_object(void) {
    display_state_t st = base_state();
    st.battery_pct = 100;
    st.remaining_sec = 359999;
    st.fw_version = "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW"; /* 31 */

    int32_t batt_right, rem_left;
    measure_battery_row(&st, &batt_right, &rem_left);
    lv_refr_now(s_disp);
    printf("cap check: object ends x=%d, time label starts x=%d\n", (int)batt_right, (int)rem_left);

    /* Bounds are the CONSTANTS, never the measured edge: deriving the
       corridor from batt_right would make the scan vacuous exactly when
       the cap has failed, because an overrunning label pushes its own
       right edge past the corridor's end and the loop stops running. */
    TEST_ASSERT_TRUE_MESSAGE(batt_right <= BATT_ROW_CAP_RIGHT, "capped label is wider than the cap allows");
    for (int32_t y = 58; y <= 91; y++) {
        for (int32_t x = BATT_ROW_CAP_RIGHT; x < TIME_LABEL_LEFT; x++) {
            int bit = (s_captured[y * (HOR / 8) + x / 8] >> (7 - (x & 7))) & 1;
            char msg[96];
            snprintf(msg, sizeof(msg), "ink at x=%d y=%d - capped label spilled past x=%d", (int)x, (int)y,
                     BATT_ROW_CAP_RIGHT);
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(1, bit, msg); /* LVGL I1: 1 = white */
        }
    }

    /* And nothing wrapped down into the mode/state row's band. */
    assert_rows_blank(92, 94);
}

void test_no_version_renders_the_row_unchanged(void) {
    /* NULL (nothing injected) and "" must both drop the separator
       entirely — no trailing whitespace widening the label, and no %s on a
       null pointer. */
    static uint8_t with_null[FB_BYTES];
    display_state_t st = base_state();
    st.fw_version = NULL;
    display_screens_build_main(&st);
    lv_refr_now(s_disp);
    memcpy(with_null, s_captured, FB_BYTES);

    st.fw_version = "";
    display_screens_build_main(&st);
    lv_refr_now(s_disp);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(with_null, s_captured, FB_BYTES, "NULL and empty version render differently");

    int32_t batt_right, rem_left;
    st.fw_version = NULL;
    measure_battery_row(&st, &batt_right, &rem_left);
    int32_t bare = batt_right;
    st.fw_version = "1.5.0";
    measure_battery_row(&st, &batt_right, &rem_left);
    TEST_ASSERT_TRUE_MESSAGE(batt_right > bare, "the version added no width - it is not being rendered");
}

void test_ota_screen(void) {
    /* Firmware update, full refresh: both versions, direction-neutral verb
       (the policy deliberately supports downgrades). */
    display_screens_build_ota("1.5.0", "1.6.0");
    assert_matches_golden("ota");
}

/* Design margin for the OTA screen: the 28 pt title renders 279 px on a
   296 px panel, i.e. 8 px a side. Asserting merely "<= HOR" would let a
   296 px line pass while silently eating that margin, so the bound is the
   margin, not the panel. */
#define OTA_MAX_LINE_W (HOR - 16)

static void assert_ota_lines_fit(const char *from, const char *to) {
    display_screens_build_ota(from, to);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_update_layout(scr);
    uint32_t n = lv_obj_get_child_count(scr);
    TEST_ASSERT_EQUAL_UINT32(4, n);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *o = lv_obj_get_child(scr, i);
        printf("ota line %u: \"%s\" w=%d h=%d y=%d\n", i, lv_label_get_text(o), (int)lv_obj_get_width(o),
               (int)lv_obj_get_height(o), (int)lv_obj_get_y(o));
        char msg[160];
        snprintf(msg, sizeof(msg), "\"%s\" is %d px wide, budget is %d (panel %d less 8 px a side)",
                 lv_label_get_text(o), (int)lv_obj_get_width(o), OTA_MAX_LINE_W, HOR);
        TEST_ASSERT_TRUE_MESSAGE(lv_obj_get_width(o) <= OTA_MAX_LINE_W, msg);
        /* A wrapped label is taller than one line of its font. */
        TEST_ASSERT_TRUE_MESSAGE(lv_obj_get_height(o) < 40, "label wrapped onto a second line");
        TEST_ASSERT_TRUE_MESSAGE(lv_obj_get_y(o) + lv_obj_get_height(o) <= VER, "label runs off the bottom edge");
    }
}

void test_ota_screen_lines_fit_the_panel(void) {
    /* The 28 pt "Updating Firmware" title is the widest fixed string on
       any screen in this tree (279 px; the plan's all-caps "UPDATING
       FIRMWARE" measured 327 and would have been clipped, which is why
       the title is mixed case). The 18 pt "Installing v..." line is the
       widest VARIABLE one — untruncated it left the panel at 21
       characters (303 px) and reached 426 px at 31. Drive both the
       ordinary and the maximum-length cases. */
    assert_ota_lines_fit("1.10.10-rc1", "1.10.11-rc2");
    for (size_t i = 0; i < sizeof(VERSION_CASES) / sizeof(VERSION_CASES[0]); i++) {
        printf("-- ota with %s --\n", VERSION_CASES[i].what);
        assert_ota_lines_fit(VERSION_CASES[i].ver, VERSION_CASES[i].ver);
    }
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

void test_bedtime_screen(void) {
    /* Bed Time lock: inverted, non-dismissable until day rollover */
    display_screens_build_bedtime();
    assert_matches_golden("bedtime");
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
    RUN_TEST(test_break_screen_no_extras);
    RUN_TEST(test_main_break_chip);
    RUN_TEST(test_main_break_chip_no_start);
    RUN_TEST(test_start_available_only_changes_button_a);
    RUN_TEST(test_break_screen_no_eligible);
    RUN_TEST(test_main_low_battery_warn_badge);
    RUN_TEST(test_version_fits_the_battery_row);
    RUN_TEST(test_version_cap_clips_the_drawing_not_just_the_object);
    RUN_TEST(test_no_version_renders_the_row_unchanged);
    RUN_TEST(test_ota_screen);
    RUN_TEST(test_ota_screen_lines_fit_the_panel);
    RUN_TEST(test_charge_me_screen);
    RUN_TEST(test_timesup_screen);
    RUN_TEST(test_sync_failed_screen);
    RUN_TEST(test_bedtime_screen);
    return UNITY_END();
}
