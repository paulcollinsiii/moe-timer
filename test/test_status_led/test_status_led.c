#include <unity.h>

/* Single-TU: the pure state->RGB table plus its one device wrapper, with
   the NeoPixel driver and the timer state machine resolved by the stubs
   below so the table can be asserted without hardware. */
#include "../../main/status_led.c"

/* ---- link-time stubs ---------------------------------------------------- */

static int stub_pixel_calls;
static int stub_pixel_idx;
static uint8_t stub_pixel_r, stub_pixel_g, stub_pixel_b;

void neopixel_status_pixel(int idx, uint8_t r, uint8_t g, uint8_t b) {
    stub_pixel_calls++;
    stub_pixel_idx = idx;
    stub_pixel_r = r;
    stub_pixel_g = g;
    stub_pixel_b = b;
}

static timer_state_t stub_state;
static int stub_state_reads;

timer_state_t timer_get_state(void) {
    stub_state_reads++;
    return stub_state;
}

void setUp(void) {
    stub_pixel_calls = 0;
    stub_pixel_idx = -1;
    stub_pixel_r = stub_pixel_g = stub_pixel_b = 0xFF;
    stub_state = TIMER_IDLE;
    stub_state_reads = 0;
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
       (private to net_window.c) are read side by side on the device, so
       the index is a fixed contract rather than whatever the constant
       happens to say. */
    stub_state = TIMER_BREAK;
    status_led_show_timer_state();
    TEST_ASSERT_EQUAL_INT(0, stub_pixel_idx);
    TEST_ASSERT_EQUAL_UINT8(0, stub_pixel_r);
    TEST_ASSERT_EQUAL_UINT8(10, stub_pixel_g);
    TEST_ASSERT_EQUAL_UINT8(25, stub_pixel_b);
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
    return UNITY_END();
}
