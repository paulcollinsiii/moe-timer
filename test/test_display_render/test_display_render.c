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
    /* The raw timer_reload_allowed() gate is true for a paused reloadable
       slot, but B resumes rather than reloads: this golden pins the play
       glyph over B, NOT a Reload label. */
    st.reload_available = true;
    display_screens_build_main(&st);
    assert_matches_golden("main_paused_reloadable");
}

void test_main_expired(void) {
    /* The Screen slot: no def, never reloadable, so B stays blank when
       the day runs out. */
    display_state_t st = base_state();
    st.timer_state = TIMER_EXPIRED;
    st.remaining_sec = 0;
    display_screens_build_main(&st);
    assert_matches_golden("main_expired");
}

void test_main_expired_reloadable(void) {
    /* An EXPIRED reloadable extra — the one screen that carries the
       Reload label. B has no other job in this state, which is what lets
       reload keep the same key rather than needing one of its own. */
    display_state_t st = base_state();
    st.timer_state = TIMER_EXPIRED;
    st.timer_name = "Piano";
    st.reloadable = true;
    st.completions = 1;
    st.allocation_sec = 900;
    st.remaining_sec = 0;
    st.reload_available = true;
    display_screens_build_main(&st);
    assert_matches_golden("main_expired_reloadable");
}

void test_break_screen(void) {
    /* Break screen with extras configured: the bottom row offers the swap.
       A is unlabelled because the mode toggle has no label yet (it gained
       its binding in M2-T3 but not a label); B is unlabelled because the
       break is still enforced (TIMER_BREAK yields no label). */
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
       Button B carries no play glyph, because a press would be refused.
       reload_available is true here and still draws nothing — the slot is
       PAUSED, not EXPIRED.

       This is a DIFFERENT scenario from test_main_break_chip, not a
       one-field variant of it — the two states differ in the timer name,
       state, allocation and remaining as well, so do not read a diff of
       the two goldens as "what start_available does".
       test_start_available_only_changes_button_b below is what isolates
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
   start_available, must differ only inside Button B's cell. Catches a
   layout that reflows when the glyph disappears — which a golden pair of
   two different scenarios cannot. */
void test_start_available_only_changes_button_b(void) {
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

    /* Button B's label is centred on x=91 (BTN_X0 + BTN_PITCH); its cell
       runs half a pitch either side, x=54..128. Byte columns 6..16 are
       the minimal byte range CONTAINING that cell, and so are slightly
       wider than it: byte 6 starts at x=48 and byte 16 ends at x=135.
       Literals, not expressions over the source constants, so a mistake
       in those constants cannot move the corridor in lockstep with the
       bug. */
    int differing = 0;
    for (int r = 0; r < VER; r++) {
        for (int b = 0; b < HOR / 8; b++) {
            int i = r * (HOR / 8) + b;
            if (with_glyph[i] == s_captured[i])
                continue;
            differing++;
            char msg[80];
            snprintf(msg, sizeof(msg), "row %d byte %d changed outside Button B's cell", r, b);
            TEST_ASSERT_TRUE_MESSAGE(r >= 112 && b >= 6 && b <= 16, msg);
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(differing > 0, "start_available changed nothing at all");
}

/* "Reload" is text where every other cell in this row is a glyph, and it
   replaced a shorter word ("Reset"), so its width is the one thing about
   the new layout that could silently collide with a neighbour. The cell
   is BTN_PITCH wide and centred, so half the label must fit in half a
   pitch on each side. Measured against the real font this suite links,
   not eyeballed from the golden. */
void test_reload_label_fits_its_cell(void) {
    display_state_t st = base_state();
    st.timer_state = TIMER_EXPIRED;
    st.timer_name = "Piano";
    st.reloadable = true;
    st.reload_available = true;
    st.remaining_sec = 0;
    display_screens_build_main(&st);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_update_layout(scr);

    int32_t w = -1;
    uint32_t n = lv_obj_get_child_count(scr);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *o = lv_obj_get_child(scr, i);
        if (lv_obj_check_type(o, &lv_label_class) && strcmp(lv_label_get_text(o), "Reload") == 0)
            w = lv_obj_get_width(o);
    }
    TEST_ASSERT_NOT_EQUAL_MESSAGE(-1, w, "Reload label not found on an EXPIRED reloadable slot");
    printf("Reload label is %d px wide; the cell is %d px\n", (int)w, BTN_PITCH);
    TEST_ASSERT_TRUE_MESSAGE(w <= BTN_PITCH, "Reload label is wider than its button cell");
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

/* ---- the status row: mode line + state word share one row ----

   The row now carries a SECOND caller-supplied string (the adjustment
   suffix) on top of the HA-supplied timer name, against a state word
   right-aligned on the same baseline. Same two-limit defence as the
   battery row: the format's own arithmetic bounds the buffer, and a hard
   geometric cap bounds the drawing — a character budget cannot, because
   the row's content is proportional text. */
static void measure_status_row(const display_state_t *st, int32_t *mode_right, int32_t *state_left) {
    display_screens_build_main(st);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_update_layout(scr);
    /* The mode label is the left-aligned one below the battery row (x=4,
       y=66); the state word is the right-most label sharing its row. */
    int32_t mode_y = -1;
    *mode_right = -1;
    *state_left = -1;
    uint32_t n = lv_obj_get_child_count(scr);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *o = lv_obj_get_child(scr, i);
        if (!lv_obj_check_type(o, &lv_label_class))
            continue;
        if (lv_obj_get_x(o) == 4 && lv_obj_get_y(o) > 80) {
            mode_y = lv_obj_get_y(o);
            *mode_right = lv_obj_get_x(o) + lv_obj_get_width(o);
        }
    }
    TEST_ASSERT_NOT_EQUAL_MESSAGE(-1, *mode_right, "mode line label not found");
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *o = lv_obj_get_child(scr, i);
        if (!lv_obj_check_type(o, &lv_label_class))
            continue;
        if (lv_obj_get_y(o) == mode_y && lv_obj_get_x(o) > 4)
            *state_left = lv_obj_get_x(o);
    }
    TEST_ASSERT_NOT_EQUAL_MESSAGE(-1, *state_left, "state label not found");
}

/* Reachable extremes, not imagined ones: the day allocation is bounded by
   CFG_BOUND_ALLOC_HI (1440 min) and an extra timer's name by the config
   text field, and "TIME'S UP" is the widest state word.

   The adjustment has no such bound. ±240 min bounds ONE adjustment (HA's
   number and a single cmd grant each), but the suffix reports the day's
   running total and cmd grants repeat once per network window, so the
   last rows drive it far past that — up to the week the snapshot
   validator will still restore. The cap is a geometric backstop, so this
   passing at 10080 min is the property, not a coincidence of the bound. */
void test_mode_row_fits_beside_the_state_word(void) {
    static const struct {
        const char *name;
        uint16_t completions;
        bool reloadable;
        uint32_t alloc;
        int32_t adjust;
        timer_state_t state;
        const char *what;
    } CASES[] = {
        {NULL, 0, false, 3600, 0, TIMER_IDLE, "plain weekday"},
        {NULL, 0, false, 3600, -1800, TIMER_RUNNING, "weekday, -30 today"},
        {NULL, 0, false, 3600, 14400, TIMER_EXPIRED, "weekday, +240 today"},
        {NULL, 0, false, 86400, -14400, TIMER_EXPIRED, "1440 min day, -240 today (both maxima)"},
        {"Meditation", 0, false, 600, 0, TIMER_IDLE, "extra timer, unadjusted"},
        {"Laundry folding", 12, true, 5400, 14400, TIMER_EXPIRED, "long name + counter + max grant"},
        {"WWWWWWWWWWWWWWWW", 99, true, 5400, -14400, TIMER_EXPIRED, "widest glyphs a name field allows"},
        {NULL, 0, false, 86400, 604800, TIMER_EXPIRED, "1440 min day, +10080 today (accumulated grants)"},
        {"WWWWWWWWWWWWWWWW", 99, true, 5400, -604800, TIMER_EXPIRED, "widest name + a week of deductions"},
    };
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        display_state_t st = base_state();
        st.timer_name = CASES[i].name;
        st.completions = CASES[i].completions;
        st.reloadable = CASES[i].reloadable;
        st.allocation_sec = CASES[i].alloc;
        st.adjust_sec = CASES[i].adjust;
        st.timer_state = CASES[i].state;
        int32_t mode_right, state_left;
        measure_status_row(&st, &mode_right, &state_left);
        printf("status row [%-40s] mode ends x=%3d, state starts x=%3d, gap=%d px\n", CASES[i].what, (int)mode_right,
               (int)state_left, (int)(state_left - mode_right));
        char msg[160];
        snprintf(msg, sizeof(msg), "%s: mode line ends at x=%d, state word starts at x=%d", CASES[i].what,
                 (int)mode_right, (int)state_left);
        TEST_ASSERT_TRUE_MESSAGE(mode_right < state_left, msg);
    }
}

/* And the cap bounds the DRAWING, not merely the object — the same
   property test_version_cap_clips_the_drawing_not_just_the_object pins
   for the battery row, and the one that could differ between LVGL
   versions. Mirrored as literals for the same reason it is there: a test
   that recomputed the corridor from the source constants would move in
   lockstep with a mistake in them. */
#define MODE_ROW_CAP_RIGHT 224 /* x=4 + MODE_ROW_MAX_W */
#define STATE_WORD_LEFT 230    /* "TIME'S UP" right-aligned at x=292 */

void test_mode_row_cap_clips_the_drawing_not_just_the_object(void) {
    display_state_t st = base_state();
    st.timer_state = TIMER_EXPIRED;
    st.timer_name = "WWWWWWWWWWWWWWWW";
    st.completions = 99;
    st.reloadable = true;
    st.allocation_sec = 5400;
    st.adjust_sec = -14400;
    int32_t mode_right, state_left;
    measure_status_row(&st, &mode_right, &state_left);
    lv_refr_now(s_disp);
    printf("mode cap check: object ends x=%d, state word starts x=%d\n", (int)mode_right, (int)state_left);
    TEST_ASSERT_TRUE_MESSAGE(mode_right <= MODE_ROW_CAP_RIGHT, "capped mode line is wider than the cap allows");
    TEST_ASSERT_TRUE_MESSAGE(state_left >= STATE_WORD_LEFT, "state word starts further left than the cap assumes");
    for (int32_t y = 92; y <= 110; y++) {
        for (int32_t x = MODE_ROW_CAP_RIGHT; x < STATE_WORD_LEFT; x++) {
            int bit = (s_captured[y * (HOR / 8) + x / 8] >> (7 - (x & 7))) & 1;
            char msg[96];
            snprintf(msg, sizeof(msg), "ink at x=%d y=%d - capped mode line spilled past x=%d", (int)x, (int)y,
                     MODE_ROW_CAP_RIGHT);
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(1, bit, msg); /* LVGL I1: 1 = white */
        }
    }
}

/* The adjusted Screen day line, rendered. The unadjusted one is
   main_idle_weekday, and the pair is the whole feature: same day, same
   default, one of them 30 minutes shorter and saying so. */
void test_main_idle_weekday_adjusted(void) {
    display_state_t st = base_state();
    st.adjust_sec = -1800;   /* HA: "Screen adjust (min) today" = -30 */
    st.remaining_sec = 1800; /* ...which is what is actually left */
    display_screens_build_main(&st);
    assert_matches_golden("main_idle_weekday_adjusted");
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

/* ---- the chore checklist (design §2.4) ---------------------------------- */

/* The checklist reads only the chore block, so its fixture starts from
   base_state() and fills that block in. IDLE and a break-free day:
   display_screen_for() has already refused RUNNING before this screen is
   ever built, and none of the timer fields below reach the panel. */
static display_state_t chore_state(uint8_t count, uint8_t acked) {
    display_state_t st = base_state();
    st.app_mode = APP_MODE_CHORES;
    st.chore_count = count;
    st.chore_acked = acked;
    static const char *NAMES[CHORE_MAX] = {"Dishes away", "Trash out", "Homework"};
    for (uint8_t i = 0; i < count && i < CHORE_MAX; i++)
        snprintf(st.chore_names[i], CHORE_NAME_BUF, "%s", NAMES[i]);
    return st;
}

/* Design §2.4's own sketch: three rows, "1 of 3" in the header, one tick,
   and the Timers / OK 1 / OK 2 / OK 3 button row. */
void test_chore_screen_one_acked(void) {
    display_state_t st = chore_state(3, 0x01);
    display_screens_build_chores(&st);
    assert_matches_golden("chores_one_acked");
}

/* The last ack: the header reads "3 of 3" and the count line appears. */
void test_chore_screen_all_acked(void) {
    display_state_t st = chore_state(3, 0x07);
    st.chore_released = true;
    display_screens_build_chores(&st);
    assert_matches_golden("chores_all_acked");
}

/* A two-chore list. Row 2 is BLANK and rows 0/1 do not move — three fixed
   rows is the property the NeoPixel mapping and the B/C/D binding both
   rest on — and D carries no ack label, because an unconfigured ack
   button is not a chore button (C1). */
void test_chore_screen_two_chores(void) {
    display_state_t st = chore_state(2, 0x02);
    display_screens_build_chores(&st);
    assert_matches_golden("chores_two_rows");
}

/* The rows are FIXED, and this is the assertion that says so without a
   byte-compare: every configured row lands on the same y whatever the
   count or the acks. A layout that closed up around a shorter list would
   pass three goldens and still break the one-for-one row/button/pixel
   mapping §2.4 and §2.5 are built on. */
static void collect_row_ys(const display_state_t *st, int32_t *ys, int *n) {
    display_screens_build_chores(st);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_update_layout(scr);
    *n = 0;
    uint32_t kids = lv_obj_get_child_count(scr);
    for (uint32_t i = 0; i < kids; i++) {
        lv_obj_t *o = lv_obj_get_child(scr, i);
        if (!lv_obj_check_type(o, &lv_label_class))
            continue;
        if (lv_obj_get_x(o) != 32) /* CHORE_NAME_X: the name column only */
            continue;
        ys[(*n)++] = lv_obj_get_y(o);
    }
}

void test_chore_rows_are_fixed_whatever_the_list_holds(void) {
    int32_t ys[CHORE_MAX + 4];
    int n;

    display_state_t three = chore_state(3, 0x00);
    collect_row_ys(&three, ys, &n);
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, n, "a three-chore list did not draw three name rows");
    int32_t r0 = ys[0], r1 = ys[1], r2 = ys[2];
    printf("chore name rows at y=%d,%d,%d\n", (int)r0, (int)r1, (int)r2);
    TEST_ASSERT_TRUE_MESSAGE(r0 < r1 && r1 < r2, "chore rows are not in list order");

    /* Acking everything must not move a row (the ticks share the band). */
    display_state_t acked = chore_state(3, 0x07);
    acked.chore_released = true;
    collect_row_ys(&acked, ys, &n);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(r0, ys[0], "acking moved row 0");
    TEST_ASSERT_EQUAL_INT32_MESSAGE(r1, ys[1], "acking moved row 1");
    TEST_ASSERT_EQUAL_INT32_MESSAGE(r2, ys[2], "acking moved row 2");

    /* A shorter list leaves the spare row blank rather than closing up. */
    display_state_t two = chore_state(2, 0x00);
    collect_row_ys(&two, ys, &n);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, n, "a two-chore list did not draw exactly two name rows");
    TEST_ASSERT_EQUAL_INT32_MESSAGE(r0, ys[0], "a shorter list moved row 0");
    TEST_ASSERT_EQUAL_INT32_MESSAGE(r1, ys[1], "a shorter list moved row 1");

    display_state_t one = chore_state(1, 0x01);
    collect_row_ys(&one, ys, &n);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(r0, ys[0], "a one-chore list moved row 0");
}

/* chores.h states the trap and refuses to solve it: CHORE_NAME_MAX is a
   STORAGE cap in bytes and cannot bound rendered width — 20 'W' at 16 pt
   measure 360 px against a 296 px panel. So the display layer carries a
   geometric cap, and this is the measurement that says it holds. Both
   halves are asserted: the object stops at the cap, and the DRAWING stops
   with it (LV_LABEL_LONG_CLIP, not LONG_DOT, which would wrap a
   content-sized label down into the next row's band). */
#define CHORE_NAME_CAP_RIGHT (32 + 260) /* CHORE_NAME_X + CHORE_NAME_MAX_W */

void test_a_pathological_chore_name_cannot_overstrike_the_panel(void) {
    display_state_t st = chore_state(3, 0x00);
    for (int i = 0; i < CHORE_MAX; i++)
        memset(st.chore_names[i], 'W', CHORE_NAME_MAX); /* 20 'W', NUL already there */
    display_screens_build_chores(&st);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_update_layout(scr);

    int rows = 0;
    uint32_t kids = lv_obj_get_child_count(scr);
    for (uint32_t i = 0; i < kids; i++) {
        lv_obj_t *o = lv_obj_get_child(scr, i);
        if (!lv_obj_check_type(o, &lv_label_class) || lv_obj_get_x(o) != 32)
            continue;
        rows++;
        int32_t right = lv_obj_get_x(o) + lv_obj_get_width(o);
        printf("20-'W' chore name at y=%d ends x=%d (cap %d, panel %d)\n", (int)lv_obj_get_y(o), (int)right,
               CHORE_NAME_CAP_RIGHT, HOR);
        char msg[96];
        snprintf(msg, sizeof(msg), "uncapped chore name ends at x=%d", (int)right);
        TEST_ASSERT_TRUE_MESSAGE(right <= CHORE_NAME_CAP_RIGHT, msg);
        TEST_ASSERT_TRUE_MESSAGE(right <= HOR, "chore name runs off the right edge");
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, rows, "the pathological names did not reach the panel at all");

    /* And the drawing, not just the object: the corridor from the cap to
       the panel edge stays white. Bounds are the CONSTANTS — deriving them
       from the measured edge would make the scan vacuous exactly when the
       cap has failed. */
    lv_refr_now(s_disp);
    for (int32_t y = 46; y <= 107; y++) {
        for (int32_t x = CHORE_NAME_CAP_RIGHT; x < HOR; x++) {
            int bit = (s_captured[y * (HOR / 8) + x / 8] >> (7 - (x & 7))) & 1;
            char msg[96];
            snprintf(msg, sizeof(msg), "ink at x=%d y=%d - a chore name spilled past x=%d", (int)x, (int)y,
                     CHORE_NAME_CAP_RIGHT);
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(1, bit, msg); /* LVGL I1: 1 = white */
        }
    }
    /* Nothing wrapped down onto the button row either. */
    assert_rows_blank(108, 110);
}

/* The masking, proved on the panel rather than only in test_display: a
   list shortened from three to two leaves bit 2 set in the stored mask,
   and the frame must be identical to the one drawn with that bit clear.
   A renderer that walked the bits raw would tick a row that is not a
   chore any more — except there IS no row 2 here, so the tick would land
   in the blank band under the list. */
void test_a_stale_ack_bit_above_the_count_changes_no_pixel(void) {
    static uint8_t clean[FB_BYTES];
    display_state_t st = chore_state(2, 0x01);
    display_screens_build_chores(&st);
    lv_refr_now(s_disp);
    memcpy(clean, s_captured, FB_BYTES);

    st.chore_acked = 0x05; /* bit 2: a stale ack from the three-chore list */
    display_screens_build_chores(&st);
    lv_refr_now(s_disp);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(clean, s_captured, FB_BYTES,
                                     "a stale ack bit above the configured count reached the panel");
}

/* The bottom row is four independent items on a 296 px panel, and the one
   that can go wrong is cell A: "Timers" is 41 px of text where the main
   screen's A cell is empty, and button A's centre is only 17 px in
   (BTN_X0), so CENTRING it on its button starts it at x=-4 and eats the
   'T'. This test is what caught that — it failed on the first build and is
   why the label is left-aligned. Written as on-panel + non-overlapping
   rather than "narrower than BTN_PITCH", because a left-anchored cell is
   not centred in a pitch and the property that matters is that no two
   labels touch. */
void test_the_chore_button_row_fits_its_cells(void) {
    display_state_t st = chore_state(3, 0x00);
    display_screens_build_chores(&st);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_update_layout(scr);

    int32_t left[8], right[8];
    int found = 0;
    uint32_t kids = lv_obj_get_child_count(scr);
    for (uint32_t i = 0; i < kids; i++) {
        lv_obj_t *o = lv_obj_get_child(scr, i);
        if (!lv_obj_check_type(o, &lv_label_class))
            continue;
        if (lv_obj_get_y(o) < VER - 20) /* the bottom row only */
            continue;
        TEST_ASSERT_TRUE_MESSAGE(found < 8, "more bottom-row labels than the row can hold");
        int32_t x = lv_obj_get_x(o), w = lv_obj_get_width(o);
        printf("chore button cell '%s' x=%d..%d (%d px)\n", lv_label_get_text(o), (int)x, (int)(x + w), (int)w);
        char msg[112];
        snprintf(msg, sizeof(msg), "'%s' spans x=%d..%d, off a %d px panel", lv_label_get_text(o), (int)x, (int)(x + w),
                 HOR);
        TEST_ASSERT_TRUE_MESSAGE(x >= 0, msg);
        TEST_ASSERT_TRUE_MESSAGE(x + w <= HOR, msg);
        left[found] = x;
        right[found] = x + w;
        found++;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(4, found, "the chore button row is not four cells");

    /* Built left to right, so a plain sweep settles it: nothing overlaps
       the cell after it. */
    for (int i = 1; i < found; i++) {
        char msg[112];
        snprintf(msg, sizeof(msg), "cell %d ends at x=%d and cell %d starts at x=%d", i - 1, (int)right[i - 1], i,
                 (int)left[i]);
        TEST_ASSERT_TRUE_MESSAGE(right[i - 1] < left[i], msg);
    }
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
    RUN_TEST(test_main_expired_reloadable);
    RUN_TEST(test_break_screen);
    RUN_TEST(test_break_screen_no_extras);
    RUN_TEST(test_main_break_chip);
    RUN_TEST(test_main_break_chip_no_start);
    RUN_TEST(test_start_available_only_changes_button_b);
    RUN_TEST(test_reload_label_fits_its_cell);
    RUN_TEST(test_break_screen_no_eligible);
    RUN_TEST(test_main_low_battery_warn_badge);
    RUN_TEST(test_main_idle_weekday_adjusted);
    RUN_TEST(test_mode_row_fits_beside_the_state_word);
    RUN_TEST(test_mode_row_cap_clips_the_drawing_not_just_the_object);
    RUN_TEST(test_version_fits_the_battery_row);
    RUN_TEST(test_version_cap_clips_the_drawing_not_just_the_object);
    RUN_TEST(test_no_version_renders_the_row_unchanged);
    RUN_TEST(test_ota_screen);
    RUN_TEST(test_ota_screen_lines_fit_the_panel);
    RUN_TEST(test_charge_me_screen);
    RUN_TEST(test_timesup_screen);
    RUN_TEST(test_sync_failed_screen);
    RUN_TEST(test_bedtime_screen);
    RUN_TEST(test_chore_screen_one_acked);
    RUN_TEST(test_chore_screen_all_acked);
    RUN_TEST(test_chore_screen_two_chores);
    RUN_TEST(test_chore_rows_are_fixed_whatever_the_list_holds);
    RUN_TEST(test_a_pathological_chore_name_cannot_overstrike_the_panel);
    RUN_TEST(test_a_stale_ack_bit_above_the_count_changes_no_pixel);
    RUN_TEST(test_the_chore_button_row_fits_its_cells);
    return UNITY_END();
}
