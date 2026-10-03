#include <stdint.h>
#include <unity.h>

/* Single-TU compilation */
#include "../../main/setup_trigger.c"

void setUp(void) {}
void tearDown(void) {}

/* ---- setup_trigger_decide: no SSID ------------------------------------- */

void test_no_ssid_cold_boot_enters_setup(void) {
    setup_trigger_in_t in = {
        .has_wifi_ssid = false, .button_wake = false, .cold_boot = true, .boot_hold_completed = false};
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_MODE_SETUP, setup_trigger_decide(&in));
}

void test_no_ssid_button_wake_enters_setup(void) {
    /* The nvs_flash_erase() recovery path (plan task 2 notes): a wiped
       device has no SSID and wakes on a button press, not a cold boot. */
    setup_trigger_in_t in = {
        .has_wifi_ssid = false, .button_wake = true, .cold_boot = false, .boot_hold_completed = false};
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_MODE_SETUP, setup_trigger_decide(&in));
}

void test_no_ssid_timer_wake_stays_normal(void) {
    /* Timers must keep running offline; the no-SSID network path already
       fails cleanly (wifi_session.c:75). */
    setup_trigger_in_t in = {
        .has_wifi_ssid = false, .button_wake = false, .cold_boot = false, .boot_hold_completed = false};
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_MODE_NORMAL, setup_trigger_decide(&in));
}

void test_no_ssid_timer_wake_stays_normal_even_if_hold_completed(void) {
    /* The plan's "no SSID on a timer wake -> normal" rule has no carve-out
       for a simultaneous BOOT hold: only cold boot and button wakes
       recover into setup when there is no SSID. */
    setup_trigger_in_t in = {
        .has_wifi_ssid = false, .button_wake = false, .cold_boot = false, .boot_hold_completed = true};
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_MODE_NORMAL, setup_trigger_decide(&in));
}

void test_no_ssid_cold_boot_and_button_wake_together_still_setup(void) {
    /* Should not occur on real hardware (a true cold boot reports no EXT1
       cause), but the decision must not special-case the combination. */
    setup_trigger_in_t in = {
        .has_wifi_ssid = false, .button_wake = true, .cold_boot = true, .boot_hold_completed = false};
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_MODE_SETUP, setup_trigger_decide(&in));
}

/* ---- setup_trigger_decide: SSID present ---------------------------------- */

void test_ssid_present_boot_hold_completed_enters_setup(void) {
    setup_trigger_in_t in = {
        .has_wifi_ssid = true, .button_wake = true, .cold_boot = false, .boot_hold_completed = true};
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_MODE_SETUP, setup_trigger_decide(&in));
}

void test_ssid_present_without_hold_stays_normal(void) {
    setup_trigger_in_t in = {
        .has_wifi_ssid = true, .button_wake = true, .cold_boot = false, .boot_hold_completed = false};
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_MODE_NORMAL, setup_trigger_decide(&in));
}

void test_ssid_present_ordinary_button_press_never_triggers_setup(void) {
    /* An ordinary Start/Pause press (B) must not be mistaken for the
       gesture just because button_wake is true. */
    setup_trigger_in_t in = {
        .has_wifi_ssid = true, .button_wake = true, .cold_boot = false, .boot_hold_completed = false};
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_MODE_NORMAL, setup_trigger_decide(&in));
}

void test_ssid_present_cold_boot_alone_stays_normal(void) {
    /* A provisioned device power-cycling must not fall into setup; only
       the hold matters once there is an SSID. */
    setup_trigger_in_t in = {
        .has_wifi_ssid = true, .button_wake = false, .cold_boot = true, .boot_hold_completed = false};
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_MODE_NORMAL, setup_trigger_decide(&in));
}

void test_ssid_present_timer_wake_with_hold_completed_enters_setup(void) {
    /* boot_hold_completed is the sole gate once there is an SSID, whatever
       the wake cause. */
    setup_trigger_in_t in = {
        .has_wifi_ssid = true, .button_wake = false, .cold_boot = false, .boot_hold_completed = true};
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_MODE_SETUP, setup_trigger_decide(&in));
}

/* ---- BOOT hold tracker --------------------------------------------------- */

void test_hold_never_pressed_stays_idle(void) {
    setup_trigger_boot_hold_t t;
    setup_trigger_boot_hold_reset(&t);
    for (uint32_t ms = 0; ms < 10000; ms += 1000) {
        TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_EVENT_NONE, setup_trigger_boot_hold_sample(&t, ms, false));
        TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_IDLE, t.state);
    }
}

void test_hold_release_just_before_threshold_cancels(void) {
    setup_trigger_boot_hold_t t;
    setup_trigger_boot_hold_reset(&t);
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_EVENT_NONE, setup_trigger_boot_hold_sample(&t, 0, true));
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_HOLDING, t.state);
    /* One ms short of the threshold: still holding, no event. */
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_EVENT_NONE,
                      setup_trigger_boot_hold_sample(&t, SETUP_TRIGGER_BOOT_HOLD_MS - 1, true));
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_HOLDING, t.state);
    /* Released at that same instant: cancelled, not armed. */
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_EVENT_CANCELLED,
                      setup_trigger_boot_hold_sample(&t, SETUP_TRIGGER_BOOT_HOLD_MS - 1, false));
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_IDLE, t.state);
}

void test_hold_exactly_at_threshold_arms(void) {
    setup_trigger_boot_hold_t t;
    setup_trigger_boot_hold_reset(&t);
    setup_trigger_boot_hold_sample(&t, 0, true);
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_EVENT_ARMED,
                      setup_trigger_boot_hold_sample(&t, SETUP_TRIGGER_BOOT_HOLD_MS, true));
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_ARMED, t.state);
}

void test_hold_armed_then_released_enters_setup(void) {
    setup_trigger_boot_hold_t t;
    setup_trigger_boot_hold_reset(&t);
    setup_trigger_boot_hold_sample(&t, 0, true);
    setup_trigger_boot_hold_sample(&t, SETUP_TRIGGER_BOOT_HOLD_MS, true); /* -> ARMED */
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_EVENT_ENTER_SETUP,
                      setup_trigger_boot_hold_sample(&t, SETUP_TRIGGER_BOOT_HOLD_MS + 50, false));
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_IDLE, t.state);
}

void test_hold_armed_does_not_refire_while_still_down(void) {
    setup_trigger_boot_hold_t t;
    setup_trigger_boot_hold_reset(&t);
    setup_trigger_boot_hold_sample(&t, 0, true);
    setup_trigger_boot_hold_sample(&t, SETUP_TRIGGER_BOOT_HOLD_MS, true); /* -> ARMED */
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_EVENT_NONE,
                      setup_trigger_boot_hold_sample(&t, SETUP_TRIGGER_BOOT_HOLD_MS + 500, true));
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_ARMED, t.state);
}

void test_hold_sub_threshold_samples_stay_holding(void) {
    setup_trigger_boot_hold_t t;
    setup_trigger_boot_hold_reset(&t);
    setup_trigger_boot_hold_sample(&t, 0, true);
    uint32_t quarter = SETUP_TRIGGER_BOOT_HOLD_MS / 4;
    for (int i = 1; i <= 3; i++) {
        TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_EVENT_NONE,
                          setup_trigger_boot_hold_sample(&t, quarter * (uint32_t)i, true));
        TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_HOLDING, t.state);
    }
}

void test_hold_second_attempt_restarts_the_clock(void) {
    /* A cancelled hold must not leave a stale press_start_ms that a later
       hold inherits — the second attempt is timed from ITS OWN start. */
    setup_trigger_boot_hold_t t;
    setup_trigger_boot_hold_reset(&t);
    setup_trigger_boot_hold_sample(&t, 0, true);
    setup_trigger_boot_hold_sample(&t, 100, false); /* cancel, well under threshold */
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_IDLE, t.state);

    setup_trigger_boot_hold_sample(&t, 200, true); /* second press, new start */
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_EVENT_NONE,
                      setup_trigger_boot_hold_sample(&t, 200 + SETUP_TRIGGER_BOOT_HOLD_MS - 1, true));
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_HOLDING, t.state);
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_EVENT_ARMED,
                      setup_trigger_boot_hold_sample(&t, 200 + SETUP_TRIGGER_BOOT_HOLD_MS, true));
}

void test_hold_timestamp_wraparound_still_arms_at_the_threshold(void) {
    /* press_start_ms sits 2000 ms before the uint32_t wrap; the threshold
       sample lands 3000 ms after it wraps back to a small value. Unsigned
       subtraction must still read this as exactly SETUP_TRIGGER_BOOT_HOLD_MS
       elapsed, not a huge or negative duration. */
    setup_trigger_boot_hold_t t;
    setup_trigger_boot_hold_reset(&t);
    uint32_t start = (uint32_t)0 - 2000; /* UINT32_MAX - 1999 */
    setup_trigger_boot_hold_sample(&t, start, true);
    uint32_t armed_at = start + SETUP_TRIGGER_BOOT_HOLD_MS; /* wraps */
    TEST_ASSERT_TRUE(armed_at < start);                     /* sanity: this really wrapped */
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_EVENT_ARMED, setup_trigger_boot_hold_sample(&t, armed_at, true));
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_ARMED, t.state);
}

/* ---- WiFi-failing hint --------------------------------------------------- */

void test_wifi_fail_hint_false_below_threshold(void) {
    TEST_ASSERT_FALSE(setup_trigger_wifi_failing_hint(0));
    TEST_ASSERT_FALSE(setup_trigger_wifi_failing_hint(SETUP_TRIGGER_WIFI_FAIL_HINT_THRESHOLD - 1));
}

void test_wifi_fail_hint_true_at_and_above_threshold(void) {
    TEST_ASSERT_TRUE(setup_trigger_wifi_failing_hint(SETUP_TRIGGER_WIFI_FAIL_HINT_THRESHOLD));
    TEST_ASSERT_TRUE(setup_trigger_wifi_failing_hint(SETUP_TRIGGER_WIFI_FAIL_HINT_THRESHOLD + 10));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_no_ssid_cold_boot_enters_setup);
    RUN_TEST(test_no_ssid_button_wake_enters_setup);
    RUN_TEST(test_no_ssid_timer_wake_stays_normal);
    RUN_TEST(test_no_ssid_timer_wake_stays_normal_even_if_hold_completed);
    RUN_TEST(test_no_ssid_cold_boot_and_button_wake_together_still_setup);
    RUN_TEST(test_ssid_present_boot_hold_completed_enters_setup);
    RUN_TEST(test_ssid_present_without_hold_stays_normal);
    RUN_TEST(test_ssid_present_ordinary_button_press_never_triggers_setup);
    RUN_TEST(test_ssid_present_cold_boot_alone_stays_normal);
    RUN_TEST(test_ssid_present_timer_wake_with_hold_completed_enters_setup);
    RUN_TEST(test_hold_never_pressed_stays_idle);
    RUN_TEST(test_hold_release_just_before_threshold_cancels);
    RUN_TEST(test_hold_exactly_at_threshold_arms);
    RUN_TEST(test_hold_armed_then_released_enters_setup);
    RUN_TEST(test_hold_armed_does_not_refire_while_still_down);
    RUN_TEST(test_hold_sub_threshold_samples_stay_holding);
    RUN_TEST(test_hold_second_attempt_restarts_the_clock);
    RUN_TEST(test_hold_timestamp_wraparound_still_arms_at_the_threshold);
    RUN_TEST(test_wifi_fail_hint_false_below_threshold);
    RUN_TEST(test_wifi_fail_hint_true_at_and_above_threshold);
    return UNITY_END();
}
