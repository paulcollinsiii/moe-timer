#include <unity.h>

/* Single-TU compilation */
#include "../../main/wake_policy.c"

void setUp(void) {}
void tearDown(void) {}

/* ---- render decision ---- */

void test_render_expiry_transition_fires_alert(void) {
    TEST_ASSERT_EQUAL(WAKE_RENDER_EXPIRY_ALERT, wake_policy_render(TIMER_RUNNING, TIMER_EXPIRED, false, false, false));
    TEST_ASSERT_EQUAL(WAKE_RENDER_EXPIRY_ALERT, wake_policy_render(TIMER_RUNNING, TIMER_EXPIRED, true, false, false));
}

void test_render_already_expired_never_refires_alert(void) {
    /* Button press on an expired timer (incl. swap landing on one):
       return to the main layout, no second alarm. */
    TEST_ASSERT_EQUAL(WAKE_RENDER_PARTIAL, wake_policy_render(TIMER_EXPIRED, TIMER_EXPIRED, true, false, false));
    TEST_ASSERT_EQUAL(WAKE_RENDER_PARTIAL, wake_policy_render(TIMER_EXPIRED, TIMER_EXPIRED, false, false, false));
}

void test_render_tick_state_change_promotes_to_full(void) {
    TEST_ASSERT_EQUAL(WAKE_RENDER_FULL, wake_policy_render(TIMER_BREAK, TIMER_PAUSED, false, false, false));
    TEST_ASSERT_EQUAL(WAKE_RENDER_FULL, wake_policy_render(TIMER_IDLE, TIMER_RUNNING, false, false, false));
}

void test_render_button_wake_is_partial_within_main_layout(void) {
    /* Same-layout button transitions ride the partial cadence (the
       every-Nth-full counter and the driver's prev-frame promotion
       still force fulls when needed). */
    TEST_ASSERT_EQUAL(WAKE_RENDER_PARTIAL, wake_policy_render(TIMER_RUNNING, TIMER_RUNNING, true, false, false));
    TEST_ASSERT_EQUAL(WAKE_RENDER_PARTIAL, wake_policy_render(TIMER_IDLE, TIMER_IDLE, true, false, false));
    TEST_ASSERT_EQUAL(WAKE_RENDER_PARTIAL, wake_policy_render(TIMER_IDLE, TIMER_RUNNING, true, false, false));
    TEST_ASSERT_EQUAL(WAKE_RENDER_PARTIAL, wake_policy_render(TIMER_RUNNING, TIMER_PAUSED, true, false, false));
    TEST_ASSERT_EQUAL(WAKE_RENDER_PARTIAL, wake_policy_render(TIMER_PAUSED, TIMER_RUNNING, true, false, false));
}

void test_render_button_wake_across_break_layout_is_full(void) {
    /* The break screen is a full-screen inversion of the main layout;
       a partial diff across that boundary would ghost the whole panel. */
    TEST_ASSERT_EQUAL(WAKE_RENDER_FULL, wake_policy_render(TIMER_RUNNING, TIMER_BREAK, true, false, false));
    TEST_ASSERT_EQUAL(WAKE_RENDER_FULL, wake_policy_render(TIMER_BREAK, TIMER_PAUSED, true, false, false));
}

void test_render_steady_timer_tick_is_partial(void) {
    TEST_ASSERT_EQUAL(WAKE_RENDER_PARTIAL, wake_policy_render(TIMER_RUNNING, TIMER_RUNNING, false, false, false));
    TEST_ASSERT_EQUAL(WAKE_RENDER_PARTIAL, wake_policy_render(TIMER_IDLE, TIMER_IDLE, false, false, false));
}

/* ---- break-over chime policy ----
   Extracted from main.c so the grace window is testable at all: the
   decision used to live in the least-testable file in the project. */

void test_break_chime_suppressed_while_an_extra_runs(void) {
    /* Rule 4, taken literally: the kid is mid-activity and will get that
       timer's own alert. */
    TEST_ASSERT_FALSE(wake_policy_break_chime(true, 0));
    TEST_ASSERT_FALSE(wake_policy_break_chime(true, 30));
}

void test_break_chime_fires_when_observed_promptly(void) {
    /* 0 s = the awake watch; 60 s = the worst-case normal tick cadence.
       Literals, not the constant, so the bound is actually pinned. */
    TEST_ASSERT_TRUE(wake_policy_break_chime(false, 0));
    TEST_ASSERT_TRUE(wake_policy_break_chime(false, 60));
    TEST_ASSERT_TRUE(wake_policy_break_chime(false, 74));
    TEST_ASSERT_TRUE(wake_policy_break_chime(false, 75)); /* inclusive bound */
}

void test_break_chime_suppressed_when_observed_late(void) {
    /* Rule 6: a charge lock (600 s naps), a bed-time lock (7200 s) or a
       power cycle spanned the end — the chime is an "it just happened"
       signal, not a replay. */
    TEST_ASSERT_FALSE(wake_policy_break_chime(false, 76));
    TEST_ASSERT_FALSE(wake_policy_break_chime(false, 600));
    TEST_ASSERT_FALSE(wake_policy_break_chime(false, 7200));
}

void test_break_chime_grace_is_the_watch_window(void) {
    /* The one place the derivation itself is pinned: the grace means
       "we were inside the watch window for it", and it must still clear
       the 60 s tick cadence with margin. */
    TEST_ASSERT_EQUAL_INT32(SLEEP_PLAN_WATCH_SEC, BREAK_CHIME_GRACE_SEC);
    TEST_ASSERT_TRUE(BREAK_CHIME_GRACE_SEC > 60);
}

void test_break_chime_ignores_negative_lateness(void) {
    /* Defensive: a clock step backwards between tick and drain must not
       read as "impossibly early" and suppress. */
    TEST_ASSERT_TRUE(wake_policy_break_chime(false, -5));
}

/* ---- selection_changed: Button C swapped the active slot ----
   `before` must stay the state that was actually PAINTED, or the
   break-screen boundary rule below has nothing to compare. Expiry
   suppression is therefore expressed separately: landing on a slot that
   was already EXPIRED is a selection change, not a transition. */

void test_render_selection_change_suppresses_the_expiry_alert(void) {
    /* Swapping onto an already-expired timer must not re-fire its alarm,
       whatever the previous slot was doing. */
    TEST_ASSERT_NOT_EQUAL(WAKE_RENDER_EXPIRY_ALERT, wake_policy_render(TIMER_PAUSED, TIMER_EXPIRED, true, false, true));
    TEST_ASSERT_NOT_EQUAL(WAKE_RENDER_EXPIRY_ALERT, wake_policy_render(TIMER_BREAK, TIMER_EXPIRED, true, false, true));
    /* Without a selection change the same pair IS a real expiry */
    TEST_ASSERT_EQUAL(WAKE_RENDER_EXPIRY_ALERT, wake_policy_render(TIMER_PAUSED, TIMER_EXPIRED, true, false, false));
}

void test_render_selection_change_across_the_break_screen_is_full(void) {
    /* Button C during a break is the feature's headline interaction, and
       both directions cross the full-screen inversion. A partial diff
       across it ghosts the whole panel; display_update only promotes
       every 5th, so four presses in five would ghost. */
    TEST_ASSERT_EQUAL(WAKE_RENDER_FULL, wake_policy_render(TIMER_BREAK, TIMER_IDLE, true, false, true));
    TEST_ASSERT_EQUAL(WAKE_RENDER_FULL, wake_policy_render(TIMER_IDLE, TIMER_BREAK, true, false, true));
    /* Onto an EXPIRED slot: alert suppressed AND still a full refresh */
    TEST_ASSERT_EQUAL(WAKE_RENDER_FULL, wake_policy_render(TIMER_BREAK, TIMER_EXPIRED, true, false, true));
    /* A swap that stays within the main layout keeps the partial cadence */
    TEST_ASSERT_EQUAL(WAKE_RENDER_PARTIAL, wake_policy_render(TIMER_PAUSED, TIMER_IDLE, true, false, true));
}

/* ---- break_ended: the wake that drops the BREAK chip ---- */

void test_render_break_ended_promotes_partial_to_full(void) {
    /* A break can now run behind another timer, so its end changes the
       panel without changing the ACTIVE slot's state: the inverted
       "BREAK m:ss" chip vanishes, and the chime case also snaps the
       selection back to Screen — a different timer's layout entirely.
       A partial diff across either would ghost. */
    TEST_ASSERT_EQUAL(WAKE_RENDER_FULL, wake_policy_render(TIMER_PAUSED, TIMER_PAUSED, false, true, false));
    TEST_ASSERT_EQUAL(WAKE_RENDER_FULL, wake_policy_render(TIMER_RUNNING, TIMER_RUNNING, false, true, false));
    TEST_ASSERT_EQUAL(WAKE_RENDER_FULL, wake_policy_render(TIMER_IDLE, TIMER_IDLE, true, true, false));
}

void test_render_break_ended_does_not_outrank_the_expiry_alert(void) {
    /* An extra timer expiring on the same wake still owns the alert —
       TIME'S UP is louder than a repaint. */
    TEST_ASSERT_EQUAL(WAKE_RENDER_EXPIRY_ALERT, wake_policy_render(TIMER_RUNNING, TIMER_EXPIRED, false, true, false));
    TEST_ASSERT_EQUAL(WAKE_RENDER_EXPIRY_ALERT, wake_policy_render(TIMER_RUNNING, TIMER_EXPIRED, true, true, false));
}

void test_render_without_break_ended_is_unchanged(void) {
    TEST_ASSERT_EQUAL(WAKE_RENDER_PARTIAL, wake_policy_render(TIMER_PAUSED, TIMER_PAUSED, false, false, false));
    TEST_ASSERT_EQUAL(WAKE_RENDER_PARTIAL, wake_policy_render(TIMER_IDLE, TIMER_IDLE, true, false, false));
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

/* ---- render-grid residue: how long to wait so the render lands on the
   state's minute grid (countdown grid for RUNNING/BREAK, wall grid
   otherwise); 0 = render where we are ---- */

void test_grid_wait_running_waits_countdown_residue(void) {
    /* 185 s to expiry: 5 s absorbs the residue, the render reads 3:00 */
    TEST_ASSERT_EQUAL_INT32(5, wake_policy_grid_wait_sec(TIMER_RUNNING, 185, 12, 25));
}

void test_grid_wait_running_on_grid_needs_no_wait(void) {
    TEST_ASSERT_EQUAL_INT32(0, wake_policy_grid_wait_sec(TIMER_RUNNING, 180, 12, 25));
}

void test_grid_wait_running_residue_beyond_max_renders_in_place(void) {
    /* 150 s to expiry: 30 s residue > 25 s cap — off-grid but honest */
    TEST_ASSERT_EQUAL_INT32(0, wake_policy_grid_wait_sec(TIMER_RUNNING, 150, 12, 25));
}

void test_grid_wait_expiry_already_passed_is_zero(void) {
    TEST_ASSERT_EQUAL_INT32(0, wake_policy_grid_wait_sec(TIMER_RUNNING, -30, 12, 25));
}

void test_grid_wait_break_uses_break_grid(void) {
    TEST_ASSERT_EQUAL_INT32(5, wake_policy_grid_wait_sec(TIMER_BREAK, 65, 40, 25));
}

void test_grid_wait_clock_states_use_wall_grid(void) {
    TEST_ASSERT_EQUAL_INT32(5, wake_policy_grid_wait_sec(TIMER_IDLE, 0, 55, 25));
    TEST_ASSERT_EQUAL_INT32(5, wake_policy_grid_wait_sec(TIMER_PAUSED, 0, 55, 25));
}

void test_grid_wait_on_wall_boundary_is_zero(void) {
    TEST_ASSERT_EQUAL_INT32(0, wake_policy_grid_wait_sec(TIMER_IDLE, 0, 0, 25));
}

void test_grid_wait_wall_residue_beyond_max_is_zero(void) {
    TEST_ASSERT_EQUAL_INT32(0, wake_policy_grid_wait_sec(TIMER_IDLE, 0, 30, 25));
}

/* ---- final-minute countdown steps (60/45/30/15 partial renders) ---- */

void test_countdown_step_values_are_quarter_minute(void) {
    TEST_ASSERT_EQUAL_INT32(60, wake_policy_countdown_step(0));
    TEST_ASSERT_EQUAL_INT32(45, wake_policy_countdown_step(1));
    TEST_ASSERT_EQUAL_INT32(30, wake_policy_countdown_step(2));
    TEST_ASSERT_EQUAL_INT32(15, wake_policy_countdown_step(3));
    TEST_ASSERT_EQUAL_INT32(0, wake_policy_countdown_step(4));
    TEST_ASSERT_EQUAL_INT32(0, wake_policy_countdown_step(-1));
}

void test_first_countdown_step_from_full_watch(void) {
    TEST_ASSERT_EQUAL_INT(0, wake_policy_first_countdown_step(65));
}

void test_first_countdown_step_skips_passed_marks_on_late_wake(void) {
    /* Woke late (slow sync): the 60 s mark already passed */
    TEST_ASSERT_EQUAL_INT(1, wake_policy_first_countdown_step(50));
    TEST_ASSERT_EQUAL_INT(3, wake_policy_first_countdown_step(15));
}

void test_first_countdown_step_none_left_inside_led_window(void) {
    /* Under 15 s only the LED binary countdown remains */
    TEST_ASSERT_EQUAL_INT(WAKE_COUNTDOWN_STEPS, wake_policy_first_countdown_step(14));
    TEST_ASSERT_EQUAL_INT(WAKE_COUNTDOWN_STEPS, wake_policy_first_countdown_step(0));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_render_expiry_transition_fires_alert);
    RUN_TEST(test_render_already_expired_never_refires_alert);
    RUN_TEST(test_render_tick_state_change_promotes_to_full);
    RUN_TEST(test_render_button_wake_is_partial_within_main_layout);
    RUN_TEST(test_render_button_wake_across_break_layout_is_full);
    RUN_TEST(test_render_steady_timer_tick_is_partial);
    RUN_TEST(test_break_chime_suppressed_while_an_extra_runs);
    RUN_TEST(test_break_chime_fires_when_observed_promptly);
    RUN_TEST(test_break_chime_suppressed_when_observed_late);
    RUN_TEST(test_break_chime_grace_is_the_watch_window);
    RUN_TEST(test_break_chime_ignores_negative_lateness);
    RUN_TEST(test_render_selection_change_suppresses_the_expiry_alert);
    RUN_TEST(test_render_selection_change_across_the_break_screen_is_full);
    RUN_TEST(test_render_break_ended_promotes_partial_to_full);
    RUN_TEST(test_render_break_ended_does_not_outrank_the_expiry_alert);
    RUN_TEST(test_render_without_break_ended_is_unchanged);
    RUN_TEST(test_snap_absorbs_positive_jitter);
    RUN_TEST(test_snap_absorbs_negative_jitter);
    RUN_TEST(test_snap_leaves_honest_offgrid_values);
    RUN_TEST(test_snap_disabled_inside_watch_window);
    RUN_TEST(test_snap_ignores_non_positive);
    RUN_TEST(test_sync_running_follows_recheck_flag);
    RUN_TEST(test_sync_break_never_syncs);
    RUN_TEST(test_sync_clock_states_use_idle_cadence);
    RUN_TEST(test_sync_never_synced_is_always_due_in_clock_states);
    RUN_TEST(test_grid_wait_running_waits_countdown_residue);
    RUN_TEST(test_grid_wait_running_on_grid_needs_no_wait);
    RUN_TEST(test_grid_wait_running_residue_beyond_max_renders_in_place);
    RUN_TEST(test_grid_wait_expiry_already_passed_is_zero);
    RUN_TEST(test_grid_wait_break_uses_break_grid);
    RUN_TEST(test_grid_wait_clock_states_use_wall_grid);
    RUN_TEST(test_grid_wait_on_wall_boundary_is_zero);
    RUN_TEST(test_grid_wait_wall_residue_beyond_max_is_zero);
    RUN_TEST(test_countdown_step_values_are_quarter_minute);
    RUN_TEST(test_first_countdown_step_from_full_watch);
    RUN_TEST(test_first_countdown_step_skips_passed_marks_on_late_wake);
    RUN_TEST(test_first_countdown_step_none_left_inside_led_window);
    return UNITY_END();
}
