#include <string.h>
#include <unity.h>

/* Single-TU: the day-scoped decisions of one HA window (BUG-14), and the
   real act payload builder they feed, so the act cases below assert what
   HA's number box would actually be sent. mqtt_ha.c itself is in no host
   suite (esp-mqtt), which is why these were lifted out of it (cycle-2
   review, MINOR-3). */
// clang-format off
#include "../../main/stats_json.c"
#include "../../main/ha_day_cmds.c"
// clang-format on

void setUp(void) {}
void tearDown(void) {}

/* ---- a Screen-adjust target that arrived this window ------------------- */

/* The ordinary window: a settled day, nothing being cleared. */
void test_a_target_on_a_settled_day_is_buffered(void) {
    TEST_ASSERT_EQUAL(HA_BONUS_BUFFER, ha_day_bonus_fate(false, false));
}

/* No clock: held, left retained for the first window with a day (owner
   decision Q1). */
void test_a_target_without_a_clock_is_held(void) {
    TEST_ASSERT_EQUAL(HA_BONUS_HOLD, ha_day_bonus_fate(true, false));
}

/* A day being cleared: the retained target is that day's, so it is
   dropped. This is also a target a parent set during the no-clock lock
   when the release starts a fresh day (owner decision Q-A: a new day
   starts with no bonus). */
void test_a_target_on_a_day_being_cleared_is_dropped(void) {
    TEST_ASSERT_EQUAL(HA_BONUS_DROP, ha_day_bonus_fate(false, true));
}

/* NO CLOCK OUTRANKS THE CLEAR. The power-on rollover queued a clear
   before a window whose NTP failed: nobody knows yet whether the target
   is stale, so it is held, not dropped (the release may restore the very
   day it belongs to). */
void test_no_clock_outranks_a_pending_clear(void) {
    TEST_ASSERT_EQUAL(HA_BONUS_HOLD, ha_day_bonus_fate(true, true));
}

/* ---- the day's bonus clear ---------------------------------------------- */

void test_a_pending_clear_is_published_on_a_settled_day(void) {
    TEST_ASSERT_TRUE(ha_day_publish_clear(false, true));
}

/* A NO-CLOCK WINDOW NEVER PUBLISHES A CLEAR: published, the power-on
   rollover's clear would revoke the bonus of a day the release restores. */
void test_a_no_clock_window_never_publishes_a_clear(void) {
    TEST_ASSERT_FALSE(ha_day_publish_clear(true, true));
    TEST_ASSERT_FALSE(ha_day_publish_clear(true, false));
}

void test_nothing_pending_publishes_nothing(void) {
    TEST_ASSERT_FALSE(ha_day_publish_clear(false, false));
}

/* ---- the act state HA's number box is sent ------------------------------ */

static const char *act_json(act_state_t a) {
    static char buf[96];
    TEST_ASSERT_TRUE(stats_json_act(buf, sizeof buf, &a) < (int)sizeof buf);
    return buf;
}

/* Nothing arrived: the snapshot's applied figure. A stale buffered_s
   (last window's) must not leak through. */
void test_act_with_no_target_reports_the_applied_figure(void) {
    act_state_t a = ha_day_act_state(600, false, 1800, false, 0, false);
    TEST_ASSERT_FALSE(a.target_pending);
    TEST_ASSERT_EQUAL_STRING("{\"screen_bonus\":10,\"locate\":\"OFF\"}", act_json(a));
}

/* Buffered this window: reported now, not a window later. */
void test_act_reports_a_buffered_target(void) {
    act_state_t a = ha_day_act_state(600, true, -2700, false, 0, false);
    TEST_ASSERT_TRUE(a.target_pending);
    TEST_ASSERT_EQUAL_STRING("{\"screen_bonus\":-45,\"locate\":\"OFF\"}", act_json(a));
}

/* HELD: reported too, so HA's box keeps the parent's value instead of
   snapping to the locked stand-in's 0 — even though the buffered slot is
   empty (and holds a stale figure). */
void test_act_reports_a_held_target_over_the_stand_ins_zero(void) {
    act_state_t a = ha_day_act_state(0, false, 1800, true, 1200, false);
    TEST_ASSERT_TRUE(a.target_pending);
    TEST_ASSERT_EQUAL_INT32(1200, a.target_s);
    TEST_ASSERT_EQUAL_STRING("{\"screen_bonus\":20,\"locate\":\"OFF\"}", act_json(a));
}

/* HELD BEATS BUFFERED. Not both within one window (the fate is fixed per
   window), but s_bonus_target_pending can be left set by an earlier window
   of the same wake: this window's held target is the one HA is shown. */
void test_act_prefers_a_held_target_over_a_stale_buffered_one(void) {
    act_state_t a = ha_day_act_state(0, true, 1800, true, 1200, false);
    TEST_ASSERT_TRUE(a.target_pending);
    TEST_ASSERT_EQUAL_INT32(1200, a.target_s);
    TEST_ASSERT_EQUAL_STRING("{\"screen_bonus\":20,\"locate\":\"OFF\"}", act_json(a));
}

/* A day being cleared reports 0 over everything (the Q-A drop is visible
   in HA). */
void test_act_on_a_cleared_day_reports_zero(void) {
    act_state_t a = ha_day_act_state(600, false, 0, false, 0, true);
    TEST_ASSERT_TRUE(a.day_cleared);
    TEST_ASSERT_EQUAL_STRING("{\"screen_bonus\":0,\"locate\":\"OFF\"}", act_json(a));
}

/* ---- one window, as mqtt_ha.c is meant to sequence the three ------------
   A hand copy of apply_sets()'s order, not a call into it (mqtt_ha.c is in
   no host suite): it pins the three functions working together, NOT that
   mqtt_ha.c keeps this order. The comment at its s_bonus_clear_pending =
   false says why the order matters there. */

typedef struct {
    bool buffered;
    int32_t buffered_s;
    bool held;
    int32_t held_s;
    bool clear_published;
    const char *act;
} window_t;

/* The order mqtt_ha.c's apply_sets() runs them in: the target's fate
   (read against the clear flag BEFORE it is consumed), then the clear,
   then the act state. */
static window_t run_window(bool no_clock, bool clear_pending, bool target_arrived, int32_t target_s,
                           int32_t applied_s) {
    window_t w = {0};
    if (target_arrived) {
        switch (ha_day_bonus_fate(no_clock, clear_pending)) {
            case HA_BONUS_HOLD:
                w.held = true;
                w.held_s = target_s;
                break;
            case HA_BONUS_BUFFER:
                w.buffered = true;
                w.buffered_s = target_s;
                break;
            default:
                break;
        }
    }
    w.clear_published = ha_day_publish_clear(no_clock, clear_pending);
    w.act = act_json(ha_day_act_state(applied_s, w.buffered, w.buffered_s, w.held, w.held_s, w.clear_published));
    return w;
}

/* The power-on rollover's window, NTP failed: target held and shown, the
   clear dropped. */
void test_window_locked_holds_the_target_and_drops_the_clear(void) {
    window_t w = run_window(true, true, true, 1800, 0);
    TEST_ASSERT_TRUE(w.held);
    TEST_ASSERT_FALSE(w.buffered);
    TEST_ASSERT_FALSE(w.clear_published);
    TEST_ASSERT_EQUAL_STRING("{\"screen_bonus\":30,\"locate\":\"OFF\"}", w.act);
}

/* The release onto a fresh day: the old day's target is dropped, the
   clear goes out, HA is shown 0. */
void test_window_on_a_fresh_day_drops_the_target_and_clears(void) {
    window_t w = run_window(false, true, true, 1800, 0);
    TEST_ASSERT_FALSE(w.held);
    TEST_ASSERT_FALSE(w.buffered);
    TEST_ASSERT_TRUE(w.clear_published);
    TEST_ASSERT_EQUAL_STRING("{\"screen_bonus\":0,\"locate\":\"OFF\"}", w.act);
}

/* The release onto a restored day: the held target is applied at last. */
void test_window_on_a_restored_day_buffers_the_held_target(void) {
    window_t w = run_window(false, false, true, 1800, 0);
    TEST_ASSERT_TRUE(w.buffered);
    TEST_ASSERT_FALSE(w.clear_published);
    TEST_ASSERT_EQUAL_STRING("{\"screen_bonus\":30,\"locate\":\"OFF\"}", w.act);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_a_target_on_a_settled_day_is_buffered);
    RUN_TEST(test_a_target_without_a_clock_is_held);
    RUN_TEST(test_a_target_on_a_day_being_cleared_is_dropped);
    RUN_TEST(test_no_clock_outranks_a_pending_clear);
    RUN_TEST(test_a_pending_clear_is_published_on_a_settled_day);
    RUN_TEST(test_a_no_clock_window_never_publishes_a_clear);
    RUN_TEST(test_nothing_pending_publishes_nothing);
    RUN_TEST(test_act_with_no_target_reports_the_applied_figure);
    RUN_TEST(test_act_reports_a_buffered_target);
    RUN_TEST(test_act_reports_a_held_target_over_the_stand_ins_zero);
    RUN_TEST(test_act_prefers_a_held_target_over_a_stale_buffered_one);
    RUN_TEST(test_act_on_a_cleared_day_reports_zero);
    RUN_TEST(test_window_locked_holds_the_target_and_drops_the_clear);
    RUN_TEST(test_window_on_a_fresh_day_drops_the_target_and_clears);
    RUN_TEST(test_window_on_a_restored_day_buffers_the_held_target);
    return UNITY_END();
}
