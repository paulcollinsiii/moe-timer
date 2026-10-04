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

/* Header only, test-only — ties this suite's SETUP_TEST_USERNAME to the
   real device's SETUP_SESSION_QR_USERNAME (see
   test_setup_screens_own_test_username_matches_the_real_qr_username
   below). display_screens.c itself never includes this: display.c must
   not depend on setup_session.h, and display_screens.c follows it. */
#include "setup_session.h"

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

void tearDown(void) {
    /* qr_render_encode() heap-allocates its result (main/qr_render.c);
       on-device, display.c's setup wrapper frees it right after render()
       returns, but this suite never links display.c, so nothing else
       ever would. Unconditional and a documented no-op when a test never
       touched the QR path — ASan's leak check runs once, at process
       exit, so whatever the LAST QR-touching test here left allocated
       would otherwise be reported as a leak regardless of which test it
       was. */
    qr_render_release();
}

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
       locking break, which is the right behaviour here.

       TWO conditions reach that footer now, not one. Since M2-T6 this is
       cell 4 of §2.6's matrix — no swap hint AND no chore list — and the
       second half rides silently on base_state() leaving chore_count at
       0. Giving base_state() a non-zero .chore_count would move this
       screen to cell 2, which has a button row, and take
       break_screen_no_extras (cell 4 likewise) and break_screen (cell 3
       to cell 1) with it: all three legacy break goldens broken by a
       fixture edit that looks unrelated to any of them. If chore counts
       are ever wanted by default, set them on the fixtures that need
       them — break_chore_state() is where they belong. */
    display_state_t st = base_state();
    st.timer_state = TIMER_BREAK;
    st.remaining_sec = 5400;
    st.break_remaining_sec = 700;
    st.swap_next_name = NULL;
    display_screens_build_break(&st);
    assert_matches_golden("break_screen_no_eligible");
}

/* ---- the break screen's chore cells (design §2.6) --------------------

   The matrix has four cells and the two with no list configured are the
   two goldens above, unchanged. These are the other two: a configured
   list puts a label over button A and a prompt line under the countdown,
   and the old `swap_next_name == NULL` suppression — which used to mean
   "no button row at all" — now only means "no cell C".

   The break screen reads two fields out of the chore block and no more:
   chore_count decides whether there is a list, and chore_acked is what
   the prompt counts — chore_outstanding was the prompt's input until
   M2-T6a turned the figure round, and nothing on this screen reads it
   now. The names belong to the checklist, so this fixture does not fill
   them in. */
/* `outstanding` stays the argument because that is what every case here
   reads naturally ("three chores, two still to do"), but chore_acked is
   filled in to match: the two are not independent fields on the device —
   app_state.c derives chore_outstanding FROM the ack bits at the assembly
   seam — and the break screen's prompt is drawn from the bits. A fixture
   that set only one of them would render a screen no device can be in,
   and would let a painter reading the wrong field look correct. */
static display_state_t break_chore_state(uint8_t count, uint8_t outstanding) {
    display_state_t st = base_state();
    st.timer_state = TIMER_BREAK;
    st.remaining_sec = 5400; /* 1:30:00 of screen time frozen */
    st.break_remaining_sec = 700;
    st.chore_count = count;
    st.chore_outstanding = outstanding;
    /* The low `count - outstanding` bits: which rows are ticked does not
       matter to any case here, only how many. */
    for (uint8_t i = 0; i < count - outstanding; i++)
        st.chore_acked |= (uint8_t)(1u << i);
    return st;
}

/* Cell 1: a list AND somewhere to swap to — the full row. */
void test_break_screen_with_chores(void) {
    display_state_t st = break_chore_state(3, 2);
    st.swap_next_name = "Piano";
    display_screens_build_break(&st);
    assert_matches_golden("break_screen_chores");
}

/* Cell 2: a list, but nothing break-eligible. A still offers Chores, so
   there IS a row — which is precisely what the generalised rule buys and
   what no arrangement of the old one could have produced. */
void test_break_screen_with_chores_no_eligible(void) {
    display_state_t st = break_chore_state(3, 2);
    st.swap_next_name = NULL;
    display_screens_build_break(&st);
    assert_matches_golden("break_screen_chores_no_eligible");
}

/* Cell 1 again, with the finished-list prompt and the widest everything
   else: a 12-hour allocation is the longest frozen screen time and an
   over-long name the longest swap hint the 8-character budget allows.
   This is NOT the chore line's widest case — the tick form is the SHORTER
   of the prompt's two, and test_the_break_chore_line_items_never_meet
   measures both against the frozen time. A pixel golden is what catches
   the packing of five bands against each other; the width assertions
   below only ever compare two at a time. */
void test_break_screen_with_every_chore_done(void) {
    display_state_t st = break_chore_state(3, 0);
    st.remaining_sec = 45000; /* "Screen 12:30" */
    st.swap_next_name = "Woodwind lesson";
    display_screens_build_break(&st);
    assert_matches_golden("break_screen_chores_all_done");
}

/* Painted rows on the INVERTED break screen. LVGL I1 has 1 = white, so
   here ink is the set bits — the mirror image of assert_rows_blank above,
   which reads the upright layouts. */
static bool break_row_has_ink(int y) {
    for (int b = 0; b < HOR / 8; b++) {
        if (s_captured[y * (HOR / 8) + b] != 0x00)
            return true;
    }
    return false;
}

/* Runs of consecutive painted rows, top to bottom; returns how many. */
static int break_ink_bands(int *tops, int *bots, int max) {
    int n = 0;
    bool in = false;
    for (int y = 0; y < VER; y++) {
        bool ink = break_row_has_ink(y);
        if (ink && !in) {
            TEST_ASSERT_TRUE_MESSAGE(n < max, "more ink bands than the break screen can hold");
            tops[n] = y;
            in = true;
        } else if (!ink && in) {
            bots[n++] = y - 1;
            in = false;
        }
    }
    if (in)
        bots[n++] = VER - 1;
    return n;
}

/* THE trap this layout had to clear. On the unchanged break screen the
   48 pt countdown ends at row 104 and the button row starts at row 109:
   four blank rows, where a 16 pt line needs twelve. So there is no band
   free for a chore line where the countdown stands, and a list lifts the
   bar and the countdown to open one.

   Measured on the glass rather than in the arithmetic, because the
   failure mode is silent: overlapping white-on-black text is unreadable
   on e-ink, and MAGTAG_WRITE_GOLDEN would happily record the overlap as
   the new correct answer. Five bands with a gutter each is the property;
   the goldens pin which pixels are in them. */
void test_the_break_chore_line_clears_the_countdown_and_the_row(void) {
    display_state_t st = break_chore_state(3, 2);
    st.swap_next_name = "Piano";
    display_screens_build_break(&st);
    lv_refr_now(s_disp);

    int tops[8], bots[8];
    int n = break_ink_bands(tops, bots, 8);
    for (int i = 0; i < n; i++)
        printf("break band %d: rows %d..%d\n", i, tops[i], bots[i]);
    TEST_ASSERT_EQUAL_INT_MESSAGE(5, n, "the break screen is not title / bar / countdown / chore line / button row");

    /* 4 is not a target plucked out of the air: it is the gutter the
       unchanged screen already runs at between its countdown and its
       row, so it is the floor a new band has to clear as well. */
    for (int i = 1; i < n; i++) {
        char msg[112];
        snprintf(msg, sizeof(msg), "band %d ends at row %d and band %d starts at row %d - %d blank rows between", i - 1,
                 bots[i - 1], i, tops[i], (int)(tops[i] - bots[i - 1] - 1));
        TEST_ASSERT_TRUE_MESSAGE(tops[i] - bots[i - 1] - 1 >= 4, msg);
    }
}

/* Collects the bottom row's label boxes, left to right as they were
   built. `found` is the cell count, which is the matrix cell's signature:
   three with a swap hint, two without. */
static int break_bottom_row(int32_t *left, int32_t *right, const char **text, int max) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_update_layout(scr);
    int found = 0;
    uint32_t kids = lv_obj_get_child_count(scr);
    for (uint32_t i = 0; i < kids; i++) {
        lv_obj_t *o = lv_obj_get_child(scr, i);
        if (!lv_obj_check_type(o, &lv_label_class))
            continue;
        if (lv_obj_get_y(o) < VER - 20) /* the bottom row only */
            continue;
        TEST_ASSERT_TRUE_MESSAGE(found < max, "more bottom-row labels than the row can hold");
        text[found] = lv_label_get_text(o);
        int32_t x = lv_obj_get_x(o), w = lv_obj_get_width(o);
        printf("break button cell '%s' x=%d..%d (%d px)\n", lv_label_get_text(o), (int)x, (int)(x + w), (int)w);
        char msg[112];
        snprintf(msg, sizeof(msg), "'%s' spans x=%d..%d, off a %d px panel", lv_label_get_text(o), (int)x, (int)(x + w),
                 HOR);
        TEST_ASSERT_TRUE_MESSAGE(x >= 0, msg);
        TEST_ASSERT_TRUE_MESSAGE(x + w <= HOR, msg);
        left[found] = x;
        right[found] = x + w;
        found++;
    }
    for (int i = 1; i < found; i++) {
        char msg[112];
        snprintf(msg, sizeof(msg), "cell %d ends at x=%d and cell %d starts at x=%d", i - 1, (int)right[i - 1], i,
                 (int)left[i]);
        TEST_ASSERT_TRUE_MESSAGE(right[i - 1] < left[i], msg);
    }
    return found;
}

/* The row is NOT four equal cells and never was: "Chores" is 57 px from a
   left margin at x=4, so it ends at x=61 and overruns the A/B midline at
   x=54. That is safe for exactly one reason — cell B carries no label on
   this screen, because the break is still enforced for the Screen timer
   and display_button_b_label() yields nothing for TIMER_BREAK — and this
   test is what holds it: three cells and none of them reaching the next.

   WHICH ASSERTION HOLDS WHAT, because it is not the obvious split. The
   real guard against cell A running into the swap hint is the adjacency
   loop inside break_bottom_row() — right[i-1] < left[i] — and yes, that
   compares a MEASURED neighbour. It has to: the hint is centred on C, so
   where its left edge falls is a function of the string's width and no
   constant can name it. The loop is not thereby vacuous, because the two
   edges come from independent build paths: cell A is left-anchored at a
   margin, the hint is centre-anchored on a button, and nothing derives
   either from the other. The midline bound below is the coarse structural
   check that survives a change of strings, and it is strictly weaker than
   it looks — see there.

   It is also why the frozen screen time is NOT in this row in the chore
   cells. "Screen 1:30" is 90 px; put it over button B and it spans
   x=46..136, which collides with "Chores" at one end and the swap hint at
   the other. It moved to the line above instead. */
void test_the_break_chore_row_fits_its_cells(void) {
    display_state_t st = break_chore_state(3, 2);
    st.swap_next_name = "Woodwind lesson"; /* the widest hint the budget allows */
    display_screens_build_break(&st);

    int32_t left[8], right[8];
    const char *text[8];
    int found = break_bottom_row(left, right, text, 8);
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, found, "cell 1 of the matrix is Chores, the swap hint and sync");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("Chores", text[0], "cell A does not offer the checklist");
    /* A COARSE UPPER BOUND, deliberately, and not the thing that keeps
       the two apart. BTN_X0 + 3*BTN_PITCH/2 is the B/C midline at x=128;
       the widest hint the budget allows already starts at x=114, 14 px
       LEFT of it, so a 120 px cell-A label would satisfy this and still
       paint over the hint. Non-overlap is the adjacency loop's job (see
       the header comment). What this adds that the loop cannot is a bound
       expressed in the row's own geometry rather than in today's strings:
       it fails if cell A grows past the structural half-way mark even on
       a future screen where the hint happens to be short enough for the
       loop to shrug. Keep both; neither implies the other. */
    TEST_ASSERT_TRUE_MESSAGE(right[0] < BTN_X0 + 3 * BTN_PITCH / 2,
                             "the Chores label reaches past cell B and over the B/C midline");
}

/* Cell 2: no break-eligible timer, so no C — but A still has a label, so
   the row survives where the old rule would have dropped it and fallen
   back to the centred footer. */
void test_the_break_row_without_a_swap_keeps_its_chore_cell(void) {
    display_state_t st = break_chore_state(3, 2);
    st.swap_next_name = NULL;
    display_screens_build_break(&st);

    int32_t left[8], right[8];
    const char *text[8];
    int found = break_bottom_row(left, right, text, 8);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, found, "cell 2 of the matrix is Chores and sync, with C empty");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("Chores", text[0], "cell A does not offer the checklist");
    TEST_ASSERT_EQUAL_INT_MESSAGE(4, left[0], "the Chores label is not at the row's left margin");
}

/* Collects the chore line's label boxes and text, left to right as they
   were built: the chore prompt, then the frozen screen time. The text
   pointers belong to the labels, and fresh_screen() deletes those on the
   next build — copy anything that has to outlive one render. */
static int break_chore_line(int32_t *left, int32_t *right, const char **text, int max) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_update_layout(scr);
    int found = 0;
    uint32_t kids = lv_obj_get_child_count(scr);
    for (uint32_t i = 0; i < kids; i++) {
        lv_obj_t *o = lv_obj_get_child(scr, i);
        if (!lv_obj_check_type(o, &lv_label_class) || lv_obj_get_y(o) != BREAK_CHORE_LINE_Y)
            continue;
        TEST_ASSERT_TRUE_MESSAGE(found < max, "more items on the chore line than it holds");
        printf("break chore line '%s' x=%d..%d\n", lv_label_get_text(o), (int)lv_obj_get_x(o),
               (int)(lv_obj_get_x(o) + lv_obj_get_width(o)));
        text[found] = lv_label_get_text(o);
        left[found] = lv_obj_get_x(o);
        right[found] = lv_obj_get_x(o) + lv_obj_get_width(o);
        found++;
    }
    return found;
}

/* The line above the row carries two items — the chore prompt at the left
   margin, the frozen screen time at the right — and nothing truncates
   either, so the only thing keeping them apart is the panel being wide
   enough. MEASURED, not assumed: M2-T5 shipped a label clipped mid-number
   one band below this one because a width was arrived at by counting
   characters.

   NEITHER SIDE'S WORST CASE IS THE OBVIOUS ONE, and both were found by
   sweeping rather than by reading the strings. The digits of this font do
   not share an advance — '0' is 3 px wider than '1' — so "Screen 12:30"
   is NOT the widest frozen time ("Screen 20:00" is, by 7 px) and the
   widest prompt is the one with the most zeros in it. Both prompt forms
   are rendered too, because the tick form is the shorter and it is the
   count form that decides the worst case. */
void test_the_break_chore_line_items_never_meet(void) {
    /* outstanding = 3 -> nothing done -> "Chores 0 of 3", the widest the
       prompt gets at this count; outstanding = 0 -> the tick form. */
    const uint8_t outstanding[2] = {3, 0};
    for (int pass = 0; pass < 2; pass++) {
        display_state_t st = break_chore_state(3, outstanding[pass]);
        st.remaining_sec = 20 * 3600; /* "Screen 20:00", the widest */
        st.swap_next_name = "Piano";
        display_screens_build_break(&st);

        int32_t left[4], right[4];
        const char *text[4];
        int found = break_chore_line(left, right, text, 4);
        TEST_ASSERT_EQUAL_INT_MESSAGE(2, found, "the chore line is the chore prompt and the frozen screen time");
        /* THE MARGINS ARE PINNED, not bounded, and the difference is the
           whole point. "inside the panel" (left >= 0, right <= HOR) is
           satisfied BETTER by deleting the padding — zeroing either
           margin moves the glyphs AWAY from the edge the bound watches,
           so the mutant reads as an improvement and only the golden
           notices. That exact shape shipped one band down in M2-T5 and
           the button row's equivalent is pinned for the same reason (see
           the Chores cell above). Equalities, so the padding cannot
           quietly go missing.

           WHICH ITEM each anchor pins is asserted and not assumed. The
           two swapped sides in M2-T6a, and an index-only anchor survives
           that swap intact: left[0] == 4 was true of the old layout as
           well, because the old left-hand item was the frozen time. So
           the identity of each item is checked first, and only then its
           margin. */
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp(text[0], "Chores", 6), "the line's left-hand item is not the prompt");
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp(text[1], "Screen ", 7),
                                      "the line's right-hand item is not the frozen screen time");
        TEST_ASSERT_EQUAL_INT_MESSAGE(4, left[0], "the chore prompt is not at the line's left margin");
        TEST_ASSERT_EQUAL_INT_MESSAGE(HOR - 4, right[1], "the frozen screen time is not at the line's right margin");
        char msg[128];
        snprintf(msg, sizeof(msg), "'%s' ends at x=%d and '%s' starts at x=%d - they touch", text[0], (int)right[0],
                 text[1], (int)left[1]);
        TEST_ASSERT_TRUE_MESSAGE(right[0] < left[1], msg);
    }
}

/* WHICH FIELD the prompt is fed, and WHICH WAY the fraction runs. Both
   mutants produce a figure that is in range and a screen that looks
   entirely plausible, so only an exact string catches either.

   chore_acked and chore_outstanding are neighbouring bytes of the same
   block, and at a count of 3 the obvious fixtures hide the difference:
   acked 0x01 has one bit set and an outstanding of 2 also has one bit
   set, so a painter reading the wrong field renders the same "1" and
   nothing fails. 0x03 against an outstanding of 1 is the case that
   separates them — two bits against one — which is why this pins that
   state and not a tidier-looking one.

   The direction is pinned by the pair: a formatter counting what is LEFT
   rather than what is DONE swaps these two strings over, so each render
   kills the reversal the other would accept. */
void test_the_break_chore_prompt_counts_what_is_done(void) {
    char first[32];
    int32_t left[4], right[4];
    const char *text[4];

    /* 3 configured, 1 outstanding -> acked 0x03, two rows done. */
    display_state_t st = break_chore_state(3, 1);
    st.swap_next_name = "Piano";
    display_screens_build_break(&st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, break_chore_line(left, right, text, 4), "the chore line is not drawn");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("Chores 2 of 3", text[0],
                                     "the prompt at 2 done of 3 is wrong - it is reading the outstanding count, or "
                                     "counting what is left rather than what is done");
    /* Copied because the next build deletes the label that owns it. */
    snprintf(first, sizeof(first), "%s", text[0]);

    /* The same list one ack earlier. Only the ack bits move. */
    st = break_chore_state(3, 2);
    st.swap_next_name = "Piano";
    display_screens_build_break(&st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, break_chore_line(left, right, text, 4), "the chore line is not drawn");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("Chores 1 of 3", text[0], "the prompt at 1 done of 3 is wrong");
    TEST_ASSERT_TRUE_MESSAGE(strcmp(first, text[0]) != 0,
                             "the chore prompt says the same thing at 1 done as at 2 - it is reading the list's "
                             "size, not how much of it is finished");
}

/* THE TICK IS THE CHECKLIST'S OWN GLYPH, held by an assertion rather than
   by the comment on DISPLAY_CHORE_TICK. That macro spells U+F00C as raw
   UTF-8 because display_layout.c is a pure formatter with no LVGL and
   test_display links none either — so this suite, which links both, is
   the only place the two spellings can be compared. Without this a font
   or symbol-table upgrade that moved the codepoint would paint a box on
   the glass with every other test still green.

   It also pins the tick's PLACEMENT end to end, and the reason it goes
   last: the word "Chores" must not move when the list is finished,
   because a word that shifts on e-ink reads as churn. The two renders
   differ in nothing but the ack bits, so the left edge holding still is
   the property itself and not a restatement of the margin anchor. */
void test_the_break_chore_tick_matches_the_checklist_glyph(void) {
    TEST_ASSERT_EQUAL_STRING_MESSAGE(LV_SYMBOL_OK, DISPLAY_CHORE_TICK,
                                     "DISPLAY_CHORE_TICK is no longer the glyph the checklist rows draw");

    int32_t left[4], right[4];
    const char *text[4];

    display_state_t st = break_chore_state(3, 1);
    st.swap_next_name = "Piano";
    display_screens_build_break(&st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, break_chore_line(left, right, text, 4), "the chore line is not drawn");
    const int32_t unfinished_left = left[0];

    st = break_chore_state(3, 0);
    st.swap_next_name = "Piano";
    display_screens_build_break(&st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, break_chore_line(left, right, text, 4), "the chore line is not drawn");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("Chores " LV_SYMBOL_OK, text[0],
                                     "the finished prompt is not the word then the tick - a leading tick is the same "
                                     "glyphs in the order that moves the word");
    TEST_ASSERT_EQUAL_INT_MESSAGE(unfinished_left, left[0], "'Chores' moves sideways when the list is finished");
}

/* A LIST OF ONE, which is the boundary the display-side gate is written
   on and the one no other case here visits: every fixture above renders
   at a count of 3. That left `chore_count > 0` free to be mutated to
   `> 1` with all 146 cases green, goldens included — and a family that
   configures a single chore (config_apply.c's apply_chores() accepts
   n = 1) would have got the legacy no-chore break screen: no Chores over
   A, no prompt line, the frozen screen time back down in the row. The
   three bands are asserted rather than a fourth golden because what is in
   question is the GATE, not the pixels, and the pixels at count 1 differ
   from count 3 only in the prompt's wording.

   It is also the only place a denominator other than 3 is rendered end
   to end rather than unit-tested on the formatter: "Chores 0 of 1" is
   the whole of what a one-chore family sees, and a prompt that hard-coded
   CHORE_MAX as its denominator would read "0 of 3" beside a checklist
   with one row on it. */
void test_a_single_chore_still_earns_the_break_screen_layout(void) {
    display_state_t st = break_chore_state(1, 1);
    st.swap_next_name = "Piano";
    display_screens_build_break(&st);
    lv_refr_now(s_disp);

    int tops[8], bots[8];
    TEST_ASSERT_EQUAL_INT_MESSAGE(5, break_ink_bands(tops, bots, 8),
                                  "one chore does not lift the bar and open a band for the chore line");

    int32_t left[4], right[4];
    const char *text[4];
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, break_chore_line(left, right, text, 4), "one chore draws no chore line");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("Chores 0 of 1", text[0],
                                     "a one-chore list is not counted against its own length");

    int32_t row_left[8], row_right[8];
    const char *row_text[8];
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, break_bottom_row(row_left, row_right, row_text, 8),
                                  "the row is Chores, the swap hint and sync");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("Chores", row_text[0], "one chore does not put Chores over button A");
}

/* THE GATE ITSELF, and the only case that sweeps its whole boundary
   rather than pinning one point of it. `st->chore_count > 0` in
   display_screens.c is a SECOND display-side spelling of
   button_a_toggle_allowed() — the break screen's, beside the chore
   screen's in display_screen_for() — and the obvious home for a
   cross-check, test_button_actions, cannot host one: that suite is a
   single TU that does not compile display_screens.c (the break painter
   needs LVGL, which it does not link), so an arm there could only test a
   copy of this literal retyped into the test. The comment in
   test_the_chore_screen_is_never_painted_where_button_a_would_be_refused
   records what is therefore still unheld.

   Both directions, because each fails differently and the two are caught
   by different things without this case. `> 1` would give a family with a
   single chore configured (config_apply.c's apply_chores() accepts n = 1)
   the legacy no-chore break screen; nothing caught it at all until
   test_a_single_chore_still_earns_the_break_screen_layout, which pins
   that one count and is the case to keep in step with this one. `>= 0`
   would put a dead "Chores" over button A on every device with no list —
   the dead affordance the gate exists to prevent — and was caught only by
   a pixel compare on three goldens. Asserting the painted label and the
   chore line rather than pixels keeps the case about the GATE; the
   goldens cover what the two layouts look like. */
void test_the_break_screen_offers_chores_exactly_when_the_list_is_non_empty(void) {
    int offered = 0;
    int withheld = 0;
    for (uint8_t n = 0; n <= CHORE_MAX; n++) {
        display_state_t st = break_chore_state(n, n);
        /* A swap is available throughout, so the bottom row exists in
           both layouts and the only thing moving is the chore cell. */
        st.swap_next_name = "Piano";
        display_screens_build_break(&st);

        int32_t row_left[8], row_right[8];
        const char *row_text[8];
        int cells = break_bottom_row(row_left, row_right, row_text, 8);
        const bool offers_chores = cells > 0 && strcmp(row_text[0], "Chores") == 0;

        int32_t left[4], right[4];
        const char *text[4];
        const bool has_line = break_chore_line(left, right, text, 4) > 0;

        if (n > 0) {
            offered++;
            TEST_ASSERT_TRUE_MESSAGE(offers_chores, "a configured list does not put Chores over button A");
            TEST_ASSERT_TRUE_MESSAGE(has_line, "a configured list draws no chore line");
        } else {
            withheld++;
            TEST_ASSERT_FALSE_MESSAGE(offers_chores,
                                      "an empty list still offers Chores over button A - the press is refused, so "
                                      "the label is a dead affordance");
            TEST_ASSERT_FALSE_MESSAGE(has_line, "an empty list still draws the chore line");
        }
    }
    /* NON-VACUITY: both arms live under a condition, so each counts its
       own firings. CHORE_MAX + 1 counts in total, split 3 / 1. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(CHORE_MAX, offered, "the sweep never rendered a configured list");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, withheld, "the sweep never rendered an empty list");
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

/* ---- the main screen's cell A (M2-HW-FIX, design §2.6) ----------------

   Collects the MAIN screen's bottom row, left to right as it was built,
   the way break_bottom_row() does for the break screen's. Separate rather
   than shared: the two rows have different fonts, different cell counts
   and different anchors, and a helper general enough for both would be
   parameterised by exactly the things these cases are about.

   The filter is the label's BOTTOM EDGE rather than its top, because the
   two rows this screen has are only 16 px apart at the anchor and a 12 pt
   line box is nearly that tall: the mode row (BOTTOM_LEFT, -18) ends at
   y=110 and the button row (-2) at y=126, so a bottom edge within 6 px of
   the panel is unambiguous where a top-edge threshold is a near miss. */
static int main_bottom_row(int32_t *left, int32_t *right, const char **text, int max) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_update_layout(scr);
    int found = 0;
    uint32_t kids = lv_obj_get_child_count(scr);
    for (uint32_t i = 0; i < kids; i++) {
        lv_obj_t *o = lv_obj_get_child(scr, i);
        if (!lv_obj_check_type(o, &lv_label_class))
            continue;
        if (lv_obj_get_y(o) + lv_obj_get_height(o) < VER - 6)
            continue; /* the bottom row only */
        TEST_ASSERT_TRUE_MESSAGE(found < max, "more bottom-row labels than the row can hold");
        text[found] = lv_label_get_text(o);
        int32_t x = lv_obj_get_x(o), w = lv_obj_get_width(o);
        printf("main button cell '%s' x=%d..%d (%d px)\n", lv_label_get_text(o), (int)x, (int)(x + w), (int)w);
        char msg[112];
        snprintf(msg, sizeof(msg), "'%s' spans x=%d..%d, off a %d px panel", lv_label_get_text(o), (int)x, (int)(x + w),
                 HOR);
        TEST_ASSERT_TRUE_MESSAGE(x >= 0, msg);
        TEST_ASSERT_TRUE_MESSAGE(x + w <= HOR, msg);
        left[found] = x;
        right[found] = x + w;
        found++;
    }
    for (int i = 1; i < found; i++) {
        char msg[112];
        snprintf(msg, sizeof(msg), "cell %d ends at x=%d and cell %d starts at x=%d", i - 1, (int)right[i - 1], i,
                 (int)left[i]);
        TEST_ASSERT_TRUE_MESSAGE(right[i - 1] < left[i], msg);
    }
    return found;
}

/* THE GATE, swept over its whole boundary in BOTH of its dimensions —
   the main screen's counterpart to
   test_the_break_screen_offers_chores_exactly_when_the_list_is_non_empty,
   and written to the same shape on purpose.

   `st->timer_state != TIMER_RUNNING && st->chore_count > 0` in
   display_screens.c is the THIRD display-side spelling of
   button_a_toggle_allowed() (the break screen's is the second,
   display_screen_for()'s the first), and the obvious home for a
   cross-check, test_button_actions, still cannot host one: that suite is
   a single TU that does not compile display_screens.c, so an arm there
   could only test a copy of this predicate retyped into the test. The
   comment on build_button_row() records what is therefore still unheld.
   What this case DOES hold is that the copy in the painter has the shape
   the predicate has.

   TWO DIMENSIONS, because this spelling has two terms where the break
   screen's has one, and each fails differently:
     - the COUNT boundary kills the same pair its sibling does. `> 1`
       gives a one-chore family no hint that A does anything on the screen
       their device actually sits on — the M2-HW-FIX bug, one config
       narrower. `>= 0` puts a dead "Chores" over button A on every device
       with no list, which is the affordance the gate exists to refuse.
     - the STATE term is this screen's alone, since the break screen is
       only ever drawn at TIMER_BREAK. Dropping it entirely, or writing
       `== TIMER_RUNNING`, offers the toggle while a timer runs, and
       button_a_toggle_allowed() refuses exactly there.
   TIMER_BREAK IS IN THE STATE LIST AND IS UNREACHABLE HERE — deliberate,
   and recorded because the next person to notice will think it a mistake.
   display_screen_for() (display_layout.c) routes every TIMER_BREAK to the
   break screen or the checklist, so the app never calls this painter in
   that state. It is swept anyway because display_screens_build_main() is
   a pure function of the snapshot it is handed, and what this case pins
   is the PREDICATE COPY inside it over that function's whole input
   domain. Dropping the row would quietly convert the case into a claim
   about today's routing: re-point one branch of display_screen_for() and
   a painter that had never been exercised at TIMER_BREAK would start
   being, with nothing having failed in between.

   The price, stated so it is not mistaken for a bug later: adding
   `&& st->timer_state != TIMER_BREAK` to build_button_row()'s gate is a
   no-op on any real device and FAILS THIS CASE. That failure is not the
   test catching a defect — it is the painter's domain being narrowed,
   which is a decision to take on purpose (and to make here too) rather
   than one to discover from a red suite and paper over.

   Asserting the painted label rather than pixels keeps the case about the
   GATE; the goldens cover what the two layouts look like. */
void test_the_main_screen_offers_chores_exactly_when_button_a_would_act(void) {
    static const timer_state_t STATES[] = {TIMER_IDLE, TIMER_RUNNING, TIMER_PAUSED, TIMER_EXPIRED, TIMER_BREAK};
    int offered = 0;
    int withheld = 0;
    for (unsigned s = 0; s < sizeof(STATES) / sizeof(STATES[0]); s++) {
        for (uint8_t n = 0; n <= CHORE_MAX; n++) {
            display_state_t st = base_state();
            st.timer_state = STATES[s];
            st.chore_count = n;
            /* A swap is available throughout, so cells C and D exist in
               both layouts and the only thing moving is cell A. */
            st.swap_available = true;
            display_screens_build_main(&st);

            int32_t left[8], right[8];
            const char *text[8];
            int cells = main_bottom_row(left, right, text, 8);
            const bool offers_chores = cells > 0 && strcmp(text[0], "Chores") == 0;

            char msg[128];
            snprintf(msg, sizeof(msg), "timer_state %d with %u chores", (int)STATES[s], (unsigned)n);
            if (STATES[s] != TIMER_RUNNING && n > 0) {
                offered++;
                TEST_ASSERT_TRUE_MESSAGE(offers_chores, msg);
                TEST_ASSERT_EQUAL_INT_MESSAGE(4, left[0], "the Chores label is not at the row's left margin");
            } else {
                withheld++;
                TEST_ASSERT_FALSE_MESSAGE(offers_chores, msg);
            }
        }
    }
    /* NON-VACUITY: both arms live under a condition, so each counts its
       own firings. 5 states x (CHORE_MAX + 1) counts = 20 renders, split
       into the 4 non-RUNNING states x CHORE_MAX counts that offer, and
       the rest that do not. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(4 * CHORE_MAX, offered, "the sweep never rendered an actionable list");
    TEST_ASSERT_EQUAL_INT_MESSAGE(20 - 4 * CHORE_MAX, withheld, "the sweep never rendered a refused toggle");
}

/* Ink bands in a byte-column window of an UPRIGHT screen: LVGL I1 has
   1 = white, so ink here is any byte that is not 0xFF — the mirror of
   break_ink_bands(), which reads the inverted break screen. */
static int main_ink_bands(int b0, int b1, int *tops, int *bots, int max) {
    int n = 0;
    bool in = false;
    for (int y = 0; y < VER; y++) {
        bool ink = false;
        for (int b = b0; b <= b1 && !ink; b++)
            ink = (s_captured[y * (HOR / 8) + b] != 0xFF);
        if (ink && !in) {
            TEST_ASSERT_TRUE_MESSAGE(n < max, "more ink bands than this window can hold");
            tops[n] = y;
            in = true;
        } else if (!ink && in) {
            bots[n++] = y - 1;
            in = false;
        }
    }
    if (in)
        bots[n++] = VER - 1;
    return n;
}

/* THE COLLISION CHECK, and the reason it is measured rather than argued.
   build_main_status() carries the comment "moved up to make room for
   button labels", so the room is already spent: the mode row sits at
   BOTTOM_LEFT -18 and this new label at BOTTOM_LEFT -2, 16 px apart at
   the anchor, with a 12 pt line box nearly that tall. And to its right,
   cell B is centred on x=91 and can carry the word "Reload".
   Overlapping text on e-ink is unreadable, and MAGTAG_WRITE_GOLDEN would
   record the overlap as the new correct answer, so neither edge may be
   taken on trust.

   BOTH NEIGHBOURS, because they fail independently. The horizontal edge
   is the adjacency loop inside main_bottom_row() — right[0] < left[1] —
   plus the structural bound below, which is the same pair the break
   screen's row uses and for the same reason: the neighbour is
   centre-anchored on its button, so where its left edge falls is a
   function of the string and no constant can name it, while the midline
   bound survives a change of strings. The vertical edge is a band count
   in cell A's own byte window, taken off the glass.

   RELOAD IS THE WIDEST CELL B, which is why this renders an EXPIRED
   reloadable slot rather than the IDLE screen the goldens use: a glyph
   cell leaves far more air, and the case that matters is the text one. */
void test_the_main_chore_label_clears_the_status_row_and_button_b(void) {
    display_state_t st = base_state();
    st.timer_state = TIMER_EXPIRED;
    st.timer_name = "Piano";
    st.reloadable = true;
    st.reload_available = true;
    st.remaining_sec = 0;
    st.chore_count = 3;
    display_screens_build_main(&st);
    lv_refr_now(s_disp);

    int32_t left[8], right[8];
    const char *text[8];
    int cells = main_bottom_row(left, right, text, 8);
    TEST_ASSERT_EQUAL_INT_MESSAGE(4, cells, "the main button row is not Chores, Reload, swap and sync");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("Chores", text[0], "cell A does not offer the checklist");
    TEST_ASSERT_EQUAL_INT_MESSAGE(4, left[0], "the Chores label is not at the row's left margin");
    /* The structural bound, in the row's own geometry rather than in
       today's strings: BTN_X0 + BTN_PITCH/2 is the A/B midline at x=54.
       Weaker than it looks on its own — Reload starts well right of it —
       but it fails if cell A ever grows past its half of the row even on
       a screen where the neighbour happens to be short. Keep both. */
    TEST_ASSERT_TRUE_MESSAGE(right[0] < BTN_X0 + BTN_PITCH / 2,
                             "the Chores label reaches past the A/B midline and into cell B");

    /* Bytes 0..5 is x=0..47, cell A's own window: wide enough to hold the
       whole label and narrow enough that cell B's ink never enters it. */
    int tops[12], bots[12];
    int n = main_ink_bands(0, 5, tops, bots, 12);
    for (int i = 0; i < n; i++)
        printf("main cell-A window band %d: rows %d..%d\n", i, tops[i], bots[i]);
    TEST_ASSERT_TRUE_MESSAGE(n >= 2, "cell A's window has no row above the button row to clear");
    /* The bottom-most band is the new label and the one above it is the
       status row. A gutter of 4 blank rows is not plucked out of the air:
       it is what the break screen's bands are held to, and what this
       screen already ran between its countdown and its button row before
       cell A was filled. */
    const int gutter = tops[n - 1] - bots[n - 2] - 1;
    char msg[128];
    snprintf(msg, sizeof(msg), "the status row ends at row %d and the Chores label starts at row %d - %d blank rows",
             bots[n - 2], tops[n - 1], gutter);
    TEST_ASSERT_TRUE_MESSAGE(gutter >= 4, msg);
    TEST_ASSERT_TRUE_MESSAGE(bots[n - 1] <= VER - 1, "the Chores label runs off the bottom of the panel");
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

/* ---- the config-error lock screen (design 5.3, row C11) -----------------

   The one screen on this panel whose job is to be read by an adult and
   acted on, so it carries three things no other lock screen does: which
   day type is broken, both numbers, and what to press. */
void test_config_error_screen(void) {
    display_screens_build_config_error(DAY_WEEKDAY, 120, 60);
    assert_matches_golden("config_error");
}

/* Leftmost and rightmost ink column anywhere on the panel. A label wider
   than 296 px does not wrap or error — LVGL clips it — so a too-long line
   shows up as ink pressed against both edges, and a golden regenerated
   over it would record the clipping as correct. */
static void ink_columns(int *left, int *right) {
    *left = HOR;
    *right = -1;
    for (int y = 0; y < VER; y++) {
        for (int x = 0; x < HOR; x++) {
            if (((s_captured[y * (HOR / 8) + x / 8] >> (7 - (x & 7))) & 1) != 0)
                continue; /* LVGL I1: 1 = white */
            if (x < *left)
                *left = x;
            if (x > *right)
                *right = x;
        }
    }
}

/* The numbers are read from NVS, not from the validated config document,
   so the screen has to survive values no setter would ever have accepted:
   an older firmware or an NVS oddity is the whole reason layer 3 exists.
   uint16_t's ceiling is therefore the width case, not CFG_BOUND_*'s 1440. */
void test_the_config_error_screen_fits_the_panel_at_its_widest(void) {
    const day_type_t days[] = {DAY_WEEKDAY, DAY_WEEKEND, DAY_HOLIDAY, DAY_SUMMER};
    int checked = 0;
    for (size_t i = 0; i < sizeof days / sizeof days[0]; i++) {
        display_screens_build_config_error(days[i], 65535, 65534);
        lv_refr_now(s_disp);
        int left, right;
        ink_columns(&left, &right);
        char msg[128];
        snprintf(msg, sizeof(msg), "day type %d: ink spans x=%d..%d on a %d px panel", (int)days[i], left, right, HOR);
        TEST_ASSERT_TRUE_MESSAGE(left >= 2, msg);
        TEST_ASSERT_TRUE_MESSAGE(right <= HOR - 3, msg);
        checked++;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(4, checked, "the sweep rendered nothing");
}

/* Naming the pair is the entire reason this is a screen rather than
   another config_ack nobody reads, and a golden alone would not notice a
   builder that ignored its arguments and drew the same four lines every
   time. Two different pairs must not render identically. */
void test_the_config_error_screen_renders_the_pair_it_is_given(void) {
    static uint8_t first[FB_BYTES];
    display_screens_build_config_error(DAY_WEEKDAY, 120, 60);
    lv_refr_now(s_disp);
    memcpy(first, s_captured, FB_BYTES);

    display_screens_build_config_error(DAY_WEEKDAY, 121, 60);
    lv_refr_now(s_disp);
    TEST_ASSERT_TRUE_MESSAGE(memcmp(first, s_captured, FB_BYTES) != 0, "the free slice does not reach the panel");

    display_screens_build_config_error(DAY_WEEKDAY, 120, 61);
    lv_refr_now(s_disp);
    TEST_ASSERT_TRUE_MESSAGE(memcmp(first, s_captured, FB_BYTES) != 0, "the allocation does not reach the panel");

    display_screens_build_config_error(DAY_SUMMER, 120, 60);
    lv_refr_now(s_disp);
    TEST_ASSERT_TRUE_MESSAGE(memcmp(first, s_captured, FB_BYTES) != 0, "the day type does not reach the panel");
}

/* ---- WiFi + MQTT provisioning plan: the setup screens -------------------

   Deterministic inputs for every test below: a fixed SSID, AP password,
   QR payload, username and form URL, so the goldens never depend on a
   real device id or a freshly drawn random password. */
#define SETUP_TEST_SSID "MagTag-a1b2c3"
#define SETUP_TEST_PASSWORD "ABCDEFGHJK"
#define SETUP_TEST_USERNAME "magtag"
#define SETUP_TEST_URL "http://192.168.4.1/mqtt"
#define SETUP_TEST_PAYLOAD                                                                       \
    "{\"ver\":\"v1\",\"name\":\"MagTag-a1b2c3\",\"username\":\"magtag\",\"pop\":\"ABCDEFGHJK\"," \
    "\"password\":\"ABCDEFGHJK\",\"transport\":\"softap\",\"security\":2}"

void test_setup_screen(void) {
    display_screens_build_setup(SETUP_TEST_SSID, SETUP_TEST_PASSWORD, SETUP_TEST_PAYLOAD, SETUP_TEST_USERNAME,
                                SETUP_TEST_URL);
    assert_matches_golden("setup");
}

/* setup_session.h's QR-payload comment names the literal username this
   device's QR always carries (SETUP_SESSION_QR_USERNAME); display.c must
   not depend on that header (same reason display_setup() takes plain
   strings instead of a setup_session_screen_info_t), so this file
   includes it ITSELF, test-only, to tie the two together instead of
   leaving that to a comment alone — the same shape as
   test_the_break_chore_tick_matches_the_checklist_glyph's own check
   against LV_SYMBOL_OK. */
void test_setup_screens_own_test_username_matches_the_real_qr_username(void) {
    TEST_ASSERT_EQUAL_STRING(SETUP_SESSION_QR_USERNAME, SETUP_TEST_USERNAME);
}

static bool fb_pixel_is_black(int x, int y) {
    return ((s_captured[y * (HOR / 8) + x / 8] >> (7 - (x & 7))) & 1) == 0; /* LVGL I1: 1 = white */
}

/* The plan's own worry: a golden of a blank QR would pass a byte
   comparison. This checks EVERY pixel of the whole QR_BLOCK_PX square —
   both pixels of every 2x2 module (not just each module's centre, which
   is blind to the whole block having shifted by one pixel), the quiet
   zone, and any padding past the real code's own smaller size up to
   QR_RENDER_MAX_MODULES — against qr_render_module() for the same
   payload, proving the drawing matches the encoding at every pixel it
   draws rather than merely that some ink exists somewhere in the block.
   QR_LEFT_MARGIN/QR_TOP_MARGIN/QR_QUIET_MODULES/QR_SCALE_PX/QR_BLOCK_PX
   are display_screens.c's own macros, visible here because this file
   #includes it as one translation unit. */
void test_setup_screen_qr_matches_the_encoding(void) {
    display_screens_build_setup(SETUP_TEST_SSID, SETUP_TEST_PASSWORD, SETUP_TEST_PAYLOAD, SETUP_TEST_USERNAME,
                                SETUP_TEST_URL);
    lv_refr_now(s_disp);

    int size = 0;
    TEST_ASSERT_TRUE(qr_render_encode(SETUP_TEST_PAYLOAD, &size));
    TEST_ASSERT_TRUE(size > 0);

    int sampled_dark = 0;
    int mismatches = 0;
    for (int py = 0; py < QR_BLOCK_PX; py++) {
        for (int px = 0; px < QR_BLOCK_PX; px++) {
            int mx = px - QR_QUIET_MODULES * QR_SCALE_PX;
            int my = py - QR_QUIET_MODULES * QR_SCALE_PX;
            bool in_code = mx >= 0 && my >= 0 && (mx / QR_SCALE_PX) < size && (my / QR_SCALE_PX) < size;
            bool expect_dark = in_code && qr_render_module(mx / QR_SCALE_PX, my / QR_SCALE_PX);
            bool got_dark = fb_pixel_is_black(QR_LEFT_MARGIN + px, QR_TOP_MARGIN + py);
            if (expect_dark != got_dark)
                mismatches++;
            if (expect_dark)
                sampled_dark++;
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, mismatches,
                                  "a pixel (module interior, quiet zone, or padding) disagrees with the encoding");
    TEST_ASSERT_TRUE_MESSAGE(sampled_dark > 0, "no dark module sampled - the blank-QR trap the plan calls out");
}

/* setup_session.h's ap_ssid buffer is 32 bytes including the NUL, so 31
   characters is the longest possible SSID. The AP line must stay inside
   its column (cap_width()'s LONG_CLIP), never overflow into the QR block
   to its left or off the panel's right edge. Found by its y coordinate
   (24, the same one display_screens_build_setup() aligns it at) rather
   than a hardcoded child index, so adding or reordering a label on this
   screen cannot silently point this at the wrong widget. */
void test_the_setup_screen_clips_a_31_byte_ssid_without_overflowing_its_column(void) {
    char ssid31[32];
    memset(ssid31, 'X', 31);
    ssid31[31] = '\0';

    display_screens_build_setup(ssid31, SETUP_TEST_PASSWORD, SETUP_TEST_PAYLOAD, SETUP_TEST_USERNAME, SETUP_TEST_URL);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_update_layout(scr);

    lv_obj_t *ap_line = NULL;
    uint32_t n = lv_obj_get_child_count(scr);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *o = lv_obj_get_child(scr, i);
        if (lv_obj_check_type(o, &lv_label_class) && lv_obj_get_y(o) == 22)
            ap_line = o;
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(ap_line, "AP line (y=22) not found");

    char msg[96];
    snprintf(msg, sizeof(msg), "AP line is %d px wide, column budget is %d", (int)lv_obj_get_width(ap_line),
             SETUP_TEXT_MAX_W);
    TEST_ASSERT_TRUE_MESSAGE(lv_obj_get_width(ap_line) <= SETUP_TEXT_MAX_W, msg);

    /* Meaningful, not a tautology: cap_width()'s LONG_CLIP guarantees
       width <= max on its own, so the property worth pinning is that the
       UNCAPPED line really would have overflowed — i.e. the cap is doing
       something on this input, not merely present. A fresh, uncapped
       label with the same text and font measures that directly, rather
       than mutating the already-asserted widget and hoping its style
       reverts cleanly. */
    char buf[48];
    snprintf(buf, sizeof(buf), "AP: %s", ssid31);
    lv_obj_t *uncapped = lv_label_create(scr);
    lv_label_set_text(uncapped, buf);
    lv_obj_set_style_text_font(uncapped, &lv_font_montserrat_12, 0);
    lv_obj_update_layout(scr);
    int32_t natural_w = lv_obj_get_width(uncapped);
    TEST_ASSERT_TRUE_MESSAGE(natural_w > SETUP_TEXT_MAX_W,
                             "a 31-byte SSID's natural width never exceeded the column "
                             "budget - this test would pass even with no clip at all");
}

/* qr_render.h's own ceiling: version 7 at ECC LOW holds at most 154 byte-
   mode bytes. 155 forces a failure qr_render_encode() cannot recover
   from — the screen must still render every text line, just with no QR
   block, and those lines are the complete manual-entry path (SSID,
   username, password/PoP, MQTT URL), not merely a note that one exists. */
void test_the_setup_screen_falls_back_to_text_when_the_payload_is_too_long(void) {
    char too_long[156];
    for (int i = 0; i < 155; i++)
        too_long[i] = (char)('a' + (i % 26));
    too_long[155] = '\0';

    display_screens_build_setup(SETUP_TEST_SSID, SETUP_TEST_PASSWORD, too_long, SETUP_TEST_USERNAME, SETUP_TEST_URL);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_update_layout(scr);

    uint32_t n = lv_obj_get_child_count(scr);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(6, n, "a failed encode must still draw every text line, just no QR object");
    for (uint32_t i = 0; i < n; i++) {
        TEST_ASSERT_TRUE_MESSAGE(lv_obj_check_type(lv_obj_get_child(scr, i), &lv_label_class),
                                 "every child is a label when no QR was drawn");
    }
}

void test_setup_release_screen(void) {
    display_screens_build_setup_release();
    assert_matches_golden("setup_release");
}

void test_setup_end_wifi_saved_screen(void) {
    display_screens_build_setup_end(DISPLAY_SETUP_END_WIFI_SAVED, false);
    assert_matches_golden("setup_end_wifi_saved");
}

void test_setup_end_mqtt_saved_screen(void) {
    display_screens_build_setup_end(DISPLAY_SETUP_END_MQTT_SAVED, true);
    assert_matches_golden("setup_end_mqtt_saved");
}

void test_setup_end_timed_out_no_ssid_screen(void) {
    display_screens_build_setup_end(DISPLAY_SETUP_END_TIMED_OUT, false);
    assert_matches_golden("setup_end_timed_out_no_ssid");
}

void test_setup_end_timed_out_with_ssid_screen(void) {
    display_screens_build_setup_end(DISPLAY_SETUP_END_TIMED_OUT, true);
    assert_matches_golden("setup_end_timed_out_with_ssid");
}

void test_setup_end_failed_no_ssid_screen(void) {
    display_screens_build_setup_end(DISPLAY_SETUP_END_FAILED, false);
    assert_matches_golden("setup_end_failed_no_ssid");
}

void test_setup_end_failed_with_ssid_screen(void) {
    display_screens_build_setup_end(DISPLAY_SETUP_END_FAILED, true);
    assert_matches_golden("setup_end_failed_with_ssid");
}

/* The retry line's wording, independent of any golden — pins the three
   distinct messages directly, including the CONFIG_MAGTAG_BOOT_WAKES=on
   branch a host build's own sdkconfig-less compile never takes on its
   own (boot_wakes_enabled() always answers false here; this calls the
   pure formatter directly with true instead, per its own doc comment). */
void test_setup_retry_line_no_ssid_says_press_any_button(void) {
    char out[48];
    format_setup_retry_line(false, false, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Press any button to retry", out);
    format_setup_retry_line(false, true, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Press any button to retry", out);
}

void test_setup_retry_line_with_ssid_and_boot_wakes_says_hold_boot(void) {
    char out[48];
    format_setup_retry_line(true, true, out, sizeof(out));
    char expect[48];
    char secs[16];
    format_hold_seconds(SETUP_TRIGGER_BOOT_HOLD_MS, secs, sizeof(secs));
    snprintf(expect, sizeof(expect), "Hold BOOT %s s to retry", secs);
    TEST_ASSERT_EQUAL_STRING(expect, out);
}

void test_setup_retry_line_with_ssid_and_no_boot_wakes_says_press_then_hold(void) {
    char out[64];
    format_setup_retry_line(true, false, out, sizeof(out));
    char expect[64];
    char secs[16];
    format_hold_seconds(SETUP_TRIGGER_BOOT_HOLD_MS, secs, sizeof(secs));
    snprintf(expect, sizeof(expect), "Press a button, then hold BOOT %s s", secs);
    TEST_ASSERT_EQUAL_STRING(expect, out);
}

/* format_hold_seconds() itself: whole seconds render bare; a non-whole
   value gets exactly one decimal digit rather than truncating down to
   the whole second below it (the defect this replaces: a 2500 ms
   threshold used to render "2 s", which a 2.0 s hold then satisfied and
   did nothing with). */
void test_format_hold_seconds_whole_and_fractional(void) {
    char out[16];
    format_hold_seconds(5000, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("5", out);
    format_hold_seconds(1000, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("1", out);
    format_hold_seconds(15000, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("15", out);
    format_hold_seconds(2500, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("2.5", out);
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

/* BUG-14's no-clock lock: its own screen (owner decision, lock screen UX),
   separate from sync_failed, whose golden is one of the legacy files that
   must not be regenerated. */
void test_no_clock_screen(void) {
    display_screens_build_no_clock();
    assert_matches_golden("no_clock");
}

/* LVGL clips rather than wraps, so a line too long for the panel would be
   frozen into the golden as correct. Same margins as the config-error
   screen's width case. */
void test_the_no_clock_screen_fits_the_panel(void) {
    display_screens_build_no_clock();
    lv_refr_now(s_disp);
    int left, right;
    ink_columns(&left, &right);
    char msg[96];
    snprintf(msg, sizeof(msg), "ink spans x=%d..%d on a %d px panel", left, right, HOR);
    TEST_ASSERT_TRUE_MESSAGE(left >= 2, msg);
    TEST_ASSERT_TRUE_MESSAGE(right <= HOR - 3, msg);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_update_layout(scr);
    TEST_ASSERT_EQUAL_UINT32(4, lv_obj_get_child_count(scr));
    for (uint32_t i = 0; i < lv_obj_get_child_count(scr); i++) {
        lv_obj_t *o = lv_obj_get_child(scr, i);
        TEST_ASSERT_TRUE_MESSAGE(lv_obj_get_height(o) < 40, "label wrapped onto a second line");
        TEST_ASSERT_TRUE_MESSAGE(lv_obj_get_y(o) + lv_obj_get_height(o) <= VER, "label runs off the bottom edge");
    }
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

/* ---- the Screen bar's locked block (design §4.1) ----

   §4.1's own worked day throughout: 60 min, chore_free 20 min, three
   chores, so 2400 s are withheld and the block is 2400 * 280 / 3600 =
   186 px of the bar's 280 px interior.

   The bar painter reads three chore fields and no others — chore_count,
   chore_acked and chore_withheld_sec — plus allocation_sec, which is the
   denominator. chore_released is deliberately NOT one of them:
   chores_withheld_sec() already returns 0 for a released day, so the
   release reaches the panel as "nothing withheld" and there is one
   source of truth rather than two that could disagree. */
static display_state_t gated_state(uint8_t count, uint8_t acked, uint32_t withheld) {
    display_state_t st = base_state();
    st.chore_count = count;
    st.chore_acked = acked;
    st.chore_withheld_sec = withheld;
    return st;
}

void test_main_chore_gated(void) {
    /* IDLE on a gated day: remaining is the day's whole effective hour
       (app_state.c), so the fill saturates at the divider and the free
       tranche shows full — 40 min locked behind the chores, 20 min ready
       to start. */
    display_state_t st = gated_state(3, 0x00, 2400);
    display_screens_build_main(&st);
    assert_matches_golden("main_chore_gated");
}

void test_main_chore_gated_running(void) {
    /* §4.1's screen: ten of the twenty free minutes spent, one chore
       ticked. RUNNING, so the swap is refused and C carries no label. */
    display_state_t st = gated_state(3, 0x01, 2400);
    st.timer_state = TIMER_RUNNING;
    st.remaining_sec = 600;
    st.swap_available = false;
    display_screens_build_main(&st);
    assert_matches_golden("main_chore_gated_running");
}

void test_main_chore_fully_gated(void) {
    /* chore_free == 0: the block is the whole bar, there is no draining
       region, and the bar is just the text. */
    display_state_t st = gated_state(3, 0x00, 3600);
    display_screens_build_main(&st);
    assert_matches_golden("main_chore_fully_gated");
}

/* A block too NARROW for §4.1's sentence — the case the other three
   goldens all miss, and the reason the clipping bug shipped past them.
   main_chore_gated and main_chore_gated_running both carry a 186 px block
   and main_chore_fully_gated a 280 px one, which are the only two widths
   on this screen where "0/3 Chores - NN min" fits whole; every golden
   therefore agreed with a painter that clipped everything else.

   60 min day with chore_free 40 — a perfectly ordinary configuration —
   withholds 20 min, and withheld/allocation of 280 px is 93. The full
   sentence needs 127 px there, so the old painter drew "0/3 Chores - 2":
   a complete, plausible statement that 2 minutes were locked when 20
   were. This golden holds the rung the ladder drops to instead,
   "0/3 - 20 min" at 72 px inside the 85 px the block allots. */
void test_main_chore_gated_narrow(void) {
    display_state_t st = gated_state(3, 0x00, 1200);
    display_screens_build_main(&st);
    assert_matches_golden("main_chore_gated_narrow");
}

/* Rightmost ink in the bar's middle row, within the interior the 2 px
   border leaves (x 8..287). The label sits to the LEFT of the fill inside
   the block, so the rightmost black pixel is the free tranche's draining
   edge whether or not a block is drawn. */
static int32_t bar_fill_right_edge(const display_state_t *st) {
    display_screens_build_main(st);
    lv_refr_now(s_disp);
    int32_t edge = -1;
    for (int32_t x = 8; x <= 287; x++) {
        int bit = (s_captured[37 * (HOR / 8) + x / 8] >> (7 - (x & 7))) & 1;
        if (bit == 0) /* LVGL I1: 0 = black */
            edge = x;
    }
    return edge;
}

/* THE invariant, measured on the glass rather than in the arithmetic:
   "px/sec must be uniform across the boundary; a rescaled free tranche
   silently changes the bar's meaning."

   Spending 1040 s moves the draining edge by the SAME distance whether
   those seconds came out of a 20-minute free tranche beside a locked
   block or out of the whole undivided hour. A free tranche scaled to its
   own width against the day would move it about 27 px instead of 82.
   Deltas, not absolute positions: the test says nothing about where the
   divider is, only that a second is worth the same distance either side
   of it.

   Both figures sit inside the free tranche and neither saturates the bar,
   which matters — a fill at the bar's far end cannot be measured past the
   2 px border, which is black whatever the fill is doing.

   One pixel of tolerance, and exactly one. lv_bar maps its 0..280 range
   across the object's whole 284 px (measured: the indicator starts at
   x=6, the bar's outer edge, and the border is painted over its ends), so
   a value converts to pixels with a truncation of its own on top of the
   seconds->value truncation. Offsetting the free tranche by the block
   lands that second truncation differently, and the residue is ±1 px. A
   rate error is not: 2% over this span is already 2 px. */
void test_the_free_tranche_drains_at_the_same_rate_as_an_ungated_bar(void) {
    display_state_t gated = gated_state(3, 0x00, 2400);
    gated.remaining_sec = 1100; /* nearly the whole 1200 s free tranche */
    int32_t gated_hi = bar_fill_right_edge(&gated);
    gated.remaining_sec = 60; /* nearly all of it spent */
    int32_t gated_lo = bar_fill_right_edge(&gated);

    display_state_t plain = base_state();
    plain.remaining_sec = 1100;
    int32_t plain_hi = bar_fill_right_edge(&plain);
    plain.remaining_sec = 60;
    int32_t plain_lo = bar_fill_right_edge(&plain);

    int32_t gated_travel = gated_hi - gated_lo, plain_travel = plain_hi - plain_lo;
    printf("gated edge %d -> %d (%d px), ungated %d -> %d (%d px)\n", (int)gated_hi, (int)gated_lo, (int)gated_travel,
           (int)plain_hi, (int)plain_lo, (int)plain_travel);
    TEST_ASSERT_TRUE_MESSAGE(gated_travel > 0, "the gated bar did not drain");
    int32_t slip = gated_travel - plain_travel;
    if (slip < 0)
        slip = -slip;
    char msg[128];
    snprintf(msg, sizeof(msg), "1040 s moved the free tranche %d px and the undivided bar %d px", (int)gated_travel,
             (int)plain_travel);
    TEST_ASSERT_TRUE_MESSAGE(slip <= 1, msg);
    /* And the block really was there — otherwise the two bars above are
       the same bar and the equality is vacuous. */
    TEST_ASSERT_TRUE_MESSAGE(gated_lo > plain_lo, "no locked block was drawn; the rate test proves nothing");
}

/* Differing bytes between two captures over a row range. The range is
   what M2-HW-FIX forced this file to start naming: cell A's "Chores"
   label makes the BUTTON ROW a second place two chore states can differ,
   and a whole-frame compare can no longer tell that difference from a
   difference in the bar. */
static int frame_diff_rows(const uint8_t *a, const uint8_t *b, int r0, int r1) {
    int n = 0;
    for (int r = r0; r <= r1; r++) {
        for (int col = 0; col < HOR / 8; col++) {
            if (a[r * (HOR / 8) + col] != b[r * (HOR / 8) + col])
                n++;
        }
    }
    return n;
}

/* Everything above the bottom button row. The row's labels are anchored
   at y=-2 and measure rows 113..122 (printed by
   test_the_main_chore_label_clears_the_status_row_and_button_b); 110 is
   the anchor's own box top and so is the conservative boundary — a
   literal, not an expression over the source constants, so a mistake in
   those constants cannot move the boundary in lockstep with a bug. */
#define BUTTON_ROW_TOP 110

/* §4.1: "on release the block simply vanishes and the bar goes full width
   with the ordinary remaining/allocation fill". Byte-identical to a day
   that never had chores ABOVE THE BUTTON ROW, which is the strongest form
   of "vanishes" that is still true.

   WHY THE ROW IS SPLIT OFF RATHER THAN IGNORED. §4.1's claim is about the
   BAR: release zeroes chores_withheld_sec(), so no block is drawn. It was
   never a claim about the LIST, which release does not delete —
   button_a_toggle_allowed() still says yes on a released day (no refusal
   reason applies), the checklist is still reachable and still worth
   reaching, so cell A still reads "Chores". Until M2-HW-FIX the main
   screen had no cell A and the two claims could share one memcmp; now a
   whole-frame compare fails for the second reason while the first is
   perfectly satisfied. So both are asserted, separately, and each can
   fail on its own: the bar is identical, and the ONLY thing the released
   day adds is the label, in cell A's own window. */
void test_the_locked_block_vanishes_when_the_day_releases(void) {
    static uint8_t ungated[FB_BYTES];
    /* Both ends of the bar, because they hide different mistakes. A full
       bar is black from its own left edge, so anything drawn at zero
       width there is black on black and invisible; an empty one shows it.
       (Measured: a mutant that built the block unconditionally survived
       the full-bar case alone and was caught only by an unrelated
       golden.) */
    static const int32_t REMAINING[] = {3600, 0};
    for (size_t i = 0; i < sizeof(REMAINING) / sizeof(REMAINING[0]); i++) {
        display_state_t plain = base_state();
        plain.remaining_sec = REMAINING[i];
        display_screens_build_main(&plain);
        lv_refr_now(s_disp);
        memcpy(ungated, s_captured, FB_BYTES);

        /* Absolute, not relative: an emptied ungated bar has nothing
           inside its border at all. The comparisons below cannot say
           this — a block drawn unconditionally appears on BOTH frames and
           cancels — and outside the goldens nothing else would. */
        if (REMAINING[i] == 0) {
            for (int32_t y = 28; y <= 47; y++) {
                for (int32_t x = 8; x <= 287; x++) {
                    int bit = (ungated[y * (HOR / 8) + x / 8] >> (7 - (x & 7))) & 1;
                    char msg[96];
                    snprintf(msg, sizeof(msg), "ink at x=%d y=%d inside an empty ungated bar", (int)x, (int)y);
                    TEST_ASSERT_EQUAL_HEX8_MESSAGE(1, bit, msg);
                }
            }
        }

        display_state_t shut = gated_state(3, 0x00, 2400);
        shut.remaining_sec = REMAINING[i];
        display_screens_build_main(&shut);
        lv_refr_now(s_disp);
        /* ABOVE THE ROW, because below it the "Chores" label alone would
           satisfy a whole-frame inequality and this assertion would pass
           on a painter that never drew a block at all. */
        TEST_ASSERT_TRUE_MESSAGE(frame_diff_rows(ungated, s_captured, 0, BUTTON_ROW_TOP - 1) > 0,
                                 "the gate changed no pixel above the button row - the block never painted");

        display_state_t open = gated_state(3, 0x07, 0); /* released: nothing withheld */
        open.remaining_sec = REMAINING[i];
        open.chore_released = true;
        display_screens_build_main(&open);
        lv_refr_now(s_disp);
        char msg[96];
        snprintf(msg, sizeof(msg), "a released day with %ld s left still carries the locked block", (long)REMAINING[i]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, frame_diff_rows(ungated, s_captured, 0, BUTTON_ROW_TOP - 1), msg);

        /* And the row below: a released day still has a list, so cell A
           still offers it — and nothing ELSE in the row may move. Bytes
           0..5 are x=0..47, which contains the 43 px "Chores" label at
           x=4 (ending at x=47) with cell B's ink well clear of it.

           The clearance argued from the geometry rather than from one
           label's measured x, because cell B's width is NOT fixed — it is
           a PLAY glyph, a PAUSE glyph or the word "Reload" depending on
           display_button_b_label(), and a figure read off whichever one a
           given case renders is not a fact about the boundary. What is
           fixed: every cell-B label is CENTRED on button B, at
           BTN_MID_OFS(1) = BTN_X0 + BTN_PITCH = x 91. Ink symmetric about
           91 can only reach x=47 once the label is 88 px wide, and the
           widest of the three ("Reload" at 12 pt, ~42 px) is less than
           half that. So the split at byte 5 is safe for every B label,
           present and added later, rather than for the one on screen. */
        int outside = 0, inside = 0;
        for (int r = BUTTON_ROW_TOP; r < VER; r++) {
            for (int col = 0; col < HOR / 8; col++) {
                int idx = r * (HOR / 8) + col;
                if (ungated[idx] == s_captured[idx])
                    continue;
                if (col <= 5)
                    inside++;
                else
                    outside++;
            }
        }
        snprintf(msg, sizeof(msg), "a released day with %ld s left moved the button row outside cell A",
                 (long)REMAINING[i]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, outside, msg);
        TEST_ASSERT_TRUE_MESSAGE(inside > 0, "a released day with a list drew no Chores label over button A");
    }
}

/* Row C1's off switch, re-tested on the panel. chores_withheld_sec()
   already returns 0 for an empty list, so this state cannot arrive from
   app_state.c — the painter re-tests it because it is handed a snapshot
   it cannot re-derive, and "0/0 Chores" is the one string this screen
   could never mean.

   It is also, deliberately, what the panel does about S2: a list emptied
   mid-day leaves the withheld remainder stranded in the MODEL, and no
   painter can invent a block for a list with no rows to tick. The bar
   going back to undivided is the correct drawing of a wrong day; the
   shortfall belongs to whoever fixes the mechanism. */
void test_an_emptied_list_draws_no_block(void) {
    static uint8_t ungated[FB_BYTES];
    display_state_t plain = base_state();
    display_screens_build_main(&plain);
    lv_refr_now(s_disp);
    memcpy(ungated, s_captured, FB_BYTES);

    display_state_t emptied = gated_state(0, 0x00, 2400);
    display_screens_build_main(&emptied);
    lv_refr_now(s_disp);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(ungated, s_captured, FB_BYTES, "a list with no rows still drew a locked block");
}

/* The badge is built after the block so it rides over it, exactly as it
   already rides over the fill. Its background is opaque, so its whole
   bounding box must render identically gated and ungated — the box is
   derived here by diffing a badged frame against an unbadged one rather
   than guessed, and it straddles the divider at the worked day's 186 px,
   which is what gives the test its teeth. Painted the other way round,
   the block's white interior and its divider would come back through. */
void test_the_charge_me_badge_still_rides_over_the_locked_block(void) {
    static uint8_t plain_badge[FB_BYTES], plain_bare[FB_BYTES];
    display_state_t plain = base_state();
    display_screens_build_main(&plain);
    lv_refr_now(s_disp);
    memcpy(plain_bare, s_captured, FB_BYTES);
    plain.charge_warn = true;
    display_screens_build_main(&plain);
    lv_refr_now(s_disp);
    memcpy(plain_badge, s_captured, FB_BYTES);

    int32_t x0 = HOR, x1 = -1, y0 = VER, y1 = -1;
    for (int32_t y = 0; y < VER; y++) {
        for (int32_t x = 0; x < HOR; x++) {
            size_t i = (size_t)y * (HOR / 8) + (size_t)x / 8;
            int b = 7 - (x & 7);
            if (((plain_badge[i] >> b) & 1) == ((plain_bare[i] >> b) & 1))
                continue;
            if (x < x0)
                x0 = x;
            if (x > x1)
                x1 = x;
            if (y < y0)
                y0 = y;
            if (y > y1)
                y1 = y;
        }
    }
    printf("Charge Me!!! badge box x %d..%d y %d..%d (divider at x=194)\n", (int)x0, (int)x1, (int)y0, (int)y1);
    TEST_ASSERT_TRUE_MESSAGE(x1 > x0 && y1 > y0, "the badge changed nothing - nothing to compare");
    TEST_ASSERT_TRUE_MESSAGE(x1 >= 190, "the badge does not reach the divider; this test would prove nothing");

    display_state_t gated = gated_state(3, 0x00, 2400);
    gated.charge_warn = true;
    display_screens_build_main(&gated);
    lv_refr_now(s_disp);
    for (int32_t y = y0; y <= y1; y++) {
        for (int32_t x = x0; x <= x1; x++) {
            size_t i = (size_t)y * (HOR / 8) + (size_t)x / 8;
            int b = 7 - (x & 7);
            char msg[96];
            snprintf(msg, sizeof(msg), "x=%d y=%d differs - the locked block painted over the badge", (int)x, (int)y);
            TEST_ASSERT_EQUAL_HEX8_MESSAGE((plain_badge[i] >> b) & 1, (s_captured[i] >> b) & 1, msg);
        }
    }
}

/* The trap the M2 notes flagged for T1, arriving on the panel: the gate is
   always slot 0's DAY, while allocation_sec follows the SELECTION. With an
   extra timer drawn, 2400 s against its 10 minutes would swallow the whole
   bar and mean nothing. The block belongs to Screen's paint only. */
void test_an_extra_timer_never_carries_the_days_locked_block(void) {
    static uint8_t meditation[FB_BYTES];
    display_state_t st = gated_state(3, 0x00, 0);
    st.timer_name = "Meditation";
    st.timer_state = TIMER_RUNNING;
    st.allocation_sec = 600;
    st.remaining_sec = 400;
    st.swap_available = false;
    display_screens_build_main(&st);
    lv_refr_now(s_disp);
    memcpy(meditation, s_captured, FB_BYTES);

    st.chore_withheld_sec = 2400; /* the day's gate, not this timer's */
    display_screens_build_main(&st);
    lv_refr_now(s_disp);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(meditation, s_captured, FB_BYTES,
                                     "the day's locked block reached an extra timer's bar");
}

/* The cap clips the DRAWING, not just the object — the same property the
   version and mode rows are pinned for. A 10 min withholding of a 60 min
   day is a 46 px block (46 * 3600 / 280 -> the design's own rate) holding
   a label that wants ~100 px, and the corridor to its right has to stay
   white or black text lands on the draining fill.

   Bounds are spelled out rather than measured: the block's box is
   BAR_X=6 plus its 2 px border, 46 px of interior and the 2 px divider,
   so it ends at x=55 and the scan starts at 56. Rows are the bar's
   interior, 28..47, the border rows 26/27 and 48/49 being black by
   design. remaining_sec = 0 so the free tranche is empty and the whole
   corridor is white; the state word above it is not what is under test. */
/* The label the locked block is holding, or NULL if it holds none. The
   block is the only lv_bar on the main screen with a child object: the
   progress bar it is laid over has none (LVGL draws a bar's indicator as
   a draw part, not an object), and the two otherwise share x, y and
   sometimes width, so a child is the only thing that tells them apart. */
static lv_obj_t *locked_label(void) {
    lv_obj_t *scr = lv_screen_active();
    for (uint32_t i = 0; i < lv_obj_get_child_count(scr); i++) {
        lv_obj_t *o = lv_obj_get_child(scr, i);
        if (!lv_obj_check_type(o, &lv_bar_class) || lv_obj_get_child_count(o) == 0)
            continue;
        lv_obj_t *c = lv_obj_get_child(o, 0);
        if (lv_obj_check_type(c, &lv_label_class))
            return c;
    }
    return NULL;
}

/* The margin between the last glyph and the divider, on the TIGHTEST
   block the config domain can produce — an 8 min day with chore_free 6
   withholds 2 min, which is 70 px of the 280, allotting exactly 62 px,
   and "0/1 - 2 min" measures exactly 62. Zero slack anywhere in the
   sweep, so the margin here is precisely LOCKED_LABEL_PAD on each side
   and one pixel of drift in either direction fails.

   Deliberately NOT the 46 px block this test used before the fit ladder
   landed. 46 px allots 38 px and the narrowest rung ("10 min") needs 39,
   so the painter now draws NO LABEL there — and a corridor scan with no
   label in it passes for the wrong reason. The old version asserted the
   margin of a label that, before the ladder, was a half-rendered "0/3 C"
   and, after it, does not exist. Hence the explicit text assertion
   below: this test must fail, not pass, if the ladder ever stops
   drawing here. */
void test_the_locked_label_cannot_overstrike_the_free_tranche(void) {
    display_state_t st = gated_state(1, 0x00, 120);
    st.allocation_sec = 480;
    st.remaining_sec = 0;
    display_screens_build_main(&st);
    lv_refr_now(s_disp);

    /* Anti-vacuity: the corridor below is white whether the label fits or
       was never drawn, so pin which rung is on the glass. */
    lv_obj_t *lbl = locked_label();
    TEST_ASSERT_NOT_NULL_MESSAGE(lbl, "no label on a 70 px block - the corridor scan below would pass vacuously");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("0/1 - 2 min", lv_label_get_text(lbl),
                                     "the tightest-fitting rung is not the one being measured");

    /* With the free tranche empty the divider is the ONLY thing marking
       the boundary — everywhere else it is the left edge of the fill and
       black on black. Row 29 is above the label, so the block's interior
       is clean there and the two border columns stand alone.
       Block object is 70 + 2*2 = 74 px at x=6, so it spans x=6..79 and
       its right border — the divider — is x=78,79. */
    static const struct {
        int32_t x;
        int expect;
    } EDGE[] = {{77, 1}, {78, 0}, {79, 0}, {80, 1}}; /* interior | divider | free */
    for (size_t i = 0; i < sizeof(EDGE) / sizeof(EDGE[0]); i++) {
        int bit = (s_captured[29 * (HOR / 8) + EDGE[i].x / 8] >> (7 - (EDGE[i].x & 7))) & 1;
        char msg[96];
        snprintf(msg, sizeof(msg), "x=%d should be %s - the block's right edge is not where it belongs", (int)EDGE[i].x,
                 EDGE[i].expect ? "white" : "black");
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(EDGE[i].expect, bit, msg);
    }

    /* Two corridors, because LVGL clips a child to its parent and the cap
       does something the clip does not. The clip alone stops the label at
       the block's outer edge — which is the divider, black on black, so
       an uncapped label reaching it is invisible there and a mutant that
       drops the cap survives a scan of the free tranche. The cap's real
       job is the margin: LOCKED_LABEL_PAD of clean white between the last
       glyph and the divider, so the block reads as a box with a word in
       it rather than as text wedged against a line.

       x 74..77 is that margin (block interior 8..77, label from x=12 and
       exactly 62 px wide, so ink ends at x=73); x 80..287 is the free
       tranche, which the clip and the fit both have to keep white.
       Bounds are the constants, not measured edges. */
    for (int32_t y = 28; y <= 47; y++) {
        for (int32_t x = 74; x <= 287; x++) {
            if (x > 77 && x < 80)
                continue; /* the divider itself: black by design */
            int bit = (s_captured[y * (HOR / 8) + x / 8] >> (7 - (x & 7))) & 1;
            char msg[112];
            snprintf(msg, sizeof(msg), "ink at x=%d y=%d - the locked label spilled out of a 70 px block%s", (int)x,
                     (int)y, (x <= 77) ? " (into the divider's margin)" : "");
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(1, bit, msg); /* LVGL I1: 1 = white */
        }
    }

    /* And the LEFT inset, x 8..11 — the block's interior before the
       label starts. Asserted here because the corridor above cannot see
       it: drop LOCKED_LABEL_PAD from the label's offset and every glyph
       shifts 4 px LEFT, which moves ink out of the right margin rather
       than into it, so the scan above goes greener rather than redder.
       Only the goldens caught that, and a golden says "a pixel moved",
       not "the label lost its inset". */
    for (int32_t y = 28; y <= 47; y++) {
        for (int32_t x = 8; x <= 11; x++) {
            int bit = (s_captured[y * (HOR / 8) + x / 8] >> (7 - (x & 7))) & 1;
            char msg[112];
            snprintf(msg, sizeof(msg), "ink at x=%d y=%d - the label lost its %d px inset from the block's left edge",
                     (int)x, (int)y, LOCKED_LABEL_PAD);
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(1, bit, msg);
        }
    }
}

/* Width of `txt` at 12 pt with nothing constraining it — the width the
   block has to find room for if the whole string is to be drawn.
   Deliberately measured on a THROWAWAY label rather than on the painter's
   own: lv_obj_get_width() of a label carrying a max-width style returns
   the cap, so asking the real label how wide it is would answer "no wider
   than its cap" whatever it holds, and the assertion below would pass on
   a string cut in half. */
static int32_t text_w_12(const char *txt) {
    lv_obj_t *probe = lv_label_create(lv_screen_active());
    lv_label_set_text(probe, txt);
    lv_obj_set_style_text_font(probe, &lv_font_montserrat_12, 0);
    lv_obj_update_layout(probe);
    int32_t w = lv_obj_get_width(probe);
    lv_obj_delete(probe);
    return w;
}

/* THE invariant for the block's label: no rendered label may state a
   number the day does not have.

   Clipping is not a cosmetic failure here. "0/3 Chores - 20 min" cut at
   a 93 px block reads "0/3 Chores - 2", which is a complete, plausible
   and wrong statement about the same day — worse than no figure at all.
   So the property is that whatever string the painter chose FITS whole,
   swept over every block width a configured day can produce rather than
   sampled at the two comfortable ones the goldens happen to use.

   Allocation runs to 240 min (four hours of screen time is already well
   past anything this schedule offers) in the 5 min steps the config is
   entered in, and chore_free over every value it can take for each. The
   four chore lists cover the digits the string can carry and both
   forms — 8/8 on a released-looking list is the widest, 0/0 the widest
   minutes figure. */
void test_no_block_width_can_truncate_the_locked_label(void) {
    static const struct {
        uint8_t count;
        uint8_t acked;
    } LIST[] = {{3, 0x00}, {3, 0x01}, {8, 0xFF}, {1, 0x00}};
    for (size_t l = 0; l < sizeof(LIST) / sizeof(LIST[0]); l++) {
        for (uint32_t alloc_min = 5; alloc_min <= 240; alloc_min += 5) {
            for (uint32_t free_min = 0; free_min <= alloc_min; free_min += 5) {
                const uint32_t alloc = alloc_min * 60, withheld = (alloc_min - free_min) * 60;
                display_state_t st = gated_state(LIST[l].count, LIST[l].acked, withheld);
                st.allocation_sec = alloc;
                st.remaining_sec = (int32_t)alloc;
                display_screens_build_main(&st);

                lv_obj_t *lbl = locked_label();
                if (lbl == NULL)
                    continue; /* no label drawn states no number: honest by construction */
                const display_bar_split_t sp = display_bar_split((int32_t)alloc, alloc, withheld);
                const int32_t allotted = (int32_t)sp.locked_px - 2 * LOCKED_LABEL_PAD;
                const int32_t need = text_w_12(lv_label_get_text(lbl));
                char msg[192];
                snprintf(msg, sizeof(msg),
                         "%u min day, chore_free %u, %u chores: a %u px block allots %d px but \"%s\" needs %d - the "
                         "figure is cut",
                         (unsigned)alloc_min, (unsigned)free_min, (unsigned)LIST[l].count, (unsigned)sp.locked_px,
                         (int)allotted, lv_label_get_text(lbl), (int)need);
                TEST_ASSERT_TRUE_MESSAGE(need <= allotted, msg);
            }
        }
    }
}

/* display.c's CLEAN_BANDS gives the bar {26, 49} — "progress bar (y=26,
   h=24) + Charge Me!!! badge". The block shares the bar's box exactly, so
   that table needs no entry of its own; this is what keeps it true. A
   block or label that grew a row either way would be inverted twice by
   the ghost-cleaning double partial, or not at all. */
void test_the_locked_block_stays_inside_the_progress_bar_band(void) {
    display_state_t st = gated_state(3, 0x00, 2400);
    display_screens_build_main(&st);
    lv_refr_now(s_disp);
    assert_rows_blank(18, 25);
    assert_rows_blank(50, 57);

    st.chore_withheld_sec = 3600; /* fully gated: the block is the whole bar */
    display_screens_build_main(&st);
    lv_refr_now(s_disp);
    assert_rows_blank(18, 25);
    assert_rows_blank(50, 57);
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
    RUN_TEST(test_the_main_screen_offers_chores_exactly_when_button_a_would_act);
    RUN_TEST(test_the_main_chore_label_clears_the_status_row_and_button_b);
    RUN_TEST(test_break_screen_no_eligible);
    RUN_TEST(test_break_screen_with_chores);
    RUN_TEST(test_break_screen_with_chores_no_eligible);
    RUN_TEST(test_break_screen_with_every_chore_done);
    RUN_TEST(test_the_break_chore_line_clears_the_countdown_and_the_row);
    RUN_TEST(test_the_break_chore_row_fits_its_cells);
    RUN_TEST(test_the_break_row_without_a_swap_keeps_its_chore_cell);
    RUN_TEST(test_the_break_chore_line_items_never_meet);
    RUN_TEST(test_the_break_chore_prompt_counts_what_is_done);
    RUN_TEST(test_the_break_chore_tick_matches_the_checklist_glyph);
    RUN_TEST(test_a_single_chore_still_earns_the_break_screen_layout);
    RUN_TEST(test_the_break_screen_offers_chores_exactly_when_the_list_is_non_empty);
    RUN_TEST(test_main_low_battery_warn_badge);
    RUN_TEST(test_main_idle_weekday_adjusted);
    RUN_TEST(test_mode_row_fits_beside_the_state_word);
    RUN_TEST(test_mode_row_cap_clips_the_drawing_not_just_the_object);
    RUN_TEST(test_version_fits_the_battery_row);
    RUN_TEST(test_version_cap_clips_the_drawing_not_just_the_object);
    RUN_TEST(test_no_version_renders_the_row_unchanged);
    RUN_TEST(test_config_error_screen);
    RUN_TEST(test_the_config_error_screen_fits_the_panel_at_its_widest);
    RUN_TEST(test_the_config_error_screen_renders_the_pair_it_is_given);
    RUN_TEST(test_setup_screen);
    RUN_TEST(test_setup_screens_own_test_username_matches_the_real_qr_username);
    RUN_TEST(test_setup_screen_qr_matches_the_encoding);
    RUN_TEST(test_the_setup_screen_clips_a_31_byte_ssid_without_overflowing_its_column);
    RUN_TEST(test_the_setup_screen_falls_back_to_text_when_the_payload_is_too_long);
    RUN_TEST(test_setup_release_screen);
    RUN_TEST(test_setup_end_wifi_saved_screen);
    RUN_TEST(test_setup_end_mqtt_saved_screen);
    RUN_TEST(test_setup_end_timed_out_no_ssid_screen);
    RUN_TEST(test_setup_end_timed_out_with_ssid_screen);
    RUN_TEST(test_setup_end_failed_no_ssid_screen);
    RUN_TEST(test_setup_end_failed_with_ssid_screen);
    RUN_TEST(test_setup_retry_line_no_ssid_says_press_any_button);
    RUN_TEST(test_setup_retry_line_with_ssid_and_boot_wakes_says_hold_boot);
    RUN_TEST(test_setup_retry_line_with_ssid_and_no_boot_wakes_says_press_then_hold);
    RUN_TEST(test_format_hold_seconds_whole_and_fractional);
    RUN_TEST(test_ota_screen);
    RUN_TEST(test_ota_screen_lines_fit_the_panel);
    RUN_TEST(test_charge_me_screen);
    RUN_TEST(test_timesup_screen);
    RUN_TEST(test_sync_failed_screen);
    RUN_TEST(test_no_clock_screen);
    RUN_TEST(test_the_no_clock_screen_fits_the_panel);
    RUN_TEST(test_bedtime_screen);
    RUN_TEST(test_chore_screen_one_acked);
    RUN_TEST(test_chore_screen_all_acked);
    RUN_TEST(test_chore_screen_two_chores);
    RUN_TEST(test_chore_rows_are_fixed_whatever_the_list_holds);
    RUN_TEST(test_a_pathological_chore_name_cannot_overstrike_the_panel);
    RUN_TEST(test_a_stale_ack_bit_above_the_count_changes_no_pixel);
    RUN_TEST(test_the_chore_button_row_fits_its_cells);
    RUN_TEST(test_main_chore_gated);
    RUN_TEST(test_main_chore_gated_running);
    RUN_TEST(test_main_chore_fully_gated);
    RUN_TEST(test_main_chore_gated_narrow);
    RUN_TEST(test_the_free_tranche_drains_at_the_same_rate_as_an_ungated_bar);
    RUN_TEST(test_the_locked_block_vanishes_when_the_day_releases);
    RUN_TEST(test_an_emptied_list_draws_no_block);
    RUN_TEST(test_the_charge_me_badge_still_rides_over_the_locked_block);
    RUN_TEST(test_an_extra_timer_never_carries_the_days_locked_block);
    RUN_TEST(test_the_locked_label_cannot_overstrike_the_free_tranche);
    RUN_TEST(test_no_block_width_can_truncate_the_locked_label);
    RUN_TEST(test_the_locked_block_stays_inside_the_progress_bar_band);
    return UNITY_END();
}
