#include <unity.h>

/* Single-TU compilation */
#include "../../main/wake_policy.c"

void setUp(void) {}
void tearDown(void) {}

/* ---- render decision ---- */

void test_render_expiry_transition_fires_alert(void) {
    TEST_ASSERT_EQUAL(WAKE_RENDER_EXPIRY_ALERT, wake_policy_render(TIMER_RUNNING, TIMER_EXPIRED, false));
    TEST_ASSERT_EQUAL(WAKE_RENDER_EXPIRY_ALERT, wake_policy_render(TIMER_RUNNING, TIMER_EXPIRED, true));
}

void test_render_already_expired_never_refires_alert(void) {
    /* Button press on an expired timer (incl. swap landing on one):
       return to the main layout, no second alarm. */
    TEST_ASSERT_EQUAL(WAKE_RENDER_FULL, wake_policy_render(TIMER_EXPIRED, TIMER_EXPIRED, true));
    TEST_ASSERT_EQUAL(WAKE_RENDER_PARTIAL, wake_policy_render(TIMER_EXPIRED, TIMER_EXPIRED, false));
}

void test_render_state_change_promotes_to_full(void) {
    TEST_ASSERT_EQUAL(WAKE_RENDER_FULL, wake_policy_render(TIMER_BREAK, TIMER_PAUSED, false));
    TEST_ASSERT_EQUAL(WAKE_RENDER_FULL, wake_policy_render(TIMER_IDLE, TIMER_RUNNING, true));
}

void test_render_button_wake_always_full(void) {
    TEST_ASSERT_EQUAL(WAKE_RENDER_FULL, wake_policy_render(TIMER_RUNNING, TIMER_RUNNING, true));
}

void test_render_steady_timer_tick_is_partial(void) {
    TEST_ASSERT_EQUAL(WAKE_RENDER_PARTIAL, wake_policy_render(TIMER_RUNNING, TIMER_RUNNING, false));
    TEST_ASSERT_EQUAL(WAKE_RENDER_PARTIAL, wake_policy_render(TIMER_IDLE, TIMER_IDLE, false));
}

/* ---- round-minute snap ---- */

#define WATCH 75 /* mirrors SLEEP_PLAN_WATCH_SEC */

void test_snap_absorbs_positive_jitter(void) {
    TEST_ASSERT_EQUAL_INT32(3660, wake_policy_snap_minute(3661, WATCH));
    TEST_ASSERT_EQUAL_INT32(3660, wake_policy_snap_minute(3662, WATCH));
}

void test_snap_absorbs_negative_jitter(void) {
    TEST_ASSERT_EQUAL_INT32(3660, wake_policy_snap_minute(3659, WATCH));
    TEST_ASSERT_EQUAL_INT32(3660, wake_policy_snap_minute(3658, WATCH));
}

void test_snap_leaves_honest_offgrid_values(void) {
    TEST_ASSERT_EQUAL_INT32(3630, wake_policy_snap_minute(3630, WATCH));
    TEST_ASSERT_EQUAL_INT32(3657, wake_policy_snap_minute(3657, WATCH));
    TEST_ASSERT_EQUAL_INT32(3663, wake_policy_snap_minute(3663, WATCH));
}

void test_snap_disabled_inside_watch_window(void) {
    /* The event watch renders exact values — never snap at/below it */
    TEST_ASSERT_EQUAL_INT32(61, wake_policy_snap_minute(61, WATCH));
    TEST_ASSERT_EQUAL_INT32(WATCH, wake_policy_snap_minute(WATCH, WATCH));
}

void test_snap_ignores_non_positive(void) {
    TEST_ASSERT_EQUAL_INT32(0, wake_policy_snap_minute(0, WATCH));
    TEST_ASSERT_EQUAL_INT32(-5, wake_policy_snap_minute(-5, WATCH));
}

/* ---- NTP cadence ---- */

#define IDLE_IVL 3600

void test_sync_running_follows_recheck_flag(void) {
    TEST_ASSERT_TRUE(wake_policy_sync_due(TIMER_RUNNING, true, 1000, 900, IDLE_IVL));
    TEST_ASSERT_FALSE(wake_policy_sync_due(TIMER_RUNNING, false, 1000, 0, IDLE_IVL));
}

void test_sync_break_never_syncs(void) {
    TEST_ASSERT_FALSE(wake_policy_sync_due(TIMER_BREAK, true, 1000, 0, IDLE_IVL));
}

void test_sync_clock_states_use_idle_cadence(void) {
    time_t last = 10000;
    TEST_ASSERT_FALSE(wake_policy_sync_due(TIMER_IDLE, false, last + IDLE_IVL - 1, last, IDLE_IVL));
    TEST_ASSERT_TRUE(wake_policy_sync_due(TIMER_IDLE, false, last + IDLE_IVL, last, IDLE_IVL));
    TEST_ASSERT_TRUE(wake_policy_sync_due(TIMER_PAUSED, false, last + IDLE_IVL, last, IDLE_IVL));
    TEST_ASSERT_TRUE(wake_policy_sync_due(TIMER_EXPIRED, false, last + IDLE_IVL, last, IDLE_IVL));
}

void test_sync_never_synced_is_always_due_in_clock_states(void) {
    TEST_ASSERT_TRUE(wake_policy_sync_due(TIMER_IDLE, false, 42, 0, IDLE_IVL));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_render_expiry_transition_fires_alert);
    RUN_TEST(test_render_already_expired_never_refires_alert);
    RUN_TEST(test_render_state_change_promotes_to_full);
    RUN_TEST(test_render_button_wake_always_full);
    RUN_TEST(test_render_steady_timer_tick_is_partial);
    RUN_TEST(test_snap_absorbs_positive_jitter);
    RUN_TEST(test_snap_absorbs_negative_jitter);
    RUN_TEST(test_snap_leaves_honest_offgrid_values);
    RUN_TEST(test_snap_disabled_inside_watch_window);
    RUN_TEST(test_snap_ignores_non_positive);
    RUN_TEST(test_sync_running_follows_recheck_flag);
    RUN_TEST(test_sync_break_never_syncs);
    RUN_TEST(test_sync_clock_states_use_idle_cadence);
    RUN_TEST(test_sync_never_synced_is_always_due_in_clock_states);
    return UNITY_END();
}
