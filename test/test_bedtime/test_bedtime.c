#include <unity.h>

/* Single-TU compilation */
#include "../../main/bedtime.c"
#include "../../main/quiet_hours.c"

void setUp(void) {}
void tearDown(void) {}

#define MIN_OF(h, m) ((h)*60 + (m))

/* ---- config validation ---- */

void test_hhmm_zero_is_valid_disabled(void) {
    TEST_ASSERT_TRUE(bedtime_hhmm_valid(0));
    TEST_ASSERT_EQUAL_INT(-1, bedtime_minutes(0));
}

void test_hhmm_evening_range_accepted(void) {
    TEST_ASSERT_TRUE(bedtime_hhmm_valid(1800));
    TEST_ASSERT_TRUE(bedtime_hhmm_valid(2200));
    TEST_ASSERT_TRUE(bedtime_hhmm_valid(2359));
    TEST_ASSERT_EQUAL_INT(MIN_OF(22, 0), bedtime_minutes(2200));
}

void test_hhmm_outside_evening_rejected(void) {
    TEST_ASSERT_FALSE(bedtime_hhmm_valid(1759)); /* before the floor */
    TEST_ASSERT_FALSE(bedtime_hhmm_valid(900));  /* daytime would brick the device until midnight */
    TEST_ASSERT_FALSE(bedtime_hhmm_valid(1));
    TEST_ASSERT_FALSE(bedtime_hhmm_valid(-2200));
    TEST_ASSERT_FALSE(bedtime_hhmm_valid(2400)); /* not a clock time */
    TEST_ASSERT_FALSE(bedtime_hhmm_valid(1860)); /* minute > 59 */
    TEST_ASSERT_EQUAL_INT(-1, bedtime_minutes(1860));
}

/* ---- active window ---- */

void test_active_at_and_after_threshold(void) {
    int bed = bedtime_minutes(2200);
    TEST_ASSERT_FALSE(bedtime_active(MIN_OF(21, 59), bed));
    TEST_ASSERT_TRUE(bedtime_active(MIN_OF(22, 0), bed)); /* exactly bedtime */
    TEST_ASSERT_TRUE(bedtime_active(MIN_OF(23, 59), bed));
}

void test_inactive_after_midnight_and_daytime(void) {
    int bed = bedtime_minutes(2200);
    TEST_ASSERT_FALSE(bedtime_active(MIN_OF(0, 0), bed)); /* rollover territory */
    TEST_ASSERT_FALSE(bedtime_active(MIN_OF(7, 30), bed));
    TEST_ASSERT_FALSE(bedtime_active(MIN_OF(17, 59), bed));
}

void test_disabled_is_never_active(void) {
    int bed = bedtime_minutes(0);
    TEST_ASSERT_FALSE(bedtime_active(MIN_OF(23, 0), bed));
    TEST_ASSERT_FALSE(bedtime_break_would_cross(MIN_OF(21, 50), 15, bed));
}

/* ---- break crossing ---- */

void test_break_crossing_boundaries(void) {
    int bed = bedtime_minutes(2200);
    /* 21:50 + 15 min ends 22:05 - crosses */
    TEST_ASSERT_TRUE(bedtime_break_would_cross(MIN_OF(21, 50), 15, bed));
    /* 21:45 + 15 min lands exactly on 22:00 - counts as crossing */
    TEST_ASSERT_TRUE(bedtime_break_would_cross(MIN_OF(21, 45), 15, bed));
    /* 21:44 + 15 min ends 21:59 - does not cross */
    TEST_ASSERT_FALSE(bedtime_break_would_cross(MIN_OF(21, 44), 15, bed));
    /* already past bedtime: the active check owns that case, not this one */
    TEST_ASSERT_FALSE(bedtime_break_would_cross(MIN_OF(22, 10), 15, bed));
}

/* ---- alert matrix ---- */

void test_alert_only_for_running_and_break(void) {
    TEST_ASSERT_TRUE(bedtime_should_alert(TIMER_RUNNING, false));
    TEST_ASSERT_TRUE(bedtime_should_alert(TIMER_BREAK, false));
    TEST_ASSERT_FALSE(bedtime_should_alert(TIMER_IDLE, false));
    TEST_ASSERT_FALSE(bedtime_should_alert(TIMER_PAUSED, false));
    TEST_ASSERT_FALSE(bedtime_should_alert(TIMER_EXPIRED, false));
}

void test_alert_for_a_background_break(void) {
    /* A break now runs on slot 0 behind whatever timer is selected, so
       the ACTIVE state no longer tells the whole story: bed time landing
       mid-break still interrupted something. */
    TEST_ASSERT_TRUE(bedtime_should_alert(TIMER_PAUSED, true));
    TEST_ASSERT_TRUE(bedtime_should_alert(TIMER_IDLE, true));
    TEST_ASSERT_TRUE(bedtime_should_alert(TIMER_EXPIRED, true));
    /* Screen selected during its own break: both signals agree */
    TEST_ASSERT_TRUE(bedtime_should_alert(TIMER_BREAK, true));
}

/* ---- break-eligible timers need no new bedtime logic ----

   Verified against the call sites rather than assumed: both of these fall
   out of code that already exists, and both tests exist to catch a future
   change that quietly breaks them.

   1. bedtime_break_would_cross sits INSIDE wake_flow_maybe_start_break, which
      source-agnostic — it is reached the same way
      whether the balance was driven by Screen or by a laundry-folding
      chore. A break that would still be running at bed time is skipped in
      favour of Bed Time itself, whoever earned it. */
void test_break_crossing_is_source_agnostic(void) {
    /* 20:45, a 30 min break, bed time 21:00: crosses, so Bed Time wins —
       the caller never sees which slot pushed the balance over. */
    TEST_ASSERT_TRUE(bedtime_break_would_cross(MIN_OF(20, 45), 30, MIN_OF(21, 0)));
    /* Comfortably clear: the break runs and bed time waits for it. */
    TEST_ASSERT_FALSE(bedtime_break_would_cross(MIN_OF(20, 0), 30, MIN_OF(21, 0)));
    /* Exactly touching the threshold still counts as crossing. */
    TEST_ASSERT_TRUE(bedtime_break_would_cross(MIN_OF(20, 30), 30, MIN_OF(21, 0)));
}

/* 2. bedtime_should_alert keys on the state of the ACTIVE slot, and only
      the active slot can be RUNNING (I1). A break-eligible timer running
      up to bed time therefore fires the bed-time screen audibly, exactly
      as the Screen timer does — and lock_gate_bedtime_engage's timer_pause
      covers it, since it pauses whatever is RUNNING. */
void test_alert_for_an_eligible_timer_running_into_bedtime(void) {
    /* Violin RUNNING with no break in progress: RUNNING is enough. */
    TEST_ASSERT_TRUE(bedtime_should_alert(TIMER_RUNNING, false));
    /* ...and equally when it is running behind a background break. */
    TEST_ASSERT_TRUE(bedtime_should_alert(TIMER_RUNNING, true));
    /* A non-eligible timer is the same code path — the flag never
       reaches this decision, which is the point. */
    TEST_ASSERT_FALSE(bedtime_should_alert(TIMER_IDLE, false));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_break_crossing_is_source_agnostic);
    RUN_TEST(test_alert_for_an_eligible_timer_running_into_bedtime);
    RUN_TEST(test_hhmm_zero_is_valid_disabled);
    RUN_TEST(test_hhmm_evening_range_accepted);
    RUN_TEST(test_hhmm_outside_evening_rejected);
    RUN_TEST(test_active_at_and_after_threshold);
    RUN_TEST(test_inactive_after_midnight_and_daytime);
    RUN_TEST(test_disabled_is_never_active);
    RUN_TEST(test_break_crossing_boundaries);
    RUN_TEST(test_alert_only_for_running_and_break);
    RUN_TEST(test_alert_for_a_background_break);
    return UNITY_END();
}
