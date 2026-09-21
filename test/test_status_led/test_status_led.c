#include <unity.h>

/* Single-TU: the pure state->RGB table plus its one device wrapper, with
   the NeoPixel driver and the timer state machine resolved by the stubs
   below so the table can be asserted without hardware.

   chores.c comes along for the plainest reason: status_led.c calls
   chores_is_acked(), so this TU needs the definition to link. Pulling in
   the real .c rather than stubbing it is the house style here — eight
   other suites do the same. It is NOT what gives the stale-high-bit test
   below its meaning; that comes from the loop bound in chores_led_for(),
   and the test passes with chores_is_acked() reduced to a bare shift. */
// clang-format off
#include "../../main/chores.c"
#include "../../main/status_led.c"
// clang-format on

/* ---- link-time stubs ---------------------------------------------------- */

static int stub_pixel_calls;
static int stub_pixel_idx;
static uint8_t stub_pixel_r, stub_pixel_g, stub_pixel_b;

/* Every pixel write, in order: the chore painter writes the whole strip,
   so "what did the last call say" cannot see a pixel being missed, and a
   missed pixel is the interesting failure (it leaves whatever the timer
   state or a network window last put there). */
#define MAX_RECORDED 16
static int stub_seen_idx[MAX_RECORDED];
static uint8_t stub_seen_r[MAX_RECORDED], stub_seen_g[MAX_RECORDED], stub_seen_b[MAX_RECORDED];

/* The CLASS each write went out as, counted separately. Both classes record
   into the arrays above, so every colour and mapping assertion in this file
   is class-agnostic and keeps meaning what it meant; these two are what let
   a case ask the one question the colours cannot answer — whether quiet
   hours can silence the write. STATUS is dropped while the quiet callback
   is true and HIGHPRI is not (neopixel.h), and for the chore strip that is
   the difference between a night-time ack showing something and showing
   nothing at all. */
static int stub_status_calls;
static int stub_hi_calls;

static void stub_record(int idx, uint8_t r, uint8_t g, uint8_t b) {
    if (stub_pixel_calls < MAX_RECORDED) {
        stub_seen_idx[stub_pixel_calls] = idx;
        stub_seen_r[stub_pixel_calls] = r;
        stub_seen_g[stub_pixel_calls] = g;
        stub_seen_b[stub_pixel_calls] = b;
    }
    stub_pixel_calls++;
    stub_pixel_idx = idx;
    stub_pixel_r = r;
    stub_pixel_g = g;
    stub_pixel_b = b;
}

void neopixel_status_pixel(int idx, uint8_t r, uint8_t g, uint8_t b) {
    stub_status_calls++;
    stub_record(idx, r, g, b);
}

void neopixel_highpri_pixel(int idx, uint8_t r, uint8_t g, uint8_t b) {
    stub_hi_calls++;
    stub_record(idx, r, g, b);
}

static timer_state_t stub_state;
static int stub_state_reads;

timer_state_t timer_get_state(void) {
    stub_state_reads++;
    return stub_state;
}

void setUp(void) {
    stub_pixel_calls = 0;
    stub_status_calls = 0;
    stub_hi_calls = 0;
    stub_pixel_idx = -1;
    stub_pixel_r = stub_pixel_g = stub_pixel_b = 0xFF;
    stub_state = TIMER_IDLE;
    stub_state_reads = 0;
    for (int i = 0; i < MAX_RECORDED; i++) {
        stub_seen_idx[i] = -1;
        stub_seen_r[i] = stub_seen_g[i] = stub_seen_b[i] = 0xFF;
    }
}

void tearDown(void) {}

#define ASSERT_RGB(exp_r, exp_g, exp_b, rgb)     \
    do {                                         \
        TEST_ASSERT_EQUAL_UINT8((exp_r), rgb.r); \
        TEST_ASSERT_EQUAL_UINT8((exp_g), rgb.g); \
        TEST_ASSERT_EQUAL_UINT8((exp_b), rgb.b); \
    } while (0)

/* ---- the table ---------------------------------------------------------- */

/* Traffic-light convention. The exact triples are the contract: they are
   read off the device by eye during a slow e-ink refresh, so a "harmless"
   brightness tweak is a visible behaviour change. */

void test_running_is_green(void) {
    status_led_rgb_t rgb = status_led_for_state(TIMER_RUNNING);
    ASSERT_RGB(0, 20, 0, rgb);
}

void test_paused_is_amber(void) {
    status_led_rgb_t rgb = status_led_for_state(TIMER_PAUSED);
    ASSERT_RGB(25, 15, 0, rgb);
}

void test_expired_is_red(void) {
    status_led_rgb_t rgb = status_led_for_state(TIMER_EXPIRED);
    ASSERT_RGB(25, 0, 0, rgb);
}

void test_break_is_blue_cyan(void) {
    status_led_rgb_t rgb = status_led_for_state(TIMER_BREAK);
    ASSERT_RGB(0, 10, 25, rgb);
}

void test_idle_falls_through_to_white(void) {
    /* IDLE has no arm of its own — it reaches the default with everything
       else, and white is what "no timer" looks like. */
    status_led_rgb_t rgb = status_led_for_state(TIMER_IDLE);
    ASSERT_RGB(10, 10, 10, rgb);
}

void test_unknown_state_falls_through_to_white(void) {
    /* Snapshots store the state as a uint8, so a corrupt or future value
       can reach here; it must light something rather than nothing. */
    status_led_rgb_t rgb = status_led_for_state((timer_state_t)(TIMER_BREAK + 1));
    ASSERT_RGB(10, 10, 10, rgb);
}

void test_table_is_total(void) {
    /* Every enumerator maps to a lit pixel — a dark pixel would read as
       "device asleep" mid-refresh. */
    const timer_state_t all[] = {TIMER_IDLE, TIMER_RUNNING, TIMER_PAUSED, TIMER_EXPIRED, TIMER_BREAK};
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        status_led_rgb_t rgb = status_led_for_state(all[i]);
        TEST_ASSERT_TRUE_MESSAGE(rgb.r || rgb.g || rgb.b, "state maps to an unlit pixel");
    }
}

/* ---- the wrapper -------------------------------------------------------- */

void test_wrapper_drives_the_state_pixel_from_the_timer(void) {
    stub_state = TIMER_RUNNING;
    status_led_show_timer_state();
    TEST_ASSERT_EQUAL_INT(1, stub_pixel_calls);
    TEST_ASSERT_EQUAL_INT(NP_STATE_PIXEL, stub_pixel_idx);
    TEST_ASSERT_EQUAL_UINT8(0, stub_pixel_r);
    TEST_ASSERT_EQUAL_UINT8(20, stub_pixel_g);
    TEST_ASSERT_EQUAL_UINT8(0, stub_pixel_b);
}

void test_wrapper_reads_the_timer_exactly_once(void) {
    /* One read per call, or the pixel can disagree with the state that
       the same wake went on to render: the orchestration mutates the
       timer between steps, so a duplicated or hoisted read is how a
       stale colour gets latched until the next wake. */
    stub_state = TIMER_PAUSED;
    status_led_show_timer_state();
    TEST_ASSERT_EQUAL_INT(1, stub_state_reads);
    TEST_ASSERT_EQUAL_INT(1, stub_pixel_calls);
}

void test_wrapper_writes_pixel_zero(void) {
    /* Hardcoded 0, not NP_STATE_PIXEL: the state pixel and the WiFi pixel
       (private to net_window.c) are read side by side on the device in
       every mode but the chore checklist, so the index is a fixed
       contract rather than whatever the constant happens to say. */
    stub_state = TIMER_BREAK;
    status_led_show_timer_state();
    TEST_ASSERT_EQUAL_INT(0, stub_pixel_idx);
    TEST_ASSERT_EQUAL_UINT8(0, stub_pixel_r);
    TEST_ASSERT_EQUAL_UINT8(10, stub_pixel_g);
    TEST_ASSERT_EQUAL_UINT8(25, stub_pixel_b);
}

/* ---- the chore strip: the mapping --------------------------------------- */

/* Read a chore's / the gate's pixel THROUGH the mapping table, so every
   semantic test below stays true if M2-HW2 inverts the strip. Exactly two
   tests pin the mapping itself, and they are the ones HW2 edits. */
#define CHORE_PX(tbl, i) ((tbl).px[k_chore_pixel[(i)]])
#define GATE_PX(tbl) ((tbl).px[k_chore_pixel[CHORES_LED_GATE_SLOT]])

void test_the_mapping_covers_every_pixel_exactly_once(void) {
    /* Three chores plus the gate is the whole strip, so the four slots
       must be a permutation of 0..NEOPIXEL_COUNT-1. An HW2 edit that
       duplicated one index would silently leave another pixel holding
       whatever the previous paint put there — the timer state, or a
       network window's blue. */
    bool seen[NEOPIXEL_COUNT] = {false};
    for (unsigned s = 0; s <= CHORES_LED_GATE_SLOT; s++) {
        TEST_ASSERT_LESS_THAN_UINT(NEOPIXEL_COUNT, k_chore_pixel[s]);
        TEST_ASSERT_FALSE_MESSAGE(seen[k_chore_pixel[s]], "two slots share a pixel");
        seen[k_chore_pixel[s]] = true;
    }
    for (int p = 0; p < NEOPIXEL_COUNT; p++) {
        TEST_ASSERT_TRUE_MESSAGE(seen[p], "a pixel is owned by no slot");
    }
}

void test_the_mapping_is_button_order_until_hardware_says_otherwise(void) {
    /* THE unverified assumption, pinned so that changing it is deliberate.
       It reads "pixel i sits over button i": button A (the mode toggle) is
       pixel 0, so the gate lands there, and the ack buttons B/C/D —
       BUTTON_CHORE_IDX_B/C/D = chores 0/1/2 — are pixels 1/2/3.

       The only support for that reading anywhere in the tree is a comment
       (neopixel.h, on the 4-bit binary display), and nothing depends on
       that comment's "over button A" clause, so nothing has ever tested
       it. M2-HW2 looks at a board; if the strip runs the other way it
       edits k_chore_pixel to {2, 1, 0, 3} and this test to match, and
       every other test in this file keeps passing unchanged. */
    TEST_ASSERT_EQUAL_UINT8(1, k_chore_pixel[0]);
    TEST_ASSERT_EQUAL_UINT8(2, k_chore_pixel[1]);
    TEST_ASSERT_EQUAL_UINT8(3, k_chore_pixel[2]);
    TEST_ASSERT_EQUAL_UINT8(0, k_chore_pixel[CHORES_LED_GATE_SLOT]);
}

/* ---- the chore strip: the colours --------------------------------------- */

void test_an_outstanding_chore_is_red(void) {
    chores_led_t t = chores_led_for(0x00, 3, false);
    ASSERT_RGB(25, 0, 0, CHORE_PX(t, 0));
    ASSERT_RGB(25, 0, 0, CHORE_PX(t, 1));
    ASSERT_RGB(25, 0, 0, CHORE_PX(t, 2));
}

void test_an_acked_chore_is_green(void) {
    chores_led_t t = chores_led_for(0x01, 3, false);
    ASSERT_RGB(0, 20, 0, CHORE_PX(t, 0));
}

void test_each_chore_lands_on_its_own_pixel(void) {
    /* Ack only the middle row: if the painter's index arithmetic were off
       by one the green would show up under the wrong name, which is the
       failure design 2.5 calls worse than no feedback at all. */
    chores_led_t t = chores_led_for(0x02, 3, false);
    ASSERT_RGB(25, 0, 0, CHORE_PX(t, 0));
    ASSERT_RGB(0, 20, 0, CHORE_PX(t, 1));
    ASSERT_RGB(25, 0, 0, CHORE_PX(t, 2));
}

void test_the_chore_colours_are_the_timer_traffic_light(void) {
    /* Same device, same eyes, one vocabulary: the chore rows must not
       teach a second green and a second red. Tied to the timer table so
       a tweak to either side has to be a deliberate change to both. */
    chores_led_t t = chores_led_for(0x01, 2, false);
    status_led_rgb_t green = status_led_for_state(TIMER_RUNNING);
    status_led_rgb_t red = status_led_for_state(TIMER_EXPIRED);
    ASSERT_RGB(green.r, green.g, green.b, CHORE_PX(t, 0));
    ASSERT_RGB(red.r, red.g, red.b, CHORE_PX(t, 1));
}

void test_all_done_and_released_is_the_whole_strip_green(void) {
    chores_led_t t = chores_led_for(0x07, 3, true);
    for (int p = 0; p < NEOPIXEL_COUNT; p++) {
        ASSERT_RGB(0, 20, 0, t.px[p]);
    }
}

/* ---- the chore strip: the gate is the fourth pixel ----------------------- */

void test_the_gate_pixel_is_red_while_time_is_locked(void) {
    chores_led_t t = chores_led_for(0x01, 3, false);
    ASSERT_RGB(25, 0, 0, GATE_PX(t));
}

void test_the_gate_pixel_is_green_once_time_is_released(void) {
    chores_led_t t = chores_led_for(0x07, 3, true);
    ASSERT_RGB(0, 20, 0, GATE_PX(t));
}

void test_the_gate_is_red_on_the_tick_before_the_latch(void) {
    /* Every chore acked but `released` not yet latched. display.h says
       this window is real (chore_outstanding hits 0 one step before
       chore_released is set), and the gate pixel must report the LATCH,
       not the mask: the seconds are not granted until the latch is. */
    chores_led_t t = chores_led_for(0x07, 3, false);
    ASSERT_RGB(25, 0, 0, GATE_PX(t));
    ASSERT_RGB(0, 20, 0, CHORE_PX(t, 0));
}

void test_the_gate_stays_green_when_a_chore_is_re_toggled_after_release(void) {
    /* THE reason the gate needs a pixel and an argument of its own. An ack
       is a toggle (chores.h: a mis-press has to be undoable with the same
       button), but release is a LATCHED day flag that un-acking does not
       revoke. So a red chore next to a green gate is a legal, reachable
       reading, and it cannot be derived from the mask. */
    chores_led_t t = chores_led_for(0x00, 3, true);
    ASSERT_RGB(0, 20, 0, GATE_PX(t));
    ASSERT_RGB(25, 0, 0, CHORE_PX(t, 0));
    ASSERT_RGB(25, 0, 0, CHORE_PX(t, 1));
    ASSERT_RGB(25, 0, 0, CHORE_PX(t, 2));
}

/* ---- the chore strip: rows that are not chores --------------------------- */

void test_a_spare_row_is_dark_not_red(void) {
    /* Two chores configured: the third ack button is not a chore button at
       all (button_actions.h refuses it), so its pixel must not claim an
       outstanding chore that does not exist. */
    chores_led_t t = chores_led_for(0x00, 2, false);
    ASSERT_RGB(25, 0, 0, CHORE_PX(t, 0));
    ASSERT_RGB(25, 0, 0, CHORE_PX(t, 1));
    ASSERT_RGB(0, 0, 0, CHORE_PX(t, 2));
}

void test_a_stale_high_bit_cannot_light_a_spare_row(void) {
    /* The list shrank from three chores to two with bit 2 still set in the
       stored mask. chores.h calls bits at or above the count meaningless;
       a green pixel under a row that is not on the screen would be a
       claim about a chore the kid cannot see. */
    chores_led_t t = chores_led_for(0x07, 2, false);
    ASSERT_RGB(0, 20, 0, CHORE_PX(t, 0));
    ASSERT_RGB(0, 20, 0, CHORE_PX(t, 1));
    ASSERT_RGB(0, 0, 0, CHORE_PX(t, 2));
}

void test_no_chores_configured_leaves_the_strip_dark(void) {
    /* C1: with no chores the feature is inert. Nothing is withheld, so a
       red gate would be a lie about locked time and a green one a lie
       about earned time. */
    chores_led_t t = chores_led_for(0x00, 0, false);
    for (int p = 0; p < NEOPIXEL_COUNT; p++) {
        ASSERT_RGB(0, 0, 0, t.px[p]);
    }
}

void test_no_chores_configured_keeps_the_gate_dark_even_when_released(void) {
    /* `released` can be a latch left over from yesterday's list. With no
       list today there is no gate to be green about. */
    chores_led_t t = chores_led_for(0x07, 0, true);
    for (int p = 0; p < NEOPIXEL_COUNT; p++) {
        ASSERT_RGB(0, 0, 0, t.px[p]);
    }
}

void test_a_count_above_the_maximum_is_clamped(void) {
    /* n comes from NVS. Only CHORE_MAX rows exist and only
       NEOPIXEL_COUNT pixels do; a count of 200 must land inside both.
       ASan is the real assertion here — an out-of-bounds write fails the
       suite rather than returning something plausible. */
    chores_led_t t = chores_led_for(0x05, 200, false);
    ASSERT_RGB(0, 20, 0, CHORE_PX(t, 0));
    ASSERT_RGB(25, 0, 0, CHORE_PX(t, 1));
    ASSERT_RGB(0, 20, 0, CHORE_PX(t, 2));
}

void test_the_chore_table_is_pure(void) {
    /* No clock, no NVS, no globals: the same arguments twice give the same
       four pixels, and nothing was painted on the way. T8 depends on this
       — it calls the table for a pre-press frame whose mask is NOT the
       live one. */
    chores_led_t a = chores_led_for(0x03, 3, false);
    chores_led_t b = chores_led_for(0x03, 3, false);
    for (int p = 0; p < NEOPIXEL_COUNT; p++) {
        ASSERT_RGB(a.px[p].r, a.px[p].g, a.px[p].b, b.px[p]);
    }
    TEST_ASSERT_EQUAL_INT(0, stub_pixel_calls);
    TEST_ASSERT_EQUAL_INT(0, stub_state_reads);
}

/* ---- the chore painter -------------------------------------------------- */

void test_the_painter_writes_every_pixel_exactly_once(void) {
    /* The chore display owns the WHOLE strip — there is no pixel left over
       for the timer state or for net_window's WiFi pixel — so every index
       has to be written. Skipping one leaves the previous owner's colour
       standing under a chore name. */
    chores_led_show(0x01, 3, false);
    TEST_ASSERT_EQUAL_INT(NEOPIXEL_COUNT, stub_pixel_calls);
    bool seen[NEOPIXEL_COUNT] = {false};
    for (int i = 0; i < NEOPIXEL_COUNT; i++) {
        TEST_ASSERT_GREATER_OR_EQUAL_INT(0, stub_seen_idx[i]);
        TEST_ASSERT_LESS_THAN_INT(NEOPIXEL_COUNT, stub_seen_idx[i]);
        TEST_ASSERT_FALSE_MESSAGE(seen[stub_seen_idx[i]], "a pixel was written twice");
        seen[stub_seen_idx[i]] = true;
    }
}

void test_the_painter_takes_over_the_timer_state_and_wifi_pixels(void) {
    /* Named explicitly because these two pixels have other owners in every
       other mode: NP_STATE_PIXEL here, and pixel 3 inside net_window.c
       under CONFIG_MAGTAG_SYNC_LED_FEEDBACK. Chore mode needs 3 + 1 = the
       whole strip, so it displaces both, and that is a behaviour worth a
       test rather than a comment. */
    chores_led_show(0x00, 3, false);
    bool state_pixel_written = false, wifi_pixel_written = false;
    for (int i = 0; i < stub_pixel_calls && i < MAX_RECORDED; i++) {
        state_pixel_written |= (stub_seen_idx[i] == NP_STATE_PIXEL);
        wifi_pixel_written |= (stub_seen_idx[i] == NEOPIXEL_COUNT - 1);
    }
    TEST_ASSERT_TRUE(state_pixel_written);
    TEST_ASSERT_TRUE(wifi_pixel_written);
}

void test_the_painter_paints_what_the_table_says(void) {
    chores_led_t t = chores_led_for(0x02, 3, true);
    chores_led_show(0x02, 3, true);
    TEST_ASSERT_EQUAL_INT(NEOPIXEL_COUNT, stub_pixel_calls);
    for (int i = 0; i < NEOPIXEL_COUNT; i++) {
        int px = stub_seen_idx[i];
        TEST_ASSERT_EQUAL_UINT8(t.px[px].r, stub_seen_r[i]);
        TEST_ASSERT_EQUAL_UINT8(t.px[px].g, stub_seen_g[i]);
        TEST_ASSERT_EQUAL_UINT8(t.px[px].b, stub_seen_b[i]);
    }
}

void test_the_painter_writes_a_spare_row_dark_rather_than_skipping_it(void) {
    /* Two chores: the third row's pixel must be actively driven to black.
       Leaving it unwritten is the same bug as skipping it — it would keep
       the amber or blue whatever painted it last. */
    chores_led_show(0x00, 2, false);
    TEST_ASSERT_EQUAL_INT(NEOPIXEL_COUNT, stub_pixel_calls);
    int spare = k_chore_pixel[2];
    bool found = false;
    for (int i = 0; i < NEOPIXEL_COUNT; i++) {
        if (stub_seen_idx[i] != spare)
            continue;
        found = true;
        TEST_ASSERT_EQUAL_UINT8(0, stub_seen_r[i]);
        TEST_ASSERT_EQUAL_UINT8(0, stub_seen_g[i]);
        TEST_ASSERT_EQUAL_UINT8(0, stub_seen_b[i]);
    }
    TEST_ASSERT_TRUE_MESSAGE(found, "the spare row's pixel was never written");
}

void test_the_painter_reads_no_timer_state(void) {
    /* It paints the mask it is handed and nothing else. T8's first frame
       is the PRE-press state, which by definition disagrees with whatever
       the live state has already become; a painter that reached for a
       global would paint the post-press frame twice and the acknowledging
       transition would never be seen. */
    stub_state = TIMER_RUNNING;
    chores_led_show(0x07, 3, true);
    TEST_ASSERT_EQUAL_INT(0, stub_state_reads);
}

/* Quiet hours would otherwise remove the ONLY acknowledgement a press gets.
   Design §2.5 stopped making the panel the ack channel on purpose, so
   during the mute a status-class strip means a child presses a button and
   nothing happens anywhere for ~1.9 s. The user's decision was to let acks
   through the mute, and HIGHPRI is how neopixel.c spells that — so the
   class is the behaviour and belongs in a test, not in a comment. */
void test_the_chore_strip_is_highpri_so_quiet_hours_cannot_silence_an_ack(void) {
    chores_led_show(0x01, 3, false);
    TEST_ASSERT_EQUAL_INT_MESSAGE(NEOPIXEL_COUNT, stub_hi_calls,
                                  "the chore strip is status class: quiet hours silence every ack");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, stub_status_calls, "part of the chore strip is still status class");
}

/* And nothing else came with it. The promotion is the CHECKLIST's, not the
   strip's: the timer state pixel is ambient — it says "the device is awake
   and counting" — and is exactly what quiet hours are for. net_window.c's
   sync pixel is the same kind of thing and is left alone in that file. */
void test_the_timer_state_pixel_stays_status_class(void) {
    stub_state = TIMER_RUNNING;
    status_led_show_timer_state();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, stub_status_calls, "the timer state pixel is no longer status class");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, stub_hi_calls, "the timer state pixel was promoted past quiet hours");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_running_is_green);
    RUN_TEST(test_paused_is_amber);
    RUN_TEST(test_expired_is_red);
    RUN_TEST(test_break_is_blue_cyan);
    RUN_TEST(test_idle_falls_through_to_white);
    RUN_TEST(test_unknown_state_falls_through_to_white);
    RUN_TEST(test_table_is_total);
    RUN_TEST(test_wrapper_drives_the_state_pixel_from_the_timer);
    RUN_TEST(test_wrapper_reads_the_timer_exactly_once);
    RUN_TEST(test_wrapper_writes_pixel_zero);
    RUN_TEST(test_the_mapping_covers_every_pixel_exactly_once);
    RUN_TEST(test_the_mapping_is_button_order_until_hardware_says_otherwise);
    RUN_TEST(test_an_outstanding_chore_is_red);
    RUN_TEST(test_an_acked_chore_is_green);
    RUN_TEST(test_each_chore_lands_on_its_own_pixel);
    RUN_TEST(test_the_chore_colours_are_the_timer_traffic_light);
    RUN_TEST(test_all_done_and_released_is_the_whole_strip_green);
    RUN_TEST(test_the_gate_pixel_is_red_while_time_is_locked);
    RUN_TEST(test_the_gate_pixel_is_green_once_time_is_released);
    RUN_TEST(test_the_gate_is_red_on_the_tick_before_the_latch);
    RUN_TEST(test_the_gate_stays_green_when_a_chore_is_re_toggled_after_release);
    RUN_TEST(test_a_spare_row_is_dark_not_red);
    RUN_TEST(test_a_stale_high_bit_cannot_light_a_spare_row);
    RUN_TEST(test_no_chores_configured_leaves_the_strip_dark);
    RUN_TEST(test_no_chores_configured_keeps_the_gate_dark_even_when_released);
    RUN_TEST(test_a_count_above_the_maximum_is_clamped);
    RUN_TEST(test_the_chore_table_is_pure);
    RUN_TEST(test_the_painter_writes_every_pixel_exactly_once);
    RUN_TEST(test_the_painter_takes_over_the_timer_state_and_wifi_pixels);
    RUN_TEST(test_the_painter_paints_what_the_table_says);
    RUN_TEST(test_the_painter_writes_a_spare_row_dark_rather_than_skipping_it);
    RUN_TEST(test_the_painter_reads_no_timer_state);
    RUN_TEST(test_the_chore_strip_is_highpri_so_quiet_hours_cannot_silence_an_ack);
    RUN_TEST(test_the_timer_state_pixel_stays_status_class);
    return UNITY_END();
}
