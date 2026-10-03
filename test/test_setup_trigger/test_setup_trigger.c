#include <stddef.h>
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

void test_no_ssid_timer_wake_with_hold_completed_still_enters_setup(void) {
    /* Finding 5 (flipped): a completed BOOT hold outranks the no-SSID
       automatic rule, same as it outranks the SSID-present rule below.
       This input vector used to be asserted NORMAL, which made the
       explicit gesture do nothing on exactly the device that needs setup
       most -- the one with no SSID at all. */
    setup_trigger_in_t in = {
        .has_wifi_ssid = false, .button_wake = false, .cold_boot = false, .boot_hold_completed = true};
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_MODE_SETUP, setup_trigger_decide(&in));
}

void test_no_ssid_cold_boot_and_button_wake_together_still_setup(void) {
    /* Should not occur on real hardware (a true cold boot reports no EXT1
       cause), but the decision must not special-case the combination. */
    setup_trigger_in_t in = {
        .has_wifi_ssid = false, .button_wake = true, .cold_boot = true, .boot_hold_completed = false};
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_MODE_SETUP, setup_trigger_decide(&in));
}

/* ---- the hold-versus-automatic-rule rows -------------------------------- */

void test_no_ssid_button_wake_with_hold_completed_still_setup(void) {
    setup_trigger_in_t in = {
        .has_wifi_ssid = false, .button_wake = true, .cold_boot = false, .boot_hold_completed = true};
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_MODE_SETUP, setup_trigger_decide(&in));
}

void test_no_ssid_cold_boot_with_hold_completed_still_setup(void) {
    setup_trigger_in_t in = {
        .has_wifi_ssid = false, .button_wake = false, .cold_boot = true, .boot_hold_completed = true};
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_MODE_SETUP, setup_trigger_decide(&in));
}

/* ---- setup_trigger_decide: SSID present ---------------------------------- */

void test_ssid_present_boot_hold_completed_enters_setup(void) {
    setup_trigger_in_t in = {
        .has_wifi_ssid = true, .button_wake = true, .cold_boot = false, .boot_hold_completed = true};
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_MODE_SETUP, setup_trigger_decide(&in));
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

void test_ssid_present_plain_timer_wake_no_hold_stays_normal(void) {
    /* Finding 8: the commonest wake in the fleet -- a provisioned device
       waking for its own periodic tick, no button, no hold -- had no row
       in this table at all. */
    setup_trigger_in_t in = {
        .has_wifi_ssid = true, .button_wake = false, .cold_boot = false, .boot_hold_completed = false};
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_MODE_NORMAL, setup_trigger_decide(&in));
}

void test_ssid_present_cold_boot_with_hold_completed_enters_setup(void) {
    /* A provisioned device whose hold completed after a cold boot gets
       the hold's answer, not the cold-boot rule's. The hold is timed
       after boot: BOOT held through the reset itself enters ROM download
       mode (GPIO0 is a strapping pin), so this firmware never sees it. */
    setup_trigger_in_t in = {
        .has_wifi_ssid = true, .button_wake = false, .cold_boot = true, .boot_hold_completed = true};
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_MODE_SETUP, setup_trigger_decide(&in));
}

/* ---- the full sixteen-row truth table, data-driven --------------------

   A literal table rather than a re-derivation of decide()'s branches: a
   mutant that changes the implementation's structure but keeps its
   answers is exactly what this is for, and a test that recomputed the
   same if/else chain would not catch it. Reasoned independently from the
   plan's rules (docs/planning/20261003.wifi-provisioning.plan.md,
   "Entering setup mode" and task 2): a completed hold always wins; else
   no SSID means cold-boot-or-button-wake; else (SSID present, no hold)
   is always normal. */
typedef struct {
    bool has_wifi_ssid;
    bool button_wake;
    bool cold_boot;
    bool boot_hold_completed;
    setup_trigger_mode_t expected;
} decide_row_t;

static const decide_row_t DECIDE_TABLE[] = {
    /* ssid,  button, cold,  hold   expected */
    {false, false, false, false, SETUP_TRIGGER_MODE_NORMAL}, {false, false, false, true, SETUP_TRIGGER_MODE_SETUP},
    {false, false, true, false, SETUP_TRIGGER_MODE_SETUP},   {false, false, true, true, SETUP_TRIGGER_MODE_SETUP},
    {false, true, false, false, SETUP_TRIGGER_MODE_SETUP},   {false, true, false, true, SETUP_TRIGGER_MODE_SETUP},
    {false, true, true, false, SETUP_TRIGGER_MODE_SETUP},    {false, true, true, true, SETUP_TRIGGER_MODE_SETUP},
    {true, false, false, false, SETUP_TRIGGER_MODE_NORMAL},  {true, false, false, true, SETUP_TRIGGER_MODE_SETUP},
    {true, false, true, false, SETUP_TRIGGER_MODE_NORMAL},   {true, false, true, true, SETUP_TRIGGER_MODE_SETUP},
    {true, true, false, false, SETUP_TRIGGER_MODE_NORMAL},   {true, true, false, true, SETUP_TRIGGER_MODE_SETUP},
    {true, true, true, false, SETUP_TRIGGER_MODE_NORMAL},    {true, true, true, true, SETUP_TRIGGER_MODE_SETUP},
};

void test_decide_covers_all_sixteen_inputs(void) {
    for (size_t i = 0; i < sizeof(DECIDE_TABLE) / sizeof(DECIDE_TABLE[0]); i++) {
        const decide_row_t *r = &DECIDE_TABLE[i];
        setup_trigger_in_t in = {.has_wifi_ssid = r->has_wifi_ssid,
                                 .button_wake = r->button_wake,
                                 .cold_boot = r->cold_boot,
                                 .boot_hold_completed = r->boot_hold_completed};
        TEST_ASSERT_EQUAL_MESSAGE(r->expected, setup_trigger_decide(&in), "decide() truth table mismatch");
    }
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

void test_hold_backwards_timestamp_does_not_spuriously_arm(void) {
    /* A backward jump that is NOT a 32-bit wrap (two clocks mixed, or a
       non-monotonic source fed in by mistake) must not read as a huge
       elapsed time and arm instantly -- unlike the wraparound case above,
       this gap (40000 ms) is nowhere near int32_t's range, which is what
       the clamp uses to tell the two apart. Clamped to zero elapsed, the
       hold just stays below threshold; a later forward sample still
       arms it normally. */
    setup_trigger_boot_hold_t t;
    setup_trigger_boot_hold_reset(&t);
    setup_trigger_boot_hold_sample(&t, 50000, true); /* HOLDING, start=50000 */
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_HOLDING, t.state);
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_EVENT_NONE, setup_trigger_boot_hold_sample(&t, 10000, true));
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_HOLDING, t.state); /* not armed */
}

void test_hold_first_sample_already_down_then_released_is_cancelled(void) {
    /* The tracker's very first sample for this press already reads DOWN
       (the common case: the caller starts polling once BOOT is noticed
       held, not before). A quick release well under threshold is still
       CANCELLED, the same as a release that was preceded by IDLE
       samples -- IDLE is not required to reach HOLDING. */
    setup_trigger_boot_hold_t t;
    setup_trigger_boot_hold_reset(&t);
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_EVENT_NONE, setup_trigger_boot_hold_sample(&t, 1000, true));
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_HOLDING, t.state);
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_EVENT_CANCELLED, setup_trigger_boot_hold_sample(&t, 1200, false));
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_IDLE, t.state);
}

void test_hold_rearms_after_a_completed_enter_setup(void) {
    /* A completed hold must not leave the tracker unable to arm again:
       the next BOOT hold is a fresh gesture, timed from its own start,
       not from whatever press_start_ms the first one left behind. */
    setup_trigger_boot_hold_t t;
    setup_trigger_boot_hold_reset(&t);
    setup_trigger_boot_hold_sample(&t, 0, true);
    setup_trigger_boot_hold_sample(&t, SETUP_TRIGGER_BOOT_HOLD_MS, true); /* -> ARMED */
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_EVENT_ENTER_SETUP,
                      setup_trigger_boot_hold_sample(&t, SETUP_TRIGGER_BOOT_HOLD_MS + 50, false));
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_IDLE, t.state);

    uint32_t start2 = SETUP_TRIGGER_BOOT_HOLD_MS + 1000;
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_EVENT_NONE, setup_trigger_boot_hold_sample(&t, start2, true));
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_HOLDING, t.state);
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_EVENT_ARMED,
                      setup_trigger_boot_hold_sample(&t, start2 + SETUP_TRIGGER_BOOT_HOLD_MS, true));
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_ARMED, t.state);
}

void test_hold_thirty_seconds_stays_armed_and_enters_on_release(void) {
    /* A hold far longer than the threshold must not do anything other
       than stay ARMED (no repeat event, per
       test_hold_armed_does_not_refire_while_still_down above) and then
       enter setup on release, same as a hold released right at
       threshold. */
    setup_trigger_boot_hold_t t;
    setup_trigger_boot_hold_reset(&t);
    setup_trigger_boot_hold_sample(&t, 0, true);
    setup_trigger_boot_hold_sample(&t, SETUP_TRIGGER_BOOT_HOLD_MS, true); /* -> ARMED */
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_EVENT_NONE, setup_trigger_boot_hold_sample(&t, 30000, true));
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_ARMED, t.state);
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_EVENT_ENTER_SETUP, setup_trigger_boot_hold_sample(&t, 30050, false));
    TEST_ASSERT_EQUAL(SETUP_TRIGGER_BOOT_HOLD_IDLE, t.state);
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
    RUN_TEST(test_no_ssid_timer_wake_with_hold_completed_still_enters_setup);
    RUN_TEST(test_no_ssid_cold_boot_and_button_wake_together_still_setup);
    RUN_TEST(test_no_ssid_button_wake_with_hold_completed_still_setup);
    RUN_TEST(test_no_ssid_cold_boot_with_hold_completed_still_setup);
    RUN_TEST(test_ssid_present_boot_hold_completed_enters_setup);
    RUN_TEST(test_ssid_present_ordinary_button_press_never_triggers_setup);
    RUN_TEST(test_ssid_present_cold_boot_alone_stays_normal);
    RUN_TEST(test_ssid_present_timer_wake_with_hold_completed_enters_setup);
    RUN_TEST(test_ssid_present_plain_timer_wake_no_hold_stays_normal);
    RUN_TEST(test_ssid_present_cold_boot_with_hold_completed_enters_setup);
    RUN_TEST(test_decide_covers_all_sixteen_inputs);
    RUN_TEST(test_hold_never_pressed_stays_idle);
    RUN_TEST(test_hold_release_just_before_threshold_cancels);
    RUN_TEST(test_hold_exactly_at_threshold_arms);
    RUN_TEST(test_hold_armed_then_released_enters_setup);
    RUN_TEST(test_hold_armed_does_not_refire_while_still_down);
    RUN_TEST(test_hold_sub_threshold_samples_stay_holding);
    RUN_TEST(test_hold_second_attempt_restarts_the_clock);
    RUN_TEST(test_hold_timestamp_wraparound_still_arms_at_the_threshold);
    RUN_TEST(test_hold_backwards_timestamp_does_not_spuriously_arm);
    RUN_TEST(test_hold_first_sample_already_down_then_released_is_cancelled);
    RUN_TEST(test_hold_rearms_after_a_completed_enter_setup);
    RUN_TEST(test_hold_thirty_seconds_stays_armed_and_enters_on_release);
    RUN_TEST(test_wifi_fail_hint_false_below_threshold);
    RUN_TEST(test_wifi_fail_hint_true_at_and_above_threshold);
    return UNITY_END();
}
