#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unity.h>

/* Single-TU compilation. button_actions.c (+ its schedule dependency) rides
   along so the state matrix can assert the real Button A outcome per row
   instead of re-deriving it here. */
// clang-format off
#include "mock_hal_time.c"
#include "mock_hal_nvs.c"
#include "../../main/timer.c"
#include "../../main/schedule.c"
#include "../../main/button_actions.c"
#include "../../main/wake_policy.c"
// clang-format on

/* SLEEP_PLAN_WATCH_SEC / BREAK_CHIME_GRACE_SEC — the chime grace is derived
   from the watch window, so the test pins the real constant, not a copy. */
#include "sleep_plan.h"

/* Base timestamp: 2026-01-05 00:00:00 UTC (Monday) */
#define T0 ((time_t)1767571200)

/* Slot table used by the multi-timer tests: slot 0 = Screen (schedule-fed),
   slot 2 left disabled to prove select_next skips holes. */
static const timer_def_t TEST_DEFS[TIMER_SLOT_COUNT] = {
    {"Screen", 0, false, false},   /* slot 0: never break-eligible */
    {"Piano", 900, true, true},    /* break-eligible: drains the balance */
    {"", 0, false, false},         /* disabled */
    {"Laundry", 600, true, false}, /* a chore done with the TV on: feeds it */
    {"Violin", 900, false, true},  /* break-eligible */
};

#define SLOT_PIANO 1
#define SLOT_LAUNDRY 3
#define SLOT_VIOLIN 4

/* I5 witness state — see assert_state_legal below. */
static bool s_i5_valid;
static int64_t s_i5_break_end;
static int32_t s_i5_frozen;

void setUp(void) {
    mock_nvs_reset();
    schedule_cache_invalidate();
    setenv("TZ", "UTC0", 1);
    tzset();
    hal_nvs_write_u16("weekday_min", 60); /* Screen allocation for button_a_apply */
    timer_set_defs(TEST_DEFS, TIMER_SLOT_COUNT);
    timer_reset();
    mock_time_set(T0);
    s_i5_valid = false;
}

/* Structural invariants I1-I5 of the non-blocking-break state model
   (docs/planning/20260727.breaktime.plan.md):

     I1  BREAK only ever appears on slot 0
     I2  at most one slot is RUNNING at any time
     I3  a RUNNING slot is always the active slot
     I4  break_expiry_wall != 0 iff slot 0 is BREAK
     I5  slot 0 BREAK => slot 0 holds the FROZEN screen time: no wall
         expiry is armed (the mechanism), and the frozen value does not
         drift while the break's identity is unchanged (the content —
         see the witness below)

   Called from tearDown, which Unity runs after EVERY test in this suite —
   the pre-existing tests included. A regression anywhere in timer.c that
   leaves an illegal state behind trips here, not three releases later.
   Call it explicitly mid-test too: the I5 witness only has teeth when it
   gets more than one observation. */

/* The I5 witness (declared above setUp) holds the frozen screen time seen
   last time slot 0 was observed holding THIS break — keyed on
   break_expiry_wall, so a fresh break starts a fresh witness.

   KNOWN LIMITATION — read this before "fixing" a failure it reports.
   The witness cannot tell an extra slot's run consuming Screen's time
   (the real I5 violation) from an HA grant deliberately moving it:
   timer_adjust(0, n) during a break legitimately changes
   remaining_at_pause. No current test observes twice within one break
   across such an adjust, so none trips. If you add one — a grant during
   a break, say — the witness will report "I5: frozen screen time
   changed" and it will be a FALSE POSITIVE. Re-baseline it by calling
   assert_state_legal() immediately after the adjust and before the next
   observation, or set s_i5_valid = false; do not weaken the check, which
   is what actually catches the failure mode I5 exists for. */
static void assert_state_legal(void) {
    int running = 0;
    for (int i = 0; i < TIMER_SLOT_COUNT; i++) {
        const timer_slot_state_t *sl = &g_rtc_state.slots[i];
        if (i > 0) {
            TEST_ASSERT_TRUE_MESSAGE(sl->state != TIMER_BREAK, "I1: BREAK on an extra slot");
            TEST_ASSERT_EQUAL_INT64_MESSAGE(0, sl->break_expiry_wall, "I4: break expiry on an extra slot");
        }
        if (sl->state == TIMER_RUNNING) {
            running++;
            TEST_ASSERT_EQUAL_INT_MESSAGE(i, timer_active_slot(), "I3: RUNNING slot is not the active slot");
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(running <= 1, "I2: more than one RUNNING slot");

    const timer_slot_state_t *s0 = &g_rtc_state.slots[0];
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)(s0->state == TIMER_BREAK), (int)(s0->break_expiry_wall != 0),
                                  "I4: break_expiry_wall != 0 must mean slot 0 is BREAK, and vice versa");
    if (s0->state == TIMER_BREAK) {
        /* The mechanism: nothing is counting the screen timer down. */
        TEST_ASSERT_EQUAL_INT64_MESSAGE(0, s0->expiry_wall_time, "I5: a break must not leave the screen expiry armed");
        /* The content: within one break, the frozen value must not drift.
           This is what catches an extra slot's run consuming Screen's
           time — the failure mode the invariant exists for. */
        if (s_i5_valid && s_i5_break_end == s0->break_expiry_wall) {
            TEST_ASSERT_EQUAL_INT32_MESSAGE(s_i5_frozen, s0->remaining_at_pause,
                                            "I5: frozen screen time changed while the break was running");
        }
        s_i5_valid = true;
        s_i5_break_end = s0->break_expiry_wall;
        s_i5_frozen = s0->remaining_at_pause;
    } else {
        s_i5_valid = false; /* no break: nothing to hold frozen */
    }
}

void tearDown(void) {
    assert_state_legal();
}

void test_reset_state_is_idle(void) {
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state());
}

void test_reset_expiry_is_zero(void) {
    TEST_ASSERT_EQUAL_INT64(0, g_rtc_state.slots[0].expiry_wall_time);
}

void test_reset_remaining_at_pause_is_zero(void) {
    TEST_ASSERT_EQUAL_INT32(0, g_rtc_state.slots[0].remaining_at_pause);
}

void test_start_sets_state_running(void) {
    timer_start(T0, 3600);
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
}

void test_start_sets_expiry_wall_time(void) {
    timer_start(T0, 3600);
    TEST_ASSERT_EQUAL_INT64((int64_t)T0 + 3600, g_rtc_state.slots[0].expiry_wall_time);
}

void test_start_sets_allocation_sec(void) {
    timer_start(T0, 3600);
    TEST_ASSERT_EQUAL_INT32(3600, g_rtc_state.slots[0].allocation_sec);
}

void test_tick_does_not_modify_expiry_wall_time(void) {
    timer_start(T0, 3600);
    int64_t expiry_before = g_rtc_state.slots[0].expiry_wall_time;

    mock_time_set(T0 + 600);
    timer_tick(T0 + 600);
    mock_time_set(T0 + 1200);
    timer_tick(T0 + 1200);

    TEST_ASSERT_EQUAL_INT64(expiry_before, g_rtc_state.slots[0].expiry_wall_time);
}

void test_tick_returns_correct_remaining_seconds(void) {
    timer_start(T0, 3600);
    int32_t remaining = timer_tick(T0 + 1000);
    TEST_ASSERT_EQUAL_INT32(2600, remaining);
}

void test_tick_returns_remaining_near_zero(void) {
    timer_start(T0, 3600);
    int32_t remaining = timer_tick(T0 + 3599);
    TEST_ASSERT_EQUAL_INT32(1, remaining);
}

void test_tick_transitions_to_expired_when_time_elapsed(void) {
    timer_start(T0, 3600);
    timer_tick(T0 + 3601);
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
}

void test_tick_returns_non_positive_when_expired(void) {
    timer_start(T0, 3600);
    int32_t remaining = timer_tick(T0 + 3601);
    // cppcheck-suppress knownConditionTrueFalse
    TEST_ASSERT_TRUE(remaining <= 0);
}

void test_tick_at_exact_expiry_transitions(void) {
    timer_start(T0, 3600);
    timer_tick(T0 + 3600);
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
}

void test_pause_sets_state_paused(void) {
    timer_start(T0, 3600);
    timer_pause(T0 + 1000);
    TEST_ASSERT_EQUAL(TIMER_PAUSED, timer_get_state());
}

void test_pause_saves_remaining_at_pause(void) {
    timer_start(T0, 3600);
    timer_pause(T0 + 1000);
    TEST_ASSERT_EQUAL_INT32(2600, g_rtc_state.slots[0].remaining_at_pause);
}

void test_pause_clears_expiry_wall_time(void) {
    timer_start(T0, 3600);
    timer_pause(T0 + 1000);
    TEST_ASSERT_EQUAL_INT64(0, g_rtc_state.slots[0].expiry_wall_time);
}

void test_resume_sets_state_running(void) {
    timer_start(T0, 3600);
    timer_pause(T0 + 1000);
    timer_resume(T0 + 2000);
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
}

void test_resume_sets_expiry_from_remaining_at_pause(void) {
    timer_start(T0, 3600);
    timer_pause(T0 + 1000);
    timer_resume(T0 + 2000);
    int64_t expected = (int64_t)(T0 + 2000) + 2600;
    TEST_ASSERT_EQUAL_INT64(expected, g_rtc_state.slots[0].expiry_wall_time);
}

void test_resume_preserves_remaining_within_one_second(void) {
    timer_start(T0, 3600);
    timer_pause(T0 + 1000);
    time_t resume_time = T0 + 5000;
    timer_resume(resume_time);
    int32_t remaining = timer_tick(resume_time);
    TEST_ASSERT_INT_WITHIN(1, 2600, remaining);
}

void test_full_cycle_idle_run_pause_resume_expire(void) {
    timer_start(T0, 100);
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());

    timer_pause(T0 + 40);
    TEST_ASSERT_EQUAL_INT32(60, g_rtc_state.slots[0].remaining_at_pause);

    timer_resume(T0 + 100);
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());

    int32_t remaining = timer_tick(T0 + 159);
    TEST_ASSERT_INT_WITHIN(1, 1, remaining);

    timer_tick(T0 + 161);
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
}

void test_record_date_formats_yyyy_mm_dd(void) {
    /* Pin the exact stored format (guards the fill_date implementation) */
    char *old_tz = getenv("TZ");
    setenv("TZ", "UTC0", 1);
    tzset();
    timer_record_date(T0); /* 2026-01-05 00:00:00 UTC */
    TEST_ASSERT_EQUAL_STRING("2026-01-05", g_rtc_state.last_date);
    if (old_tz)
        setenv("TZ", old_tz, 1);
    else
        unsetenv("TZ");
    tzset();
}

void test_is_new_day_false_when_same_date(void) {
    timer_record_date(T0);
    TEST_ASSERT_FALSE(timer_is_new_day(T0 + 3600));
}

void test_is_new_day_true_after_midnight(void) {
    timer_record_date(T0);
    TEST_ASSERT_TRUE(timer_is_new_day(T0 + 86400));
}

void test_needs_ntp_sync_false_immediately_after_sync(void) {
    timer_record_ntp_sync(T0);
    TEST_ASSERT_FALSE(timer_needs_ntp_sync(T0 + 1));
}

void test_needs_ntp_sync_true_after_10_minutes(void) {
    timer_record_ntp_sync(T0);
    TEST_ASSERT_TRUE(timer_needs_ntp_sync(T0 + NTP_SYNC_INTERVAL_SEC + 1));
}

void test_tick_while_paused_returns_remaining_at_pause(void) {
    timer_start(T0, 3600);
    timer_pause(T0 + 1000);                 /* remaining_at_pause = 2600 */
    int32_t result = timer_tick(T0 + 9999); /* arbitrary time, shouldn't matter */
    TEST_ASSERT_EQUAL_INT32(2600, result);
}

void test_is_new_day_true_on_cold_boot(void) {
    /* After timer_reset(), last_date is zeroed — cold boot should force re-init */
    TEST_ASSERT_TRUE(timer_is_new_day(T0));
}

/* timer_shift_expiry: after an immediate start on an unsynced clock, the
   post-start NTP sync steps time(NULL); the expiry must step with it. */

void test_shift_expiry_forward_while_running(void) {
    timer_start(T0, 3600);
    timer_shift_expiry(120); /* clock stepped 2 min forward */
    TEST_ASSERT_EQUAL_INT64((int64_t)T0 + 3600 + 120, g_rtc_state.slots[0].expiry_wall_time);
    TEST_ASSERT_EQUAL_INT32(3600, timer_tick(T0 + 120)); /* remaining unchanged */
}

void test_shift_expiry_backward_while_running(void) {
    timer_start(T0, 3600);
    timer_shift_expiry(-90);
    TEST_ASSERT_EQUAL_INT64((int64_t)T0 + 3600 - 90, g_rtc_state.slots[0].expiry_wall_time);
}

void test_shift_expiry_noop_when_paused(void) {
    /* remaining_at_pause is a duration, not a wall time — never shifted */
    timer_start(T0, 3600);
    timer_pause(T0 + 1000);
    timer_shift_expiry(120);
    TEST_ASSERT_EQUAL_INT32(2600, g_rtc_state.slots[0].remaining_at_pause);
    TEST_ASSERT_EQUAL_INT64(0, g_rtc_state.slots[0].expiry_wall_time);
}

void test_shift_expiry_noop_when_idle(void) {
    timer_shift_expiry(120);
    TEST_ASSERT_EQUAL_INT64(0, g_rtc_state.slots[0].expiry_wall_time);
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state());
}

/* ---- the screen-exposure balance (break_eligible) ----

   Rows below are the plan's behaviour table
   (docs/planning/20260728.breakeligible.plan.md); the numbers in the test
   names are that table's row numbers, so a failure points straight at the
   contract it broke. */

/* Walk Button C round to `slot` (timer_select_next skips disabled holes). */
static void select_slot(int slot) {
    for (int i = 0; i <= TIMER_SLOT_COUNT && timer_active_slot() != slot; i++)
        TEST_ASSERT_TRUE_MESSAGE(timer_select_next(), "select_slot: swap refused");
    TEST_ASSERT_EQUAL_INT(slot, timer_active_slot());
}

/* Row 2: a non-eligible extra feeds slot 0's balance and earns the break,
   with Screen never started. The break pauses it and snaps to slot 0. */
void test_row2_a_laundry_run_earns_the_break_and_is_paused_by_it(void) {
    select_slot(SLOT_LAUNDRY);
    timer_start(T0, 3600); /* Laundry: not break_eligible -> +1:1 */
    TEST_ASSERT_EQUAL_INT32(1800, timer_run_accum(T0 + 1800));
    TEST_ASSERT_TRUE(timer_break_due(T0 + 1800, 1800));

    timer_start_break(T0 + 1800, 900);
    TEST_ASSERT_EQUAL(TIMER_PAUSED, timer_slot_state(SLOT_LAUNDRY));
    TEST_ASSERT_EQUAL_INT32(1800, g_rtc_state.slots[SLOT_LAUNDRY].remaining_at_pause);
    TEST_ASSERT_EQUAL(TIMER_BREAK, timer_slot_state(0));
    TEST_ASSERT_EQUAL_INT(SLOT_LAUNDRY, timer_break_interrupted_slot());
    TEST_ASSERT_EQUAL_INT(0, timer_active_slot()); /* selection snapped */
}

/* Row 3 (timer half): the expiry lands and the break is due in the SAME
   tick. main.c owns the ordering (expiry alert, then maybe_start_break);
   what timer.c must guarantee is that the expiry folds the segment and
   leaves the break genuinely due rather than swallowing it. */
void test_row3_expiry_and_a_due_break_in_one_tick(void) {
    select_slot(SLOT_LAUNDRY);
    timer_start(T0, 1800); /* sized to end exactly on the interval */
    TEST_ASSERT_FALSE(timer_break_due(T0 + 1799, 1800));

    timer_tick(T0 + 1800);
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_slot_state(SLOT_LAUNDRY));
    TEST_ASSERT_TRUE(timer_break_due(T0 + 1800, 1800));
    timer_start_break(T0 + 1800, 900);
    TEST_ASSERT_EQUAL_INT(SLOT_LAUNDRY, timer_break_interrupted_slot());
}

/* Row 4: a break-eligible timer starts normally during a break. */
void test_row4_a_break_starts_an_eligible_timer_normally(void) {
    timer_start(T0, 3600);
    timer_start_break(T0 + 1800, 900);
    select_slot(SLOT_PIANO);
    TEST_ASSERT_TRUE(timer_start_allowed());
    TEST_ASSERT_EQUAL(BTN_A_STARTED, button_a_apply(T0 + 1800));
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_slot_state(SLOT_PIANO));
}

/* Row 5: a non-eligible timer is refused during a break. */
void test_row5_a_break_refuses_a_non_eligible_timer(void) {
    timer_start(T0, 3600);
    timer_start_break(T0 + 1800, 900);
    select_slot(SLOT_LAUNDRY);
    TEST_ASSERT_FALSE(timer_start_allowed());
    TEST_ASSERT_EQUAL(BTN_A_NONE, button_a_apply(T0 + 1800));
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_slot_state(SLOT_LAUNDRY));
}

/* Row 6: Screen itself is never break-eligible, so the break screen's
   Button A stays refused (unchanged behaviour, now via the same rule). */
void test_row6_a_break_refuses_screen_itself(void) {
    timer_start(T0, 3600);
    timer_start_break(T0 + 1800, 900);
    TEST_ASSERT_EQUAL_INT(0, timer_active_slot());
    TEST_ASSERT_FALSE(timer_start_allowed());
    TEST_ASSERT_EQUAL(BTN_A_NONE, button_a_apply(T0 + 1800));
    TEST_ASSERT_EQUAL(TIMER_BREAK, timer_slot_state(0));
}

/* Row 7: break end returns the selection to the interrupted slot. */
void test_row7_break_end_returns_to_the_interrupted_slot(void) {
    select_slot(SLOT_LAUNDRY);
    timer_start(T0, 3600);
    timer_start_break(T0 + 1800, 900);
    TEST_ASSERT_EQUAL_INT(0, timer_active_slot());

    timer_break_tick(T0 + 2700);
    TEST_ASSERT_TRUE(timer_break_take_ended(T0 + 2700, NULL));
    TEST_ASSERT_TRUE(timer_select_interrupted());
    TEST_ASSERT_EQUAL_INT(SLOT_LAUNDRY, timer_active_slot());
    TEST_ASSERT_EQUAL(TIMER_PAUSED, timer_slot_state(SLOT_LAUNDRY));
}

/* Row 8: a RUNNING timer at break end suppresses the snap (and, in
   main.c, the chime) — the selection is never stolen mid-run. */
void test_row8_break_end_does_not_snap_while_a_timer_runs(void) {
    timer_start(T0, 3600);
    timer_start_break(T0 + 1800, 900);
    select_slot(SLOT_PIANO);
    timer_start(T0 + 1800, 1800); /* eligible: allowed during the break */

    timer_break_tick(T0 + 2700);
    TEST_ASSERT_FALSE(timer_select_interrupted());
    TEST_ASSERT_EQUAL_INT(SLOT_PIANO, timer_active_slot());
}

/* Row 9: Screen interrupted -> selection stays on 0, banked time intact. */
void test_row9_break_end_stays_on_screen_when_screen_was_interrupted(void) {
    timer_start(T0, 3600);
    timer_start_break(T0 + 1800, 900);
    TEST_ASSERT_EQUAL_INT(0, timer_break_interrupted_slot());

    timer_break_tick(T0 + 2700);
    TEST_ASSERT_TRUE(timer_select_interrupted());
    TEST_ASSERT_EQUAL_INT(0, timer_active_slot());
    TEST_ASSERT_EQUAL(TIMER_PAUSED, timer_slot_state(0));
    TEST_ASSERT_EQUAL_INT32(1800, g_rtc_state.slots[0].remaining_at_pause);
}

/* Row 10 (I7): Screen never started today -> the break exits to IDLE, not
   to a PAUSED timer holding zero. */
void test_row10_break_exit_is_idle_when_screen_never_started(void) {
    select_slot(SLOT_LAUNDRY);
    timer_start(T0, 3600);
    timer_start_break(T0 + 1800, 900);
    TEST_ASSERT_EQUAL_INT32(0, g_rtc_state.slots[0].remaining_at_pause);

    timer_break_tick(T0 + 2700);
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_slot_state(0));
    TEST_ASSERT_EQUAL_INT64(0, g_rtc_state.slots[0].break_expiry_wall);
}

/* Row 11 (G2): expiry folds the live segment. Without the fold, up to a
   whole laundry run of accrual evaporates at expiry — and, keyed on
   run_started_wall rather than state, the balance would instead advance
   forever afterwards. */
void test_row11_expiry_folds_the_segment_and_stops_the_balance(void) {
    select_slot(SLOT_LAUNDRY);
    timer_start(T0, 1800);
    timer_tick(T0 + 1800); /* -> EXPIRED */

    TEST_ASSERT_EQUAL_INT32(1800, g_rtc_state.slots[0].run_accum_sec);
    TEST_ASSERT_EQUAL_INT64(0, g_rtc_state.slots[0].run_started_wall);
    TEST_ASSERT_EQUAL_INT32(1800, timer_run_accum(T0 + 9000)); /* frozen after */
    TEST_ASSERT_TRUE(timer_break_due(T0 + 1800, 1800));        /* nothing running */
}

/* Row 12 (G1): starting ANY timer must not reset the balance. Fold 25 min
   of laundry, press A on Piano, and the eye-rest clock is back to zero —
   the cheapest exploit there is, and one a kid finds by accident. */
void test_row12_starting_a_timer_does_not_reset_the_balance(void) {
    select_slot(SLOT_LAUNDRY);
    timer_start(T0, 3600);
    timer_pause(T0 + 1500); /* 25 min banked */
    TEST_ASSERT_EQUAL_INT32(1500, timer_run_accum(T0 + 1500));

    select_slot(0);
    timer_start(T0 + 1500, 3600); /* press A on Screen */
    TEST_ASSERT_EQUAL_INT32(1500, timer_run_accum(T0 + 1500));
    TEST_ASSERT_TRUE(timer_break_due(T0 + 1800, 1800)); /* 5 more minutes */
}

/* Row 13 (G3): a break entered from a non-RUNNING slot 0 must not
   recompute remaining_at_pause — a PAUSED Screen has expiry_wall_time 0,
   so the subtraction would clamp the kid's banked time to zero. */
void test_row13_break_start_preserves_a_paused_screens_banked_time(void) {
    timer_start(T0, 3600);
    timer_pause(T0 + 600); /* 3000 s banked, balance 600 */
    select_slot(SLOT_LAUNDRY);
    timer_start(T0 + 600, 3600);

    timer_start_break(T0 + 1800, 900);
    TEST_ASSERT_EQUAL(TIMER_BREAK, timer_slot_state(0));
    TEST_ASSERT_EQUAL_INT32(3000, g_rtc_state.slots[0].remaining_at_pause);
}

/* Row 14: an eligible run drains the balance 1:1. */
void test_row14_an_eligible_run_drains_the_balance(void) {
    timer_start(T0, 7200); /* Screen: +900 */
    timer_pause(T0 + 900);
    TEST_ASSERT_EQUAL_INT32(900, timer_run_accum(T0 + 900));

    select_slot(SLOT_VIOLIN);
    timer_start(T0 + 900, 1800); /* break-eligible: -600 */
    TEST_ASSERT_EQUAL_INT32(300, timer_run_accum(T0 + 1500));
    timer_pause(T0 + 1500);
    TEST_ASSERT_EQUAL_INT32(300, g_rtc_state.slots[0].run_accum_sec);
    TEST_ASSERT_EQUAL_INT32(300, timer_run_accum(T0 + 9000)); /* frozen */
}

/* Row 15: the floor is zero — three hours of violin must not buy three
   hours of uninterrupted TV. Enforced twice, because the in-flight
   segment is not folded yet: at read time and at fold time. */
void test_row15_the_balance_floors_at_zero(void) {
    timer_start(T0, 7200);
    timer_pause(T0 + 300); /* balance 300 */
    select_slot(SLOT_VIOLIN);
    timer_start(T0 + 300, 3600);

    TEST_ASSERT_EQUAL_INT32(0, timer_run_accum(T0 + 3900)); /* read clamps */
    timer_pause(T0 + 3900);
    TEST_ASSERT_EQUAL_INT32(0, g_rtc_state.slots[0].run_accum_sec); /* fold clamps */

    /* No banked credit: Screen still needs the full interval. */
    select_slot(0);
    timer_resume(T0 + 3900);
    TEST_ASSERT_FALSE(timer_break_due(T0 + 3900 + 1799, 1800));
    TEST_ASSERT_TRUE(timer_break_due(T0 + 3900 + 1800, 1800));
}

/* Row 16 (3d): an HA edit flipping break_eligible on a RUNNING slot must
   fold at the OLD sign, or the whole in-flight run is retroactively
   re-signed. net_apply reinstalls the defs table BEFORE reconciling, so
   the old direction is only available from old_def. */
void test_row16_an_eligibility_edit_folds_at_the_old_sign(void) {
    static const timer_def_t EDITED[TIMER_SLOT_COUNT] = {
        {"Screen", 0, false, false},  {"Piano", 900, true, true},
        {"", 0, false, false},        {"Laundry", 600, true, true}, /* HA flipped it to break-eligible */
        {"Violin", 900, false, true},
    };
    select_slot(SLOT_LAUNDRY);
    timer_start(T0, 3600); /* 600 s at the old (adding) sign */

    timer_set_defs(EDITED, TIMER_SLOT_COUNT);
    bool was_running = false;
    TEST_ASSERT_EQUAL(TIMER_RECONCILE_NONE, timer_reconcile_def(SLOT_LAUNDRY, &TEST_DEFS[SLOT_LAUNDRY],
                                                                &EDITED[SLOT_LAUNDRY], T0 + 600, &was_running));
    TEST_ASSERT_TRUE(was_running);
    TEST_ASSERT_EQUAL_INT32(600, g_rtc_state.slots[0].run_accum_sec); /* kept */
    TEST_ASSERT_EQUAL_INT32(300, timer_run_accum(T0 + 900));          /* now draining */
    timer_set_defs(TEST_DEFS, TIMER_SLOT_COUNT);
}

/* Row 17: idle neither adds nor drains. Deliberate — the device deep-
   sleeps whenever nothing runs, so decaying through idle would mean the
   balance almost never survives to reach the interval. */
void test_row17_idle_neither_adds_nor_drains(void) {
    timer_start(T0, 7200);
    timer_pause(T0 + 600);
    TEST_ASSERT_EQUAL_INT32(600, timer_run_accum(T0 + 600));
    TEST_ASSERT_EQUAL_INT32(600, timer_run_accum(T0 + 600 + 3600)); /* an hour */
    TEST_ASSERT_EQUAL_INT32(600, g_rtc_state.slots[0].run_accum_sec);
}

/* The motivating case (plan "The balance"): fold laundry 15 min, practise
   violin 15 min, then sit down to watch TV. The break must NOT fire
   immediately — it fires a full interval into the Screen run. */
void test_balance_motivating_case_laundry_then_violin_then_screen(void) {
    select_slot(SLOT_LAUNDRY);
    timer_start(T0, 3600);
    timer_pause(T0 + 900); /* +900 */
    select_slot(SLOT_VIOLIN);
    timer_start(T0 + 900, 3600);
    timer_pause(T0 + 1800); /* -900 -> 0 */
    TEST_ASSERT_EQUAL_INT32(0, timer_run_accum(T0 + 1800));

    select_slot(0);
    timer_start(T0 + 1800, 7200);
    TEST_ASSERT_FALSE(timer_break_due(T0 + 1800 + 1799, 1800));
    TEST_ASSERT_TRUE(timer_break_due(T0 + 1800 + 1800, 1800));
}

/* The simpler half of the sleep planning: while an ELIGIBLE timer runs
   the balance is falling, so there is no break moment to schedule at all.
   However long Violin runs, no break can become due from it. */
void test_an_eligible_run_never_makes_a_break_due(void) {
    timer_start(T0, 7200); /* Screen: balance up to just under the line */
    timer_pause(T0 + 1799);
    select_slot(SLOT_VIOLIN);
    timer_start(T0 + 1799, 7200);
    for (int32_t t = 0; t <= 3600; t += 60) {
        TEST_ASSERT_FALSE(timer_break_due(T0 + 1799 + t, 1800));
    }
}

/* Slot 0's terminal states are not a break target. Dropping the RUNNING
   requirement from timer_break_due exposes them: an EXPIRED Screen forced
   into BREAK comes back as IDLE (I7) and silently refunds the whole day's
   allocation. Reachable with a 60 min allocation and a 30 min interval —
   the break at 30 min resets the balance, and the second half of the run
   re-earns it exactly as the timer expires. */
void test_break_is_not_due_while_screen_holds_no_startable_time(void) {
    timer_start(T0, 1800);
    timer_tick(T0 + 1800); /* -> EXPIRED, balance folded at 1800 */
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_slot_state(0));
    TEST_ASSERT_EQUAL_INT32(1800, timer_run_accum(T0 + 1800));
    TEST_ASSERT_FALSE(timer_break_due(T0 + 1800, 1800));

    /* Self-healing: an HA grant gives Screen time again, and the break
       that was genuinely earned fires against it. */
    timer_adjust(0, 600);
    TEST_ASSERT_EQUAL(TIMER_PAUSED, timer_slot_state(0));
    TEST_ASSERT_TRUE(timer_break_due(T0 + 1800, 1800));
}

/* A break already running is never "due" again — the balance it reset
   can only be drained by the eligible timers it allows. */
void test_break_is_not_due_while_one_is_already_running(void) {
    timer_start(T0, 3600);
    timer_start_break(T0 + 1800, 900);
    TEST_ASSERT_FALSE(timer_break_due(T0 + 1800, 1800));
    TEST_ASSERT_EQUAL_INT32(0, timer_run_accum(T0 + 2000));
}

/* Outside a break every slot is startable — the gate is the break, not
   the flag. */
void test_start_allowed_outside_a_break_is_always_true(void) {
    TEST_ASSERT_TRUE(timer_start_allowed());
    select_slot(SLOT_LAUNDRY);
    TEST_ASSERT_TRUE(timer_start_allowed());
    TEST_ASSERT_EQUAL(BTN_A_STARTED, button_a_apply(T0));
}

/* The break screen's swap hint promises a timer you can actually start. */
void test_eligible_extra_count_counts_only_break_eligible_slots(void) {
    TEST_ASSERT_EQUAL_INT(3, timer_extra_count());          /* Piano, Laundry, Violin */
    TEST_ASSERT_EQUAL_INT(2, timer_eligible_extra_count()); /* Piano, Violin */
    TEST_ASSERT_FALSE(timer_slot_break_eligible(0));        /* Screen, always */
    TEST_ASSERT_TRUE(timer_slot_break_eligible(SLOT_PIANO));
    TEST_ASSERT_FALSE(timer_slot_break_eligible(SLOT_LAUNDRY));
    TEST_ASSERT_FALSE(timer_slot_break_eligible(2));  /* disabled */
    TEST_ASSERT_FALSE(timer_slot_break_eligible(99)); /* out of range */
}

/* The balance lives on slot 0 whichever slot drives it, so an NTP step
   must move SLOT 0's segment start — not the active slot's. */
void test_shift_expiry_moves_the_balance_segment_on_slot_zero(void) {
    select_slot(SLOT_LAUNDRY);
    timer_start(T0, 3600);
    timer_shift_expiry(120);
    TEST_ASSERT_EQUAL_INT32(600, timer_run_accum(T0 + 120 + 600));
}

/* A rename/disable edit ends the run, so its live segment must be folded
   too — otherwise slot 0's balance advances forever behind a slot that no
   longer exists (the G2 failure mode by another route). */
void test_reconcile_reset_folds_the_live_segment(void) {
    static const timer_def_t GONE = {"", 0, false, false};
    select_slot(SLOT_LAUNDRY);
    timer_start(T0, 3600);
    TEST_ASSERT_EQUAL(TIMER_RECONCILE_RESET,
                      timer_reconcile_def(SLOT_LAUNDRY, &TEST_DEFS[SLOT_LAUNDRY], &GONE, T0 + 600, NULL));
    TEST_ASSERT_EQUAL_INT32(600, g_rtc_state.slots[0].run_accum_sec);
    TEST_ASSERT_EQUAL_INT64(0, g_rtc_state.slots[0].run_started_wall);
    TEST_ASSERT_EQUAL_INT32(600, timer_run_accum(T0 + 9000));
}

/* ---- eye-rest: run-time accrual + TIMER_BREAK ---- */

void test_run_accum_counts_running_time(void) {
    timer_start(T0, 7200);
    TEST_ASSERT_EQUAL_INT32(600, timer_run_accum(T0 + 600));
}

void test_run_accum_excludes_pause_gaps(void) {
    timer_start(T0, 7200);
    timer_pause(T0 + 600);
    TEST_ASSERT_EQUAL_INT32(600, timer_run_accum(T0 + 9000)); /* frozen while paused */
    timer_resume(T0 + 9000);
    TEST_ASSERT_EQUAL_INT32(900, timer_run_accum(T0 + 9300));
}

void test_break_due_at_interval_survives_a_pause(void) {
    /* A break that has been EARNED cannot be dodged: the balance reaching
       the interval is the whole condition. Pausing holds it (no exposure,
       no movement) but does not un-earn it — the RUNNING requirement the
       predecessor had is gone, because slot 0 is routinely IDLE or PAUSED
       while a non-eligible extra drives the balance. */
    timer_start(T0, 7200);
    TEST_ASSERT_FALSE(timer_break_due(T0 + 1799, 1800));
    TEST_ASSERT_TRUE(timer_break_due(T0 + 1800, 1800));
    timer_pause(T0 + 1800);
    TEST_ASSERT_TRUE(timer_break_due(T0 + 2000, 1800)); /* still owed */
    timer_reset();
    TEST_ASSERT_FALSE(timer_break_due(T0 + 9999, 1800)); /* IDLE, balance 0 */
}

void test_start_break_freezes_timer_and_arms_break(void) {
    timer_start(T0, 3600);
    timer_start_break(T0 + 1800, 900);
    TEST_ASSERT_EQUAL(TIMER_BREAK, timer_get_state());
    TEST_ASSERT_EQUAL_INT32(1800, g_rtc_state.slots[0].remaining_at_pause);
    TEST_ASSERT_EQUAL_INT64(0, g_rtc_state.slots[0].expiry_wall_time);
    TEST_ASSERT_EQUAL_INT32(0, timer_run_accum(T0 + 1800)); /* accrual resets */
    TEST_ASSERT_EQUAL_INT64((int64_t)T0 + 2700, g_rtc_state.slots[0].break_expiry_wall);
}

void test_break_remaining_counts_down(void) {
    timer_start(T0, 3600);
    timer_start_break(T0 + 1800, 900);
    TEST_ASSERT_EQUAL_INT32(700, timer_break_remaining(T0 + 2000));
    timer_reset();
    TEST_ASSERT_EQUAL_INT32(0, timer_break_remaining(T0 + 2000)); /* not BREAK */
}

void test_tick_during_break_holds_then_transitions_to_paused(void) {
    timer_start(T0, 3600);
    timer_start_break(T0 + 1800, 900);
    TEST_ASSERT_EQUAL_INT32(1800, timer_tick(T0 + 2000)); /* frozen screen-time */
    TEST_ASSERT_EQUAL(TIMER_BREAK, timer_get_state());
    TEST_ASSERT_EQUAL_INT32(1800, timer_tick(T0 + 2700)); /* break over */
    TEST_ASSERT_EQUAL(TIMER_PAUSED, timer_get_state());
    TEST_ASSERT_EQUAL_INT64(0, g_rtc_state.slots[0].break_expiry_wall);
    timer_resume(T0 + 2700);
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
    TEST_ASSERT_EQUAL_INT32(1800, timer_tick(T0 + 2700));
}

void test_shift_expiry_shifts_run_started_wall_when_running(void) {
    timer_start(T0, 3600);
    timer_shift_expiry(120); /* clock stepped forward */
    /* accrual measured on the stepped clock stays correct */
    TEST_ASSERT_EQUAL_INT32(600, timer_run_accum(T0 + 120 + 600));
}

void test_shift_expiry_shifts_break_expiry_when_in_break(void) {
    timer_start(T0, 3600);
    timer_start_break(T0 + 100, 900);
    timer_shift_expiry(60);
    TEST_ASSERT_EQUAL_INT64((int64_t)T0 + 1000 + 60, g_rtc_state.slots[0].break_expiry_wall);
}

/* Snapshot make/restore: crash recovery — a panic wipes RTC memory, so the
   state is persisted to NVS and restored when the stored date is today. */

void test_make_snapshot_captures_running_state(void) {
    timer_start(T0, 3600);
    timer_record_date(T0);
    timer_snapshot_t snap;
    timer_make_snapshot(&snap);
    TEST_ASSERT_EQUAL_UINT8(TIMER_SNAPSHOT_VERSION, snap.version);
    TEST_ASSERT_EQUAL_UINT8(TIMER_RUNNING, snap.slots[0].state);
    TEST_ASSERT_EQUAL_INT64((int64_t)T0 + 3600, snap.slots[0].expiry_wall_time);
    TEST_ASSERT_EQUAL_INT32(3600, snap.slots[0].allocation_sec);
    TEST_ASSERT_EQUAL_STRING(g_rtc_state.last_date, snap.date);
}

void test_restore_snapshot_running_when_date_matches(void) {
    timer_start(T0, 3600);
    timer_record_date(T0);
    timer_snapshot_t snap;
    timer_make_snapshot(&snap);

    timer_reset(); /* simulate the RTC wipe */
    TEST_ASSERT_TRUE(timer_restore_snapshot(&snap, T0 + 500));
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
    TEST_ASSERT_EQUAL_INT64((int64_t)T0 + 3600, g_rtc_state.slots[0].expiry_wall_time);
    TEST_ASSERT_EQUAL_INT32(3600, g_rtc_state.slots[0].allocation_sec);
    TEST_ASSERT_EQUAL_STRING(snap.date, g_rtc_state.last_date);
    TEST_ASSERT_EQUAL_INT32(3100, timer_tick(T0 + 500)); /* countdown continues */
}

void test_restore_snapshot_paused_preserves_remaining(void) {
    timer_start(T0, 3600);
    timer_pause(T0 + 1000); /* remaining 2600 */
    timer_record_date(T0);
    timer_snapshot_t snap;
    timer_make_snapshot(&snap);

    timer_reset();
    TEST_ASSERT_TRUE(timer_restore_snapshot(&snap, T0 + 5000));
    TEST_ASSERT_EQUAL(TIMER_PAUSED, timer_get_state());
    TEST_ASSERT_EQUAL_INT32(2600, timer_tick(T0 + 5000));
}

void test_restore_snapshot_rejected_when_date_differs(void) {
    timer_start(T0, 3600);
    timer_record_date(T0);
    timer_snapshot_t snap;
    timer_make_snapshot(&snap);

    timer_reset();
    /* Next day: yesterday's snapshot must not refund or restore anything */
    TEST_ASSERT_FALSE(timer_restore_snapshot(&snap, T0 + 86400));
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state());
    TEST_ASSERT_EQUAL_INT64(0, g_rtc_state.slots[0].expiry_wall_time);
}

void test_restore_snapshot_rejected_on_version_mismatch(void) {
    timer_start(T0, 3600);
    timer_record_date(T0);
    timer_snapshot_t snap;
    timer_make_snapshot(&snap);
    snap.version = 99;

    timer_reset();
    TEST_ASSERT_FALSE(timer_restore_snapshot(&snap, T0 + 500));
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state());
}

void test_restore_snapshot_rejected_on_checksum_mismatch(void) {
    timer_start(T0, 3600);
    timer_record_date(T0);
    timer_snapshot_t snap;
    timer_make_snapshot(&snap);
    snap.slots[0].allocation_sec ^= 0x4; /* corrupt one field, checksum now stale */

    timer_reset();
    TEST_ASSERT_FALSE(timer_restore_snapshot(&snap, T0 + 500));
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state());
}

void test_restore_snapshot_rejected_on_invalid_state_enum(void) {
    timer_start(T0, 3600);
    timer_record_date(T0);
    timer_snapshot_t snap;
    timer_make_snapshot(&snap);
    snap.slots[0].state = 200;
    snap.checksum = timer_snapshot_checksum(&snap); /* checksum passes... */

    timer_reset();
    TEST_ASSERT_FALSE(timer_restore_snapshot(&snap, T0 + 500)); /* ...state check rejects */
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state());
}

void test_restore_snapshot_rejected_on_implausible_expiry(void) {
    timer_start(T0, 3600);
    timer_record_date(T0);
    timer_snapshot_t snap;
    timer_make_snapshot(&snap);
    snap.slots[0].expiry_wall_time = (int64_t)T0 + 30 * 86400; /* 30 days out — nonsense */
    snap.checksum = timer_snapshot_checksum(&snap);

    timer_reset();
    TEST_ASSERT_FALSE(timer_restore_snapshot(&snap, T0 + 500));
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state());
}

void test_restore_snapshot_rejected_all_zeros(void) {
    /* All-zeros XORs to 0 — indistinguishable from blank storage; reject */
    timer_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    TEST_ASSERT_FALSE(timer_restore_snapshot(&snap, T0));
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state());
}

void test_restore_snapshot_running_past_expiry_becomes_expired(void) {
    /* Power cut before the EXPIRED snapshot was saved: the stored state is
       RUNNING but the expiry passed while unplugged. Restoring as RUNNING
       would re-transition on the next tick and re-fire the alert — the
       moment already passed, so restore directly as EXPIRED (silent). */
    timer_start(T0, 100);
    timer_record_date(T0);
    timer_snapshot_t snap;
    timer_make_snapshot(&snap);

    timer_reset();
    TEST_ASSERT_TRUE(timer_restore_snapshot(&snap, T0 + 500));
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
    TEST_ASSERT_TRUE(timer_tick(T0 + 500) <= 0); /* stays expired, no transition */
}

void test_snapshot_restores_mid_break_with_same_end_time(void) {
    /* Power cycle mid-break must neither restart nor shorten the break */
    timer_start(T0, 3600);
    timer_start_break(T0 + 1800, 900);
    timer_record_date(T0);
    timer_snapshot_t snap;
    timer_make_snapshot(&snap);

    timer_reset();
    TEST_ASSERT_TRUE(timer_restore_snapshot(&snap, T0 + 2000));
    TEST_ASSERT_EQUAL(TIMER_BREAK, timer_get_state());
    TEST_ASSERT_EQUAL_INT64((int64_t)T0 + 2700, g_rtc_state.slots[0].break_expiry_wall);
    TEST_ASSERT_EQUAL_INT32(700, timer_break_remaining(T0 + 2000));
    TEST_ASSERT_EQUAL_INT32(1800, g_rtc_state.slots[0].remaining_at_pause);
}

void test_snapshot_restore_break_past_end_becomes_paused(void) {
    timer_start(T0, 3600);
    timer_start_break(T0 + 1800, 900);
    timer_record_date(T0);
    timer_snapshot_t snap;
    timer_make_snapshot(&snap);

    timer_reset();
    TEST_ASSERT_TRUE(timer_restore_snapshot(&snap, T0 + 3000)); /* past T0+2700 */
    TEST_ASSERT_EQUAL(TIMER_PAUSED, timer_get_state());
    TEST_ASSERT_EQUAL_INT64(0, g_rtc_state.slots[0].break_expiry_wall);
    TEST_ASSERT_EQUAL_INT32(1800, timer_tick(T0 + 3000));
}

void test_snapshot_restore_running_preserves_accrual(void) {
    timer_start(T0, 7200);
    timer_pause(T0 + 600);
    timer_resume(T0 + 9000);
    timer_record_date(T0);
    timer_snapshot_t snap;
    timer_make_snapshot(&snap);

    timer_reset();
    TEST_ASSERT_TRUE(timer_restore_snapshot(&snap, T0 + 9300));
    TEST_ASSERT_EQUAL_INT32(900, timer_run_accum(T0 + 9300));
}

void test_restore_snapshot_expired_state_restores(void) {
    /* Crash after expiry must not refund time: EXPIRED snapshot restores */
    timer_start(T0, 100);
    timer_tick(T0 + 200); /* -> EXPIRED */
    timer_record_date(T0);
    timer_snapshot_t snap;
    timer_make_snapshot(&snap);

    timer_reset();
    TEST_ASSERT_TRUE(timer_restore_snapshot(&snap, T0 + 500));
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
}

/* ---- multi-timer slots: selection ---- */

void test_default_active_slot_is_zero(void) {
    TEST_ASSERT_EQUAL_INT(0, timer_active_slot());
    TEST_ASSERT_NULL(timer_active_def()); /* slot 0 = Screen, schedule-fed */
}

void test_select_next_cycles_enabled_slots_skipping_disabled(void) {
    TEST_ASSERT_TRUE(timer_select_next()); /* 0 -> 1 (Piano) */
    TEST_ASSERT_EQUAL_INT(1, timer_active_slot());
    TEST_ASSERT_EQUAL_STRING("Piano", timer_active_def()->name);
    TEST_ASSERT_TRUE(timer_select_next()); /* 1 -> 3 (slot 2 disabled) */
    TEST_ASSERT_EQUAL_INT(3, timer_active_slot());
    TEST_ASSERT_TRUE(timer_select_next()); /* 3 -> 4 */
    TEST_ASSERT_EQUAL_INT(4, timer_active_slot());
    TEST_ASSERT_TRUE(timer_select_next()); /* 4 -> 0 (wrap) */
    TEST_ASSERT_EQUAL_INT(0, timer_active_slot());
}

void test_select_next_refused_while_running(void) {
    timer_start(T0, 3600);
    TEST_ASSERT_FALSE(timer_select_next());
    TEST_ASSERT_EQUAL_INT(0, timer_active_slot());
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
}

void test_select_next_allowed_during_break(void) {
    /* v1.4: the break enforces the SCREEN timer, not the whole device —
       Button C stays live so the kid can go and run Piano. */
    timer_start(T0, 3600);
    timer_start_break(T0 + 1800, 900);
    TEST_ASSERT_TRUE(timer_select_next());
    TEST_ASSERT_EQUAL_INT(1, timer_active_slot());
    TEST_ASSERT_EQUAL(TIMER_BREAK, timer_slot_state(0)); /* break unaffected */
}

void test_select_next_allowed_when_paused_or_expired(void) {
    timer_start(T0, 3600);
    timer_pause(T0 + 100);
    TEST_ASSERT_TRUE(timer_select_next());
    TEST_ASSERT_EQUAL_INT(1, timer_active_slot());
    timer_start(T0 + 200, 900);
    timer_tick(T0 + 2000); /* -> EXPIRED */
    TEST_ASSERT_TRUE(timer_select_next());
    TEST_ASSERT_EQUAL_INT(3, timer_active_slot());
}

void test_select_next_noop_without_extras(void) {
    timer_set_defs(NULL, 0);
    TEST_ASSERT_FALSE(timer_select_next());
    TEST_ASSERT_EQUAL_INT(0, timer_active_slot());
}

void test_slot_def_accessor(void) {
    TEST_ASSERT_NULL(timer_slot_def(0)); /* Screen has no def (schedule-fed) */
    TEST_ASSERT_NOT_NULL(timer_slot_def(1));
    TEST_ASSERT_EQUAL_STRING("Piano", timer_slot_def(1)->name);
    TEST_ASSERT_NULL(timer_slot_def(2));                /* disabled slot */
    TEST_ASSERT_NULL(timer_slot_def(TIMER_SLOT_COUNT)); /* out of range */
}

void test_slot_by_name(void) {
    TEST_ASSERT_EQUAL_INT(0, timer_slot_by_name("Screen"));
    TEST_ASSERT_EQUAL_INT(0, timer_slot_by_name(NULL)); /* default Screen */
    TEST_ASSERT_EQUAL_INT(0, timer_slot_by_name(""));
    TEST_ASSERT_EQUAL_INT(1, timer_slot_by_name("Piano"));
    TEST_ASSERT_EQUAL_INT(3, timer_slot_by_name("Laundry"));
    TEST_ASSERT_EQUAL_INT(4, timer_slot_by_name("Violin"));
    TEST_ASSERT_EQUAL_INT(-1, timer_slot_by_name("Guitar")); /* not configured */
}

void test_swap_allowed_tracks_state_and_extras(void) {
    /* Drives the Button C wake mask: C must not even wake the device when
       a press could only burn a full refresh (swap refused). */
    TEST_ASSERT_TRUE(timer_swap_allowed()); /* IDLE + extras */
    timer_start(T0, 3600);
    TEST_ASSERT_FALSE(timer_swap_allowed()); /* RUNNING */
    timer_pause(T0 + 100);
    TEST_ASSERT_TRUE(timer_swap_allowed()); /* PAUSED */
    timer_resume(T0 + 200);
    timer_start_break(T0 + 300, 900);
    TEST_ASSERT_TRUE(timer_swap_allowed()); /* BREAK no longer blocks the swap */
    timer_reset();
    timer_start(T0, 100);
    timer_tick(T0 + 200); /* -> EXPIRED */
    TEST_ASSERT_TRUE(timer_swap_allowed());
}

void test_swap_allowed_false_without_extras(void) {
    timer_set_defs(NULL, 0);
    TEST_ASSERT_FALSE(timer_swap_allowed());
}

void test_reload_allowed_reloadable_timer_except_running(void) {
    /* Drives the Button B wake mask: no ParentTesting needed on Piano */
    timer_select_next(); /* Piano (reloadable) */
    TEST_ASSERT_TRUE(timer_reload_allowed(false));
    timer_start(T0, 900);
    TEST_ASSERT_FALSE(timer_reload_allowed(false)); /* can't reset a running timer */
    TEST_ASSERT_FALSE(timer_reload_allowed(true));  /* not even in parent mode */
    timer_pause(T0 + 100);
    TEST_ASSERT_TRUE(timer_reload_allowed(false));
}

void test_reload_allowed_non_reloadable_needs_parent_testing(void) {
    TEST_ASSERT_FALSE(timer_reload_allowed(false)); /* Screen, production */
    TEST_ASSERT_TRUE(timer_reload_allowed(true));   /* Screen, parent mode */
    timer_start(T0, 3600);
    timer_start_break(T0 + 1800, 900);
    TEST_ASSERT_FALSE(timer_reload_allowed(false));
    TEST_ASSERT_TRUE(timer_reload_allowed(true)); /* parent escape from a break */
}

/* ---- multi-timer slots: per-slot independence ---- */

void test_slot_states_are_independent(void) {
    /* Run Piano down to 700 s, pause it, swap away, run Screen, swap back */
    timer_select_next(); /* -> Piano */
    timer_start(T0, 900);
    timer_pause(T0 + 200); /* Piano paused, 700 left */

    timer_select_next(); /* -> Laundry */
    timer_select_next(); /* -> Violin */
    timer_select_next(); /* -> Screen */
    TEST_ASSERT_EQUAL_INT(0, timer_active_slot());
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state()); /* Screen untouched */
    timer_start(T0 + 300, 3600);
    timer_pause(T0 + 400);

    timer_select_next(); /* -> Piano again */
    TEST_ASSERT_EQUAL(TIMER_PAUSED, timer_get_state());
    TEST_ASSERT_EQUAL_INT32(700, timer_tick(T0 + 9999)); /* frozen while paused */
}

void test_expiry_wall_accessor_tracks_active_slot(void) {
    timer_select_next(); /* Piano */
    timer_start(T0, 900);
    TEST_ASSERT_EQUAL_INT64((int64_t)T0 + 900, timer_expiry_wall());
    timer_pause(T0 + 100);
    timer_select_next(); /* Laundry, idle */
    TEST_ASSERT_EQUAL_INT64(0, timer_expiry_wall());
}

void test_break_never_due_on_extra_slot(void) {
    timer_select_next(); /* Piano */
    timer_start(T0, 900);
    /* Accrual passed the interval, but breaks are Screen-only */
    TEST_ASSERT_FALSE(timer_break_due(T0 + 800, 600));
}

/* ---- multi-timer slots: reload ---- */

void test_reload_refused_while_running(void) {
    timer_select_next(); /* Piano */
    timer_start(T0, 900);
    TEST_ASSERT_FALSE(timer_reload());
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
}

void test_reload_from_paused_returns_to_idle(void) {
    timer_select_next(); /* Piano */
    timer_start(T0, 900);
    timer_pause(T0 + 200);
    TEST_ASSERT_TRUE(timer_reload());
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state());
    TEST_ASSERT_EQUAL_INT64(0, timer_expiry_wall());
    TEST_ASSERT_EQUAL_INT32(0, g_rtc_state.slots[1].remaining_at_pause);
}

void test_reload_from_expired_returns_to_idle(void) {
    timer_select_next(); /* Piano */
    timer_start(T0, 900);
    timer_tick(T0 + 901); /* -> EXPIRED */
    TEST_ASSERT_TRUE(timer_reload());
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state());
}

void test_reload_only_touches_active_slot(void) {
    timer_start(T0, 3600); /* Screen running */
    timer_pause(T0 + 100);
    timer_select_next(); /* Piano */
    timer_start(T0 + 200, 900);
    timer_pause(T0 + 300);
    TEST_ASSERT_TRUE(timer_reload()); /* Piano -> IDLE */
    timer_select_next();
    timer_select_next();
    timer_select_next(); /* back to Screen */
    TEST_ASSERT_EQUAL(TIMER_PAUSED, timer_get_state());
    TEST_ASSERT_EQUAL_INT32(3500, timer_tick(T0 + 9999));
}

/* ---- multi-timer slots: completion counter ---- */

void test_completions_increment_on_expiry_only(void) {
    timer_select_next(); /* Piano */
    TEST_ASSERT_EQUAL_UINT16(0, timer_completions());
    timer_start(T0, 900);
    timer_tick(T0 + 901); /* -> EXPIRED */
    TEST_ASSERT_EQUAL_UINT16(1, timer_completions());
    timer_tick(T0 + 950); /* already EXPIRED: no double count */
    TEST_ASSERT_EQUAL_UINT16(1, timer_completions());
}

void test_reload_midway_does_not_increment_completions(void) {
    timer_select_next(); /* Piano */
    timer_start(T0, 900);
    timer_pause(T0 + 200);
    timer_reload();
    TEST_ASSERT_EQUAL_UINT16(0, timer_completions());
}

void test_reload_preserves_completions(void) {
    timer_select_next(); /* Piano */
    timer_start(T0, 900);
    timer_tick(T0 + 901); /* completion #1 */
    TEST_ASSERT_TRUE(timer_reload());
    TEST_ASSERT_EQUAL_UINT16(1, timer_completions());
    timer_start(T0 + 1000, 900);
    timer_tick(T0 + 1901); /* completion #2 */
    TEST_ASSERT_EQUAL_UINT16(2, timer_completions());
}

void test_completions_are_per_slot(void) {
    timer_select_next(); /* Piano */
    timer_start(T0, 900);
    timer_tick(T0 + 901);
    timer_select_next(); /* Laundry */
    TEST_ASSERT_EQUAL_UINT16(0, timer_completions());
}

void test_reset_clears_all_slots_and_reverts_to_screen(void) {
    timer_select_next(); /* Piano */
    timer_start(T0, 900);
    timer_tick(T0 + 901); /* completion */
    timer_reset();        /* day rollover */
    TEST_ASSERT_EQUAL_INT(0, timer_active_slot());
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state());
    TEST_ASSERT_EQUAL_UINT16(0, g_rtc_state.slots[1].completions);
    TEST_ASSERT_EQUAL(TIMER_IDLE, (timer_state_t)g_rtc_state.slots[1].state);
}

/* ---- snapshot v3: multi-slot round trip ---- */

void test_snapshot_v3_roundtrip_multi_slot(void) {
    timer_select_next(); /* Piano */
    timer_start(T0, 900);
    timer_tick(T0 + 901); /* Piano EXPIRED, 1 completion */
    timer_reload();
    timer_select_next();
    timer_select_next();
    timer_select_next(); /* -> Screen */
    timer_start(T0 + 1000, 3600);
    timer_record_date(T0);
    timer_snapshot_t snap;
    timer_make_snapshot(&snap);

    timer_reset();
    TEST_ASSERT_TRUE(timer_restore_snapshot(&snap, T0 + 1500));
    TEST_ASSERT_EQUAL_INT(0, timer_active_slot());
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
    TEST_ASSERT_EQUAL_INT32(3100, timer_tick(T0 + 1500));
    TEST_ASSERT_EQUAL_UINT16(1, g_rtc_state.slots[1].completions);
    TEST_ASSERT_EQUAL(TIMER_IDLE, (timer_state_t)g_rtc_state.slots[1].state);
}

void test_snapshot_restores_active_extra_slot(void) {
    timer_select_next(); /* Piano */
    timer_start(T0, 900);
    timer_pause(T0 + 200);
    timer_record_date(T0);
    timer_snapshot_t snap;
    timer_make_snapshot(&snap);

    timer_reset();
    TEST_ASSERT_TRUE(timer_restore_snapshot(&snap, T0 + 500));
    TEST_ASSERT_EQUAL_INT(1, timer_active_slot());
    TEST_ASSERT_EQUAL(TIMER_PAUSED, timer_get_state());
    TEST_ASSERT_EQUAL_INT32(700, timer_tick(T0 + 500));
}

void test_snapshot_restore_expired_while_off_increments_completions(void) {
    timer_select_next(); /* Piano */
    timer_start(T0, 900);
    timer_record_date(T0);
    timer_snapshot_t snap;
    timer_make_snapshot(&snap); /* saved as RUNNING */

    timer_reset();
    /* Expiry passed while powered off: restores EXPIRED and counts the run */
    TEST_ASSERT_TRUE(timer_restore_snapshot(&snap, T0 + 1000));
    TEST_ASSERT_EQUAL_INT(1, timer_active_slot());
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
    TEST_ASSERT_EQUAL_UINT16(1, timer_completions());
}

void test_snapshot_rejected_on_bad_active_slot(void) {
    timer_start(T0, 3600);
    timer_record_date(T0);
    timer_snapshot_t snap;
    timer_make_snapshot(&snap);
    snap.active_slot = TIMER_SLOT_COUNT; /* out of range */
    snap.checksum = timer_snapshot_checksum(&snap);

    timer_reset();
    TEST_ASSERT_FALSE(timer_restore_snapshot(&snap, T0 + 500));
    TEST_ASSERT_EQUAL_INT(0, timer_active_slot());
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state());
}

void test_snapshot_rejected_on_invalid_state_in_any_slot(void) {
    timer_start(T0, 3600);
    timer_record_date(T0);
    timer_snapshot_t snap;
    timer_make_snapshot(&snap);
    snap.slots[2].state = 200; /* non-active slot corrupt */
    snap.checksum = timer_snapshot_checksum(&snap);

    timer_reset();
    TEST_ASSERT_FALSE(timer_restore_snapshot(&snap, T0 + 500));
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state());
}

/* ---- gap tests (2026-07 review): pins, edges, and corruption ---- */

void test_reload_screen_slot_escapes_break_and_keeps_date(void) {
    /* Parent-mode B during an enforced break: the one break escape hatch */
    timer_record_date(T0);
    timer_start(T0, 3600);
    timer_start_break(T0 + 1800, 900);
    TEST_ASSERT_TRUE(timer_reload());
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state());
    TEST_ASSERT_EQUAL_INT64(0, g_rtc_state.slots[0].break_expiry_wall);
    TEST_ASSERT_EQUAL_INT32(0, g_rtc_state.slots[0].remaining_at_pause);
    TEST_ASSERT_EQUAL_INT32(0, timer_run_accum(T0 + 2000));
    /* last_date untouched — reload must never fake a day rollover */
    TEST_ASSERT_FALSE(timer_is_new_day(T0 + 3600));
}

void test_set_defs_count_clamped_to_slot_count(void) {
    timer_set_defs(TEST_DEFS, 99); /* must not read past the table */
    TEST_ASSERT_EQUAL_INT(3, timer_extra_count());
}

void test_set_defs_shorter_table_disables_missing_slots(void) {
    timer_set_defs(TEST_DEFS, 2); /* only Screen + Piano visible */
    TEST_ASSERT_EQUAL_INT(1, timer_extra_count());
    TEST_ASSERT_TRUE(timer_select_next());
    TEST_ASSERT_EQUAL_INT(1, timer_active_slot());
    TEST_ASSERT_TRUE(timer_select_next()); /* wraps straight back to Screen */
    TEST_ASSERT_EQUAL_INT(0, timer_active_slot());
}

void test_is_new_day_tracks_local_date_across_dst(void) {
    /* is_new_day compares LOCAL calendar dates, not 24 h spans — pin that
       on both DST transitions (device TZ is US Eastern). */
    char *old_tz = getenv("TZ");
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    tzset();

    /* Fall-back day (2026-11-01) is 25 h long: 24 h after 00:30 EDT it is
       still Nov 1 (23:30 EST); the new day arrives at the 25 h mark. */
    struct tm tm = {0};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 10; /* November */
    tm.tm_mday = 1;
    tm.tm_min = 30;
    tm.tm_isdst = -1;
    time_t fall = mktime(&tm);
    timer_record_date(fall);
    TEST_ASSERT_FALSE(timer_is_new_day(fall + 24 * 3600));
    TEST_ASSERT_TRUE(timer_is_new_day(fall + 25 * 3600));

    /* Spring-forward day (2026-03-08) is 23 h long: the new local date
       already arrives 23 h after 00:30 EST. */
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 2; /* March */
    tm.tm_mday = 8;
    tm.tm_min = 30;
    tm.tm_isdst = -1;
    time_t spring = mktime(&tm);
    timer_record_date(spring);
    TEST_ASSERT_TRUE(timer_is_new_day(spring + 23 * 3600));

    if (old_tz)
        setenv("TZ", old_tz, 1);
    else
        unsetenv("TZ");
    tzset();
}

void test_start_with_zero_allocation_expires_on_first_tick(void) {
    /* Callers guard (Kconfig range >= 1 min; slot_enabled needs duration>0);
       pin the fallback: no hang, no negative-duration weirdness. */
    timer_start(T0, 0);
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
    TEST_ASSERT_TRUE(timer_tick(T0) <= 0);
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
}

void test_tick_with_clock_stepped_backwards_stays_running(void) {
    /* NTP steps the clock back without a matching timer_shift_expiry:
       remaining exceeds the allocation (bar clamps at full), no expiry. */
    timer_start(T0, 3600);
    TEST_ASSERT_EQUAL_INT32(4100, timer_tick(T0 - 500));
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
}

void test_completions_saturate_at_uint16_max(void) {
    timer_select_next(); /* Piano */
    g_rtc_state.slots[1].completions = UINT16_MAX;
    timer_start(T0, 900);
    timer_tick(T0 + 901); /* must not wrap to 0 and erase the history */
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, timer_completions());
}

static const timer_def_t DEFS_NO_EXTRAS[TIMER_SLOT_COUNT] = {
    {"Screen", 0, false, false}, {"", 0, false, false}, {"", 0, false, false},
    {"", 0, false, false},       {"", 0, false, false},
};

void test_snapshot_restore_falls_back_when_active_slot_disabled(void) {
    /* Reflashing with a slot removed from menuconfig must not strand the
       device on a dead slot the buttons can no longer leave. */
    timer_select_next(); /* Piano */
    timer_start(T0, 900);
    timer_pause(T0 + 200);
    timer_record_date(T0);
    timer_snapshot_t snap;
    timer_make_snapshot(&snap);

    timer_reset();
    timer_set_defs(DEFS_NO_EXTRAS, TIMER_SLOT_COUNT); /* Piano gone */
    TEST_ASSERT_TRUE(timer_restore_snapshot(&snap, T0 + 500));
    TEST_ASSERT_EQUAL_INT(0, timer_active_slot());
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state()); /* Screen was idle */
}

/* ---- display-remaining + screen-used primitives (shared, pure) ---- */

void test_slot_remaining_per_state(void) {
    /* RUNNING: expiry - now */
    timer_start(T0, 3600);
    TEST_ASSERT_EQUAL_INT32(3000, timer_slot_remaining(0, T0 + 600, 3600));
    /* RUNNING past expiry clamps to 0, never negative */
    TEST_ASSERT_EQUAL_INT32(0, timer_slot_remaining(0, T0 + 5000, 3600));
    /* PAUSED: the frozen remaining, now irrelevant */
    timer_pause(T0 + 600);
    TEST_ASSERT_EQUAL_INT32(3000, timer_slot_remaining(0, T0 + 99999, 3600));
    /* IDLE: the caller's fallback (allocation for a full bar) */
    timer_reset();
    TEST_ASSERT_EQUAL_INT32(3600, timer_slot_remaining(0, T0, 3600));
    /* EXPIRED: 0 */
    timer_start(T0, 100);
    timer_tick(T0 + 200);
    TEST_ASSERT_EQUAL_INT32(0, timer_slot_remaining(0, T0 + 200, 100));
}

void test_slot_remaining_reads_the_named_slot(void) {
    timer_select_next(); /* Piano active */
    timer_start(T0, 900);
    timer_pause(T0 + 100); /* Piano paused, 800 left */
    /* Screen (slot 0) is still IDLE → fallback; Piano (slot 1) → 800 */
    TEST_ASSERT_EQUAL_INT32(1234, timer_slot_remaining(0, T0 + 100, 1234));
    TEST_ASSERT_EQUAL_INT32(800, timer_slot_remaining(1, T0 + 100, 900));
}

void test_screen_used_sec_per_state(void) {
    /* Never started: 0 used (IDLE, allocation still 0) */
    TEST_ASSERT_EQUAL_INT32(0, timer_screen_used_sec(T0));
    /* RUNNING 600 s into a 3600 s allocation → 600 used */
    timer_start(T0, 3600);
    TEST_ASSERT_EQUAL_INT32(600, timer_screen_used_sec(T0 + 600));
    /* PAUSED after 1000 s → 1000 used */
    timer_pause(T0 + 1000);
    TEST_ASSERT_EQUAL_INT32(1000, timer_screen_used_sec(T0 + 9999));
    /* EXPIRED → the whole allocation counts as used */
    timer_reset();
    timer_start(T0, 100);
    timer_tick(T0 + 200);
    TEST_ASSERT_EQUAL_INT32(100, timer_screen_used_sec(T0 + 200));
}

void test_screen_used_sec_is_slot_zero_only(void) {
    /* An extra timer running must not affect Screen's used-today */
    timer_select_next(); /* Piano */
    timer_start(T0, 900);
    TEST_ASSERT_EQUAL_INT32(0, timer_screen_used_sec(T0 + 300)); /* Screen never ran */
}

/* ---- HA grant: extra time from Home Assistant (phase 3) ---- */

void test_grant_idle_banks_bonus_realized_at_start(void) {
    /* Screen idle: a grant banks bonus, consumed when the timer starts */
    timer_adjust(0, 900);
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state()); /* still idle */
    TEST_ASSERT_EQUAL_INT32(900, g_rtc_state.slots[0].bonus_sec);
    timer_start(T0, 3600); /* base alloc 3600 + 900 bonus */
    TEST_ASSERT_EQUAL_INT32(4500, g_rtc_state.slots[0].allocation_sec);
    TEST_ASSERT_EQUAL_INT32(0, g_rtc_state.slots[0].bonus_sec); /* consumed */
    TEST_ASSERT_EQUAL_INT32(4500, timer_tick(T0));
}

void test_grant_running_extends_expiry_and_allocation(void) {
    timer_start(T0, 3600);
    timer_adjust(0, 600);
    TEST_ASSERT_EQUAL_INT64((int64_t)T0 + 4200, g_rtc_state.slots[0].expiry_wall_time);
    TEST_ASSERT_EQUAL_INT32(4200, g_rtc_state.slots[0].allocation_sec);
    TEST_ASSERT_EQUAL_INT32(4200, timer_tick(T0));
}

void test_grant_paused_extends_remaining(void) {
    timer_start(T0, 3600);
    timer_pause(T0 + 1000); /* 2600 left */
    timer_adjust(0, 400);
    TEST_ASSERT_EQUAL_INT32(3000, g_rtc_state.slots[0].remaining_at_pause);
    TEST_ASSERT_EQUAL_INT32(4000, g_rtc_state.slots[0].allocation_sec);
}

void test_grant_expired_becomes_paused_holding_grant(void) {
    /* The chores-done case: time already ran out, +15 min → PAUSED,
       press A to use it (never auto-RUNNING, alert never re-fires). */
    timer_start(T0, 100);
    timer_tick(T0 + 200); /* EXPIRED */
    timer_adjust(0, 900);
    TEST_ASSERT_EQUAL(TIMER_PAUSED, timer_get_state());
    TEST_ASSERT_EQUAL_INT32(900, g_rtc_state.slots[0].remaining_at_pause);
    timer_resume(T0 + 300);
    TEST_ASSERT_EQUAL_INT32(900, timer_tick(T0 + 300));
}

void test_grant_targets_named_non_active_slot(void) {
    /* Active = Screen; grant Piano (slot 1) while it sits idle */
    timer_adjust(1, 600);
    TEST_ASSERT_EQUAL_INT(0, timer_active_slot()); /* selection unchanged */
    TEST_ASSERT_EQUAL_INT32(600, g_rtc_state.slots[1].bonus_sec);
    TEST_ASSERT_EQUAL_INT32(0, g_rtc_state.slots[0].bonus_sec); /* Screen untouched */
    timer_select_next();                                        /* -> Piano */
    timer_start(T0, 900);                                       /* 900 + 600 bonus */
    TEST_ASSERT_EQUAL_INT32(1500, g_rtc_state.slots[1].allocation_sec);
}

void test_grant_break_extends_frozen_screen_time(void) {
    timer_start(T0, 3600);
    timer_start_break(T0 + 1800, 900); /* 1800 screen-time frozen */
    timer_adjust(0, 300);
    TEST_ASSERT_EQUAL(TIMER_BREAK, timer_get_state()); /* break intact */
    TEST_ASSERT_EQUAL_INT32(2100, g_rtc_state.slots[0].remaining_at_pause);
}

void test_snapshot_v4_round_trips_bonus(void) {
    timer_adjust(1, 600); /* Piano idle bonus */
    timer_start(T0, 3600);
    timer_record_date(T0);
    timer_snapshot_t snap;
    timer_make_snapshot(&snap);
    TEST_ASSERT_EQUAL_UINT8(TIMER_SNAPSHOT_VERSION, snap.version);

    timer_reset();
    TEST_ASSERT_TRUE(timer_restore_snapshot(&snap, T0 + 100));
    TEST_ASSERT_EQUAL_INT32(600, g_rtc_state.slots[1].bonus_sec);
}

/* ---- HA idempotent "bonus minutes today" reconcile (Phase C) ---- */

void test_bonus_reconcile_grants_only_the_delta(void) {
    /* Target 15 min on an IDLE Screen: grants 900, banked as bonus */
    timer_bonus_reconcile(0, 900);
    TEST_ASSERT_EQUAL_INT32(900, g_rtc_state.slots[0].bonus_sec);
    TEST_ASSERT_EQUAL_INT32(900, g_rtc_state.slots[0].bonus_applied);
    /* Same target again: no-op (idempotent across wakes) */
    timer_bonus_reconcile(0, 900);
    TEST_ASSERT_EQUAL_INT32(900, g_rtc_state.slots[0].bonus_sec);
    /* Raise target to 20 min: grant only the extra 5 min */
    timer_bonus_reconcile(0, 1200);
    TEST_ASSERT_EQUAL_INT32(1200, g_rtc_state.slots[0].bonus_sec);
    TEST_ASSERT_EQUAL_INT32(1200, g_rtc_state.slots[0].bonus_applied);
}

void test_bonus_reconcile_lowering_target_reclaims_delta(void) {
    timer_bonus_reconcile(0, 900);
    timer_bonus_reconcile(0, 300); /* chores not done: take back 10 min */
    TEST_ASSERT_EQUAL_INT32(300, g_rtc_state.slots[0].bonus_applied);
    TEST_ASSERT_EQUAL_INT32(300, g_rtc_state.slots[0].bonus_sec);
}

void test_bonus_reconcile_negative_target_applies_delta_once(void) {
    /* +15 in the morning, -5 in the afternoon: the -20 delta lands
       exactly once; retained-message replays are no-ops. */
    timer_start(T0, 3600);
    timer_bonus_reconcile(0, 900); /* +15 min */
    TEST_ASSERT_EQUAL_INT32(4500, g_rtc_state.slots[0].allocation_sec);
    timer_bonus_reconcile(0, -300); /* down to -5 min */
    TEST_ASSERT_EQUAL_INT64((int64_t)T0 + 3300, g_rtc_state.slots[0].expiry_wall_time);
    TEST_ASSERT_EQUAL_INT32(3300, g_rtc_state.slots[0].allocation_sec);
    TEST_ASSERT_EQUAL_INT32(-300, g_rtc_state.slots[0].bonus_applied);
    timer_bonus_reconcile(0, -300); /* replay */
    TEST_ASSERT_EQUAL_INT32(3300, g_rtc_state.slots[0].allocation_sec);
}

void test_bonus_reconcile_stale_replay_across_rollover_is_noop(void) {
    /* Rollover ordering: the stale retained target reconciles BEFORE the
       day reset (delta 0), the broker is cleared to "0", and next window
       "0" meets bonus_applied 0 — nothing re-applies. */
    timer_bonus_reconcile(0, -300);
    TEST_ASSERT_EQUAL_INT32(-300, g_rtc_state.slots[0].bonus_sec);
    timer_bonus_reconcile(0, -300); /* stale replay, pre-reset */
    TEST_ASSERT_EQUAL_INT32(-300, g_rtc_state.slots[0].bonus_sec);
    timer_reset();               /* day rollover */
    timer_bonus_reconcile(0, 0); /* cleared broker value */
    TEST_ASSERT_EQUAL_INT32(0, g_rtc_state.slots[0].bonus_sec);
    TEST_ASSERT_EQUAL_INT32(0, g_rtc_state.slots[0].bonus_applied);
}

/* ---- signed timer_adjust: negative = time lost ---- */

void test_adjust_running_deducts_expiry_and_allocation(void) {
    timer_start(T0, 3600);
    timer_adjust(0, -600);
    TEST_ASSERT_EQUAL_INT64((int64_t)T0 + 3000, g_rtc_state.slots[0].expiry_wall_time);
    TEST_ASSERT_EQUAL_INT32(3000, g_rtc_state.slots[0].allocation_sec);
    TEST_ASSERT_EQUAL_INT32(3000, timer_tick(T0));
}

void test_adjust_running_past_zero_expires_on_next_tick(void) {
    timer_start(T0, 600);
    timer_adjust(0, -900);                               /* deduction exceeds remaining */
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state()); /* until the tick */
    timer_tick(T0 + 1);
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
}

void test_adjust_paused_past_zero_expires(void) {
    /* Same contract as timer_reconcile_def: a deduction that empties a
       paused timer expires it (honest TIME'S UP, not a 0:00 pause). */
    timer_start(T0, 3600);
    timer_pause(T0 + 1000); /* 2600 left */
    timer_adjust(0, -3000);
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
    TEST_ASSERT_EQUAL_INT32(0, g_rtc_state.slots[0].remaining_at_pause);
}

void test_adjust_paused_partial_deduction_stays_paused(void) {
    timer_start(T0, 3600);
    timer_pause(T0 + 1000); /* 2600 left */
    timer_adjust(0, -600);
    TEST_ASSERT_EQUAL(TIMER_PAUSED, timer_get_state());
    TEST_ASSERT_EQUAL_INT32(2000, g_rtc_state.slots[0].remaining_at_pause);
    TEST_ASSERT_EQUAL_INT32(3000, g_rtc_state.slots[0].allocation_sec);
}

void test_adjust_break_clamps_frozen_remaining(void) {
    /* BREAK stays intact — the deduction lands on the frozen screen
       time; an emptied timer expires on the post-break resume tick. */
    timer_start(T0, 3600);
    timer_start_break(T0 + 1800, 900); /* 1800 frozen */
    timer_adjust(0, -2500);
    TEST_ASSERT_EQUAL(TIMER_BREAK, timer_get_state());
    TEST_ASSERT_EQUAL_INT32(0, g_rtc_state.slots[0].remaining_at_pause);
}

void test_adjust_expired_negative_is_noop(void) {
    timer_start(T0, 100);
    timer_tick(T0 + 200); /* EXPIRED */
    timer_adjust(0, -600);
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
    TEST_ASSERT_EQUAL_INT32(100, g_rtc_state.slots[0].allocation_sec);
}

void test_adjust_idle_banks_negative_start_clamps_at_zero(void) {
    timer_adjust(0, -7200); /* deduction beyond the whole allocation */
    TEST_ASSERT_EQUAL_INT32(-7200, g_rtc_state.slots[0].bonus_sec);
    timer_start(T0, 3600); /* 3600 - 7200 clamps to 0 */
    TEST_ASSERT_EQUAL_INT32(0, g_rtc_state.slots[0].allocation_sec);
    timer_tick(T0 + 1);
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
}

void test_snapshot_round_trips_negative_bonus(void) {
    timer_adjust(1, -600); /* Piano idle: banked deduction */
    timer_bonus_reconcile(0, -300);
    timer_record_date(T0);
    timer_snapshot_t snap;
    timer_make_snapshot(&snap);

    timer_reset();
    TEST_ASSERT_TRUE(timer_restore_snapshot(&snap, T0 + 100));
    TEST_ASSERT_EQUAL_INT32(-600, g_rtc_state.slots[1].bonus_sec);
    TEST_ASSERT_EQUAL_INT32(-300, g_rtc_state.slots[0].bonus_applied);
}

void test_bonus_applied_resets_at_rollover(void) {
    timer_bonus_reconcile(0, 900);
    timer_reset(); /* day rollover */
    TEST_ASSERT_EQUAL_INT32(0, g_rtc_state.slots[0].bonus_applied);
}

void test_bonus_applied_survives_snapshot_roundtrip(void) {
    timer_bonus_reconcile(0, 900);
    timer_start(T0, 3600); /* consumes bonus_sec into allocation */
    timer_record_date(T0);
    timer_snapshot_t snap;
    timer_make_snapshot(&snap);
    TEST_ASSERT_EQUAL_UINT8(TIMER_SNAPSHOT_VERSION, snap.version);

    timer_reset();
    TEST_ASSERT_TRUE(timer_restore_snapshot(&snap, T0 + 100));
    TEST_ASSERT_EQUAL_INT32(900, g_rtc_state.slots[0].bonus_applied);
    /* After restore, re-reconciling the same target is a no-op */
    timer_bonus_reconcile(0, 900);
    TEST_ASSERT_EQUAL_INT32(4500, g_rtc_state.slots[0].allocation_sec); /* not re-granted */
}

/* ---- HA config reconcile of a redefined running/paused timer ---- */

/* The active-slot def as it was before the network window (Piano, 15 min,
   reloadable — matches TEST_DEFS slot 1). */
static const timer_def_t RECON_OLD = {"Piano", 900, true, true};

void test_reconcile_rename_running_resets_with_was_running(void) {
    timer_select_next(); /* Piano */
    timer_start(T0, 900);
    timer_def_t renamed = {"Guitar", 900, true, true};
    bool was_running = false;
    TEST_ASSERT_EQUAL(TIMER_RECONCILE_RESET, timer_reconcile_def(1, &RECON_OLD, &renamed, T0 + 100, &was_running));
    TEST_ASSERT_TRUE(was_running);
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state());
    TEST_ASSERT_EQUAL_INT64(0, g_rtc_state.slots[1].expiry_wall_time);
    TEST_ASSERT_EQUAL_INT32(0, g_rtc_state.slots[1].remaining_at_pause);
}

void test_reconcile_rename_paused_resets_without_was_running(void) {
    timer_select_next();
    timer_start(T0, 900);
    timer_pause(T0 + 100);
    timer_def_t renamed = {"Guitar", 900, true, true};
    bool was_running = true;
    TEST_ASSERT_EQUAL(TIMER_RECONCILE_RESET, timer_reconcile_def(1, &RECON_OLD, &renamed, T0 + 200, &was_running));
    TEST_ASSERT_FALSE(was_running);
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state());
}

void test_reconcile_rename_preserves_completions(void) {
    timer_select_next();
    g_rtc_state.slots[1].completions = 3;
    timer_start(T0, 900);
    timer_def_t renamed = {"Guitar", 900, true, true};
    TEST_ASSERT_EQUAL(TIMER_RECONCILE_RESET, timer_reconcile_def(1, &RECON_OLD, &renamed, T0 + 100, NULL));
    TEST_ASSERT_EQUAL_UINT16(3, timer_completions());
}

void test_reconcile_disable_resets_like_rename(void) {
    timer_select_next();
    timer_start(T0, 900);
    timer_def_t disabled = {"", 0, false, false};
    bool was_running = false;
    TEST_ASSERT_EQUAL(TIMER_RECONCILE_RESET, timer_reconcile_def(1, &RECON_OLD, &disabled, T0 + 100, &was_running));
    TEST_ASSERT_TRUE(was_running);
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state());
}

void test_reconcile_duration_grow_running_delta_shifts_expiry(void) {
    timer_select_next();
    timer_start(T0, 900); /* expiry T0+900 */
    timer_def_t grown = {"Piano", 1200, true, true};
    TEST_ASSERT_EQUAL(TIMER_RECONCILE_UPDATED, timer_reconcile_def(1, &RECON_OLD, &grown, T0 + 300, NULL));
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
    TEST_ASSERT_EQUAL_INT64((int64_t)T0 + 1200, g_rtc_state.slots[1].expiry_wall_time);
    TEST_ASSERT_EQUAL_INT32(1200, g_rtc_state.slots[1].allocation_sec);
    /* The run SEGMENT is untouched by a duration edit — and it lives on
       slot 0, which owns the exposure balance whatever is running. */
    TEST_ASSERT_EQUAL_INT64((int64_t)T0, g_rtc_state.slots[0].run_started_wall);
}

void test_reconcile_duration_grow_after_pause_resume_history(void) {
    /* run_started_wall marks the current SEGMENT (reset on resume) — a
       naive run_started + new_duration would credit back the 300 s that
       elapsed before the pause. Delta-shift must not. */
    timer_select_next();
    timer_start(T0, 900);    /* expiry T0+900 */
    timer_pause(T0 + 300);   /* remaining 600 */
    timer_resume(T0 + 1000); /* expiry T0+1600, run_started T0+1000 */
    timer_def_t grown = {"Piano", 1200, true, true};
    TEST_ASSERT_EQUAL(TIMER_RECONCILE_UPDATED, timer_reconcile_def(1, &RECON_OLD, &grown, T0 + 1100, NULL));
    /* +300 delta on the real expiry — NOT T0+1000+1200 = T0+2200 */
    TEST_ASSERT_EQUAL_INT64((int64_t)T0 + 1900, g_rtc_state.slots[1].expiry_wall_time);
}

void test_reconcile_duration_shrink_past_elapsed_expires(void) {
    timer_select_next();
    timer_start(T0, 900);
    timer_def_t shrunk = {"Piano", 600, true, true};
    bool was_running = false;
    /* 700 s elapsed >= new 600 s duration: the run is over */
    TEST_ASSERT_EQUAL(TIMER_RECONCILE_EXPIRED, timer_reconcile_def(1, &RECON_OLD, &shrunk, T0 + 700, &was_running));
    TEST_ASSERT_TRUE(was_running);
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
    TEST_ASSERT_EQUAL_UINT16(1, timer_completions()); /* reached 00:00 — counts */
}

void test_reconcile_duration_shrink_running_still_ahead_updates(void) {
    timer_select_next();
    timer_start(T0, 900);
    timer_def_t shrunk = {"Piano", 600, true, true};
    TEST_ASSERT_EQUAL(TIMER_RECONCILE_UPDATED, timer_reconcile_def(1, &RECON_OLD, &shrunk, T0 + 100, NULL));
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
    TEST_ASSERT_EQUAL_INT64((int64_t)T0 + 600, g_rtc_state.slots[1].expiry_wall_time);
    TEST_ASSERT_EQUAL_INT32(600, g_rtc_state.slots[1].allocation_sec);
}

void test_reconcile_duration_grow_paused_extends_remaining(void) {
    timer_select_next();
    timer_start(T0, 900);
    timer_pause(T0 + 300); /* remaining 600 */
    timer_def_t grown = {"Piano", 1200, true, true};
    TEST_ASSERT_EQUAL(TIMER_RECONCILE_UPDATED, timer_reconcile_def(1, &RECON_OLD, &grown, T0 + 400, NULL));
    TEST_ASSERT_EQUAL(TIMER_PAUSED, timer_get_state());
    TEST_ASSERT_EQUAL_INT32(900, g_rtc_state.slots[1].remaining_at_pause);
    TEST_ASSERT_EQUAL_INT32(1200, g_rtc_state.slots[1].allocation_sec);
}

void test_reconcile_duration_shrink_paused_past_elapsed_expires(void) {
    timer_select_next();
    timer_start(T0, 900);
    timer_pause(T0 + 300);                           /* elapsed 300, remaining 600 */
    timer_def_t shrunk = {"Piano", 240, true, true}; /* 300 elapsed >= 240 */
    bool was_running = true;
    TEST_ASSERT_EQUAL(TIMER_RECONCILE_EXPIRED, timer_reconcile_def(1, &RECON_OLD, &shrunk, T0 + 400, &was_running));
    TEST_ASSERT_FALSE(was_running);
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
    TEST_ASSERT_EQUAL_INT32(0, g_rtc_state.slots[1].remaining_at_pause); /* clamped */
    TEST_ASSERT_EQUAL_UINT16(1, timer_completions());
}

void test_reconcile_duration_shift_preserves_granted_time(void) {
    /* An HA grant already extended allocation+expiry: the duration delta
       must move both without erasing the grant. */
    timer_select_next();
    timer_start(T0, 900);
    timer_adjust(1, 300); /* allocation 1200, expiry T0+1200 */
    timer_def_t shrunk = {"Piano", 600, true, true};
    TEST_ASSERT_EQUAL(TIMER_RECONCILE_UPDATED, timer_reconcile_def(1, &RECON_OLD, &shrunk, T0 + 100, NULL));
    TEST_ASSERT_EQUAL_INT64((int64_t)T0 + 900, g_rtc_state.slots[1].expiry_wall_time);
    TEST_ASSERT_EQUAL_INT32(900, g_rtc_state.slots[1].allocation_sec); /* grant kept */
}

void test_reconcile_reload_flag_only_is_none(void) {
    timer_select_next();
    timer_start(T0, 900);
    timer_def_t reload_off = {"Piano", 900, false, true};
    TEST_ASSERT_EQUAL(TIMER_RECONCILE_NONE, timer_reconcile_def(1, &RECON_OLD, &reload_off, T0 + 100, NULL));
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
    TEST_ASSERT_EQUAL_INT64((int64_t)T0 + 900, g_rtc_state.slots[1].expiry_wall_time);
}

void test_reconcile_identical_def_is_none(void) {
    timer_select_next();
    timer_start(T0, 900);
    timer_def_t same = {"Piano", 900, true, true};
    TEST_ASSERT_EQUAL(TIMER_RECONCILE_NONE, timer_reconcile_def(1, &RECON_OLD, &same, T0 + 100, NULL));
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
}

void test_reconcile_screen_slot_exempt(void) {
    timer_start(T0, 3600); /* Screen running */
    timer_def_t a = {"Screen", 3600, false, false};
    timer_def_t b = {"Screen", 600, false, false};
    TEST_ASSERT_EQUAL(TIMER_RECONCILE_NONE, timer_reconcile_def(0, &a, &b, T0 + 100, NULL));
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
    TEST_ASSERT_EQUAL_INT64((int64_t)T0 + 3600, g_rtc_state.slots[0].expiry_wall_time);
}

void test_reconcile_non_active_paused_slot(void) {
    /* Field corner case: Violin paused at 10 min remaining, user swaps back
       to Screen, HA shrinks the def to 2 min. The reconcile must fix the
       frozen remaining on the NON-ACTIVE slot too — resuming later must not
       run the stale 10 minutes. */
    timer_select_next(); /* Piano (slot 1) */
    timer_start(T0, 900);
    timer_pause(T0 + 60);        /* remaining 840 */
    g_rtc_state.active_slot = 0; /* back on Screen; Piano stays PAUSED */
    timer_def_t shrunk = {"Piano", 120, true, true};
    TEST_ASSERT_EQUAL(TIMER_RECONCILE_UPDATED, timer_reconcile_def(1, &RECON_OLD, &shrunk, T0 + 100, NULL));
    TEST_ASSERT_EQUAL(TIMER_PAUSED, g_rtc_state.slots[1].state);
    TEST_ASSERT_EQUAL_INT32(60, g_rtc_state.slots[1].remaining_at_pause); /* 840 + (120-900) */
    TEST_ASSERT_EQUAL_INT32(120, g_rtc_state.slots[1].allocation_sec);
    /* the active Screen slot is untouched */
    TEST_ASSERT_EQUAL_INT(0, timer_active_slot());
    TEST_ASSERT_EQUAL(TIMER_IDLE, g_rtc_state.slots[0].state);
}

void test_reconcile_idle_and_expired_slots_are_none(void) {
    timer_select_next(); /* Piano, IDLE */
    timer_def_t renamed = {"Guitar", 600, true, true};
    TEST_ASSERT_EQUAL(TIMER_RECONCILE_NONE, timer_reconcile_def(1, &RECON_OLD, &renamed, T0, NULL));
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state());

    timer_start(T0, 900);
    timer_tick(T0 + 901); /* EXPIRED */
    TEST_ASSERT_EQUAL(TIMER_RECONCILE_NONE, timer_reconcile_def(1, &RECON_OLD, &renamed, T0 + 902, NULL));
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
}

/* ------------------------------------------------------------------ */
/* NTP bookkeeping: next_ntp_sync is the single RTC source; the last-  */
/* sync time shown on screen is derived, not stored twice.             */
/* ------------------------------------------------------------------ */

void test_last_ntp_sync_zero_when_never_synced(void) {
    TEST_ASSERT_EQUAL_INT64(0, (int64_t)timer_last_ntp_sync());
}

void test_last_ntp_sync_derived_from_record(void) {
    timer_record_ntp_sync(T0 + 1234);
    TEST_ASSERT_EQUAL_INT64((int64_t)(T0 + 1234), (int64_t)timer_last_ntp_sync());
}

void test_last_ntp_sync_cleared_by_reset(void) {
    timer_record_ntp_sync(T0);
    timer_reset();
    TEST_ASSERT_EQUAL_INT64(0, (int64_t)timer_last_ntp_sync());
}

/* ------------------------------------------------------------------ */
/* Active-slot guard: selection may never rest on a disabled slot      */
/* ------------------------------------------------------------------ */

void test_ensure_active_slot_keeps_enabled_slot(void) {
    g_rtc_state.active_slot = 1; /* Piano — enabled in TEST_DEFS */
    timer_ensure_active_slot_enabled();
    TEST_ASSERT_EQUAL_INT(1, timer_active_slot());
}

void test_ensure_active_slot_reverts_when_disabled(void) {
    g_rtc_state.active_slot = 2; /* hole in TEST_DEFS */
    timer_ensure_active_slot_enabled();
    TEST_ASSERT_EQUAL_INT(0, timer_active_slot());
}

/* ------------------------------------------------------------------ */
/* Read-only slot accessors (used by the stats/summary builders)       */
/* ------------------------------------------------------------------ */

void test_slot_accessors_read_state_alloc_completions(void) {
    timer_start(T0, 3600);
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_slot_state(0));
    TEST_ASSERT_EQUAL_INT32(3600, timer_slot_allocation(0));
    g_rtc_state.slots[1].completions = 3;
    TEST_ASSERT_EQUAL_UINT16(3, timer_slot_completions(1));
}

void test_slot_accessors_out_of_range_are_benign(void) {
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_slot_state(-1));
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_slot_state(TIMER_SLOT_COUNT));
    TEST_ASSERT_EQUAL_INT32(0, timer_slot_allocation(99));
    TEST_ASSERT_EQUAL_UINT16(0, timer_slot_completions(99));
}

void test_current_date_tracks_record_date(void) {
    TEST_ASSERT_EQUAL_STRING("", timer_current_date());
    timer_record_date(T0); /* 2026-01-05 UTC */
    TEST_ASSERT_EQUAL_STRING("2026-01-05", timer_current_date());
}

/* Break-due can land INSIDE the final minute (short allocations, e.g.
   3 min screen / 2 min interval): the event watch must keep checking
   timer_break_due mid-watch or the break is silently swallowed by the
   expiry. This pins the trigger condition the watch loop relies on. */
void test_break_due_lands_inside_final_minute(void) {
    timer_start(T0, 180);                              /* 3 min allocation, 2 min break interval */
    TEST_ASSERT_FALSE(timer_break_due(T0 + 110, 120)); /* pre-watch wake: not yet */
    TEST_ASSERT_TRUE(timer_break_due(T0 + 120, 120));  /* due at 60 s remaining */
    timer_start_break(T0 + 120, 120);
    TEST_ASSERT_EQUAL(TIMER_BREAK, timer_get_state());
    /* The frozen remaining survives the break: back to PAUSED with 60 s. */
    timer_tick(T0 + 240); /* break over */
    TEST_ASSERT_EQUAL(TIMER_PAUSED, timer_get_state());
    TEST_ASSERT_EQUAL_INT32(60, g_rtc_state.slots[0].remaining_at_pause);
}

/* ==================================================================== */
/* Non-blocking Screen Break: slot 0 may hold TIMER_BREAK while ANY     */
/* slot is active. The combination space is wide, so the contract is a  */
/* table — a new row in the code needs a new row here.                  */
/* ==================================================================== */

/* Slot-0 setup for a matrix row. */
typedef enum {
    S0_IDLE = 0,
    S0_RUNNING,    /* Screen counting down */
    S0_BREAK,      /* break armed: starts T0+600, ends T0+1500 */
    S0_POST_BREAK, /* that break already ended -> Screen PAUSED */
} s0_setup_t;

/* Wall time every matrix assertion is made at: inside the break window
   (T0+600 .. T0+1500) and after the extra slot's own setup. */
#define M_NOW (T0 + 800)

/* "Would the break-over chime fire?" — n/a on rows with no break. */
typedef enum { CHIME_NA = 0, CHIME_YES, CHIME_NO } chime_expect_t;

typedef struct {
    const char *name;
    s0_setup_t s0;
    int active_slot;             /* 0 = Screen, 1 = Piano */
    timer_state_t active_state;  /* state the ACTIVE slot is left in */
    bool swap_allowed;           /* timer_swap_allowed() */
    bool break_active;           /* timer_break_active() */
    bool break_remaining_gt0;    /* timer_break_remaining(M_NOW) > 0 */
    btn_a_action_t btn_a;        /* button_a_apply(M_NOW) */
    timer_state_t state_after_a; /* active slot afterwards */
    /* Composition of the state fact (timer_any_extra_running) with the
       pure policy (wake_policy_break_chime, tested on its own in
       test_wake_policy). The snap back to Screen rides the same answer. */
    chime_expect_t chime_at_break_end;
} matrix_case_t;

static const matrix_case_t STATE_MATRIX[] = {
    /* --- break running (slot 0 == BREAK) --- */
    {"break, Screen selected", S0_BREAK, 0, TIMER_BREAK, true, true, true, BTN_A_NONE, TIMER_BREAK, CHIME_YES},
    {"break, extra IDLE", S0_BREAK, 1, TIMER_IDLE, true, true, true, BTN_A_STARTED, TIMER_RUNNING, CHIME_YES},
    {"break, extra RUNNING", S0_BREAK, 1, TIMER_RUNNING, false, true, true, BTN_A_PAUSED, TIMER_PAUSED, CHIME_NO},
    {"break, extra PAUSED", S0_BREAK, 1, TIMER_PAUSED, true, true, true, BTN_A_RESUMED, TIMER_RUNNING, CHIME_YES},
    {"break, extra EXPIRED", S0_BREAK, 1, TIMER_EXPIRED, true, true, true, BTN_A_NONE, TIMER_EXPIRED, CHIME_YES},
    /* --- no break running: the chime column does not apply --- */
    {"post-break Screen PAUSED, extra RUNNING", S0_POST_BREAK, 1, TIMER_RUNNING, false, false, false, BTN_A_PAUSED,
     TIMER_PAUSED, CHIME_NA},
    {"Screen RUNNING", S0_RUNNING, 0, TIMER_RUNNING, false, false, false, BTN_A_PAUSED, TIMER_PAUSED, CHIME_NA},
    {"Screen IDLE, extra IDLE", S0_IDLE, 1, TIMER_IDLE, true, false, false, BTN_A_STARTED, TIMER_RUNNING, CHIME_NA},
};

/* Put the ACTIVE slot into `st` (extra slots only — slot 0's state comes
   from the s0 column). */
static void matrix_set_active_state(timer_state_t st) {
    switch (st) {
        case TIMER_RUNNING:
            timer_start(T0 + 700, 900);
            break;
        case TIMER_PAUSED:
            timer_start(T0 + 700, 900);
            timer_pause(T0 + 750);
            break;
        case TIMER_EXPIRED:
            timer_start(T0 + 700, 10);
            timer_tick(T0 + 790);
            break;
        default: /* IDLE: fresh slot */
            break;
    }
}

static void matrix_setup(const matrix_case_t *c) {
    timer_reset();
    switch (c->s0) {
        case S0_RUNNING:
            timer_start(T0, 3600);
            break;
        case S0_BREAK:
            timer_start(T0, 3600);
            timer_start_break(T0 + 600, 900); /* frozen 3000 s; ends T0+1500 */
            break;
        case S0_POST_BREAK:
            /* An EARLIER break, already over before the extra slot below
               starts at T0+700 — the fixture has to be temporally
               coherent even though every call takes `now` explicitly. */
            timer_start(T0, 3600);
            timer_start_break(T0 + 100, 300); /* ends T0+400 */
            timer_break_tick(T0 + 400);
            TEST_ASSERT_TRUE(timer_break_take_ended(T0 + 400, NULL));
            break;
        default:
            break;
    }
    if (c->active_slot != 0) {
        TEST_ASSERT_TRUE_MESSAGE(timer_select_next(), c->name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(c->active_slot, timer_active_slot(), c->name);
        matrix_set_active_state(c->active_state);
    }
    TEST_ASSERT_EQUAL_MESSAGE(c->active_state, timer_get_state(), c->name);
}

void test_state_matrix(void) {
    for (size_t i = 0; i < sizeof(STATE_MATRIX) / sizeof(STATE_MATRIX[0]); i++) {
        const matrix_case_t *c = &STATE_MATRIX[i];
        matrix_setup(c);

        TEST_ASSERT_EQUAL_INT_MESSAGE((int)c->swap_allowed, (int)timer_swap_allowed(), c->name);
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)c->break_active, (int)timer_break_active(), c->name);
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)c->break_remaining_gt0, (int)(timer_break_remaining(M_NOW) > 0), c->name);
        if (c->chime_at_break_end != CHIME_NA) {
            /* Composition: the state fact feeds the pure policy. Timely
               observation (overdue 0) isolates the suppression rule; the
               grace window is exercised in test_wake_policy. */
            chime_expect_t got = wake_policy_break_chime(timer_any_extra_running(), 0) ? CHIME_YES : CHIME_NO;
            TEST_ASSERT_EQUAL_INT_MESSAGE((int)c->chime_at_break_end, (int)got, c->name);
        }

        TEST_ASSERT_EQUAL_MESSAGE(c->btn_a, button_a_apply(M_NOW), c->name);
        TEST_ASSERT_EQUAL_MESSAGE(c->state_after_a, timer_get_state(), c->name);
        assert_state_legal();
    }
}

/* ---- selection while a break runs ---- */

void test_select_next_off_slot_zero_and_back_preserves_break(void) {
    timer_start(T0, 3600);
    timer_start_break(T0 + 600, 900);
    TEST_ASSERT_TRUE(timer_select_next()); /* -> Piano */
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state());
    TEST_ASSERT_TRUE(timer_break_active());
    TEST_ASSERT_EQUAL_INT32(700, timer_break_remaining(T0 + 800));
    TEST_ASSERT_TRUE(timer_select_next()); /* -> Laundry (slot 2 disabled) */
    TEST_ASSERT_TRUE(timer_select_next()); /* -> Violin */
    TEST_ASSERT_TRUE(timer_select_next()); /* -> Screen */
    TEST_ASSERT_EQUAL_INT(0, timer_active_slot());
    TEST_ASSERT_EQUAL(TIMER_BREAK, timer_get_state()); /* still the break screen */
    TEST_ASSERT_EQUAL_INT32(3000, g_rtc_state.slots[0].remaining_at_pause);
}

void test_select_screen_returns_from_non_running_extra(void) {
    timer_start(T0, 3600);
    timer_start_break(T0 + 600, 900);
    TEST_ASSERT_TRUE(timer_select_next()); /* Piano, IDLE */
    TEST_ASSERT_TRUE(timer_select_screen());
    TEST_ASSERT_EQUAL_INT(0, timer_active_slot());
    /* PAUSED and EXPIRED snap back too */
    TEST_ASSERT_TRUE(timer_select_next());
    timer_start(T0 + 700, 900);
    timer_pause(T0 + 750);
    TEST_ASSERT_TRUE(timer_select_screen());
    TEST_ASSERT_EQUAL_INT(0, timer_active_slot());
    TEST_ASSERT_TRUE(timer_select_next());
    timer_start(T0 + 800, 10);
    timer_tick(T0 + 900); /* EXPIRED */
    TEST_ASSERT_TRUE(timer_select_screen());
    TEST_ASSERT_EQUAL_INT(0, timer_active_slot());
}

void test_select_screen_refused_while_extra_running(void) {
    /* Stealing the selection out from under a running timer would be
       hostile — the kid gets back to Screen with Button C. */
    timer_start(T0, 3600);
    timer_start_break(T0 + 600, 900);
    TEST_ASSERT_TRUE(timer_select_next());
    timer_start(T0 + 700, 900);
    TEST_ASSERT_FALSE(timer_select_screen());
    TEST_ASSERT_EQUAL_INT(1, timer_active_slot());
}

void test_select_screen_already_on_screen_is_a_noop(void) {
    timer_start(T0, 3600); /* Screen RUNNING, already selected */
    TEST_ASSERT_TRUE(timer_select_screen());
    TEST_ASSERT_EQUAL_INT(0, timer_active_slot());
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
}

/* ---- I5: an extra slot's whole life cycle never touches the break ---- */

void test_extra_slot_lifecycle_leaves_break_untouched(void) {
    timer_start(T0, 3600);
    timer_start_break(T0 + 600, 900); /* frozen 3000, ends T0+1500 */
    TEST_ASSERT_TRUE(timer_select_next());

    /* assert_state_legal() after every mutation: the I5 witness compares
       slot 0's frozen screen time against the previous observation, so an
       extra slot's run consuming it fails here rather than silently. */
    assert_state_legal();
    timer_start(T0 + 700, 500); /* expires T0+1200 */
    assert_state_legal();
    TEST_ASSERT_EQUAL_INT64((int64_t)T0 + 1500, g_rtc_state.slots[0].break_expiry_wall);
    TEST_ASSERT_EQUAL_INT32(3000, g_rtc_state.slots[0].remaining_at_pause);

    timer_tick(T0 + 800);
    assert_state_legal();
    timer_pause(T0 + 850); /* 350 left */
    assert_state_legal();
    timer_resume(T0 + 900); /* expires T0+1250 */
    assert_state_legal();
    timer_tick(T0 + 1100); /* still inside the run */
    assert_state_legal();
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
    timer_tick(T0 + 1300); /* past T0+1250 -> EXPIRED, all while the break runs */
    assert_state_legal();
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());

    /* Slot 0 is exactly where the break left it */
    TEST_ASSERT_EQUAL(TIMER_BREAK, timer_slot_state(0));
    TEST_ASSERT_EQUAL_INT64((int64_t)T0 + 1500, g_rtc_state.slots[0].break_expiry_wall);
    TEST_ASSERT_EQUAL_INT32(3000, g_rtc_state.slots[0].remaining_at_pause);
    TEST_ASSERT_EQUAL_INT32(200, timer_break_remaining(T0 + 1300));
}

/* ---- the break-end edge is a LATCH, not a return value ----
   timer_tick() ends an elapsed break internally, so an edge reported only
   as a return value could be consumed by any tick and silently lost (it
   was: the expiry alert's tick swallowed the chime). Latching removes the
   ordering obligation — whoever drains last still sees it. */

void test_break_end_latches_at_the_transition(void) {
    timer_start(T0, 3600);
    timer_start_break(T0 + 600, 900); /* ends T0+1500 */

    timer_break_tick(T0 + 1499);
    TEST_ASSERT_EQUAL(TIMER_BREAK, timer_slot_state(0));
    TEST_ASSERT_FALSE(timer_break_take_ended(T0 + 1499, NULL)); /* nothing latched yet */

    timer_break_tick(T0 + 1500);
    TEST_ASSERT_EQUAL(TIMER_PAUSED, timer_slot_state(0));
    TEST_ASSERT_EQUAL_INT64(0, g_rtc_state.slots[0].break_expiry_wall);
    TEST_ASSERT_EQUAL_INT32(3000, g_rtc_state.slots[0].remaining_at_pause); /* screen time survives */

    int32_t overdue = -1;
    TEST_ASSERT_TRUE(timer_break_take_ended(T0 + 1500, &overdue));
    TEST_ASSERT_EQUAL_INT32(0, overdue);
    /* Consumed: exactly one owner gets the edge */
    TEST_ASSERT_FALSE(timer_break_take_ended(T0 + 1501, &overdue));
    TEST_ASSERT_EQUAL_INT32(0, overdue);
}

void test_break_end_latch_survives_an_intervening_tick(void) {
    /* The regression this design exists for: a timer_tick between the
       transition and the drain must NOT eat the edge. */
    timer_start(T0, 3600);
    timer_start_break(T0 + 600, 900);
    TEST_ASSERT_TRUE(timer_select_next());
    timer_start(T0 + 700, 1200); /* Piano runs past the break end */

    timer_tick(T0 + 1600); /* ends the break internally... */
    TEST_ASSERT_EQUAL(TIMER_PAUSED, timer_slot_state(0));
    timer_tick(T0 + 1650); /* ...and more ticks must not clear the latch */
    timer_tick(T0 + 1700);

    int32_t overdue = -1;
    TEST_ASSERT_TRUE(timer_break_take_ended(T0 + 1700, &overdue));
    TEST_ASSERT_EQUAL_INT32(200, overdue);               /* measured from the WALL end, T0+1500 */
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state()); /* Piano untouched */
}

void test_break_end_overdue_is_measured_at_drain_time(void) {
    /* Lateness is "how late are we telling the user", not "how late was
       the tick" — the expiry alert can hold the CPU for ~15 s between the
       two, and the grace window must judge the moment the chime would
       actually sound. */
    timer_start(T0, 3600);
    timer_start_break(T0 + 600, 900); /* ends T0+1500 */
    timer_break_tick(T0 + 1500);      /* observed on time... */
    int32_t overdue = -1;
    TEST_ASSERT_TRUE(timer_break_take_ended(T0 + 1530, &overdue)); /* ...drained 30 s later */
    TEST_ASSERT_EQUAL_INT32(30, overdue);
}

void test_break_tick_does_nothing_without_a_break(void) {
    timer_break_tick(T0); /* IDLE */
    TEST_ASSERT_FALSE(timer_break_take_ended(T0, NULL));
    timer_start(T0, 3600);
    timer_break_tick(T0 + 100); /* RUNNING */
    TEST_ASSERT_FALSE(timer_break_take_ended(T0 + 100, NULL));
}

void test_break_tick_leaves_the_active_extra_alone(void) {
    timer_start(T0, 3600);
    timer_start_break(T0 + 600, 900);
    TEST_ASSERT_TRUE(timer_select_next());
    timer_start(T0 + 700, 1200); /* Piano runs past the break end */
    timer_break_tick(T0 + 1500);
    TEST_ASSERT_TRUE(timer_break_take_ended(T0 + 1500, NULL));
    TEST_ASSERT_EQUAL_INT(1, timer_active_slot());
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
    TEST_ASSERT_EQUAL_INT64((int64_t)T0 + 1900, g_rtc_state.slots[1].expiry_wall_time);
    TEST_ASSERT_EQUAL(TIMER_PAUSED, timer_slot_state(0));
}

void test_tick_on_an_extra_slot_still_ends_an_elapsed_break(void) {
    /* Defensive delegation: no path may strand a break, even one nobody
       is looking at. */
    timer_start(T0, 3600);
    timer_start_break(T0 + 600, 900);
    TEST_ASSERT_TRUE(timer_select_next());
    timer_start(T0 + 700, 1200);
    timer_tick(T0 + 1600); /* ticking Piano ends Screen's break */
    TEST_ASSERT_EQUAL(TIMER_PAUSED, timer_slot_state(0));
    TEST_ASSERT_EQUAL_INT64(0, g_rtc_state.slots[0].break_expiry_wall);
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state()); /* Piano keeps going */
    TEST_ASSERT_TRUE(timer_break_take_ended(T0 + 1600, NULL));
}

void test_day_rollover_clears_a_pending_break_end_latch(void) {
    /* A latch that outlived its day would chime into the next morning. */
    timer_start(T0, 3600);
    timer_start_break(T0 + 600, 900);
    timer_break_tick(T0 + 1500);
    timer_reset();
    TEST_ASSERT_FALSE(timer_break_take_ended(T0 + 1500, NULL));
}

void test_snapshot_restore_of_an_elapsed_break_never_latches(void) {
    /* The power-cycle path converts BREAK->PAUSED silently by design; it
       must not manufacture a chime on the next boot. */
    timer_start(T0, 3600);
    timer_start_break(T0 + 600, 900);
    timer_record_date(T0);
    timer_snapshot_t snap;
    timer_make_snapshot(&snap);

    timer_reset();
    TEST_ASSERT_TRUE(timer_restore_snapshot(&snap, T0 + 2000)); /* past T0+1500 */
    TEST_ASSERT_EQUAL(TIMER_PAUSED, timer_slot_state(0));
    TEST_ASSERT_FALSE(timer_break_take_ended(T0 + 2000, NULL));
}

/* ---- clock steps move both pending wall times ---- */

void test_shift_expiry_moves_extra_expiry_and_background_break(void) {
    timer_start(T0, 3600);
    timer_start_break(T0 + 600, 900); /* break ends T0+1500 */
    TEST_ASSERT_TRUE(timer_select_next());
    timer_start(T0 + 700, 1200); /* Piano expires T0+1900 */
    timer_shift_expiry(120);
    TEST_ASSERT_EQUAL_INT64((int64_t)T0 + 1900 + 120, g_rtc_state.slots[1].expiry_wall_time);
    /* The balance's segment start is on slot 0, not on the running extra */
    TEST_ASSERT_EQUAL_INT64((int64_t)T0 + 700 + 120, g_rtc_state.slots[0].run_started_wall);
    TEST_ASSERT_EQUAL_INT64((int64_t)T0 + 1500 + 120, g_rtc_state.slots[0].break_expiry_wall);
}

void test_shift_expiry_moves_a_latched_break_end(void) {
    /* The latch stores a WALL time, so it steps with an NTP correction
       like every other stored wall time. main.c really can land a clock
       step between the transition and the drain (finish_action_and_render
       ticks, then net_apply_finish applies the step, then drains); an
       unshifted latch would read a forward step as lateness and silence a
       chime that is not actually late. */
    timer_start(T0, 3600);
    timer_start_break(T0 + 600, 900); /* ends T0+1500 */
    timer_break_tick(T0 + 1500);      /* latched, on time */
    timer_shift_expiry(300);          /* NTP steps the clock 5 min forward */

    int32_t overdue = -1;
    TEST_ASSERT_TRUE(timer_break_take_ended(T0 + 1500 + 300, &overdue));
    TEST_ASSERT_EQUAL_INT32(0, overdue); /* still on time on the corrected clock */
    TEST_ASSERT_TRUE(wake_policy_break_chime(false, overdue));
}

void test_shift_expiry_moves_a_latched_break_end_backwards(void) {
    timer_start(T0, 3600);
    timer_start_break(T0 + 600, 900);
    timer_break_tick(T0 + 1500);
    timer_shift_expiry(-120);
    int32_t overdue = -1;
    TEST_ASSERT_TRUE(timer_break_take_ended(T0 + 1500 - 120, &overdue));
    TEST_ASSERT_EQUAL_INT32(0, overdue);
}

void test_shift_expiry_leaves_an_undrained_latch_alone(void) {
    /* Nothing latched: the shift must not manufacture one. */
    timer_start(T0, 3600);
    timer_shift_expiry(300);
    TEST_ASSERT_FALSE(timer_break_take_ended(T0 + 300, NULL));
}

void test_shift_expiry_never_double_shifts_slot_zero(void) {
    /* Slot 0 active and in BREAK: the break expiry must move exactly once
       (it is neither RUNNING nor a second slot). */
    timer_start(T0, 3600);
    timer_start_break(T0 + 600, 900);
    timer_shift_expiry(60);
    TEST_ASSERT_EQUAL_INT64((int64_t)T0 + 1500 + 60, g_rtc_state.slots[0].break_expiry_wall);
    TEST_ASSERT_EQUAL_INT64(0, g_rtc_state.slots[0].expiry_wall_time);
}

/* ---- helpers driving suppression + the swap hint ---- */

void test_any_extra_running_ignores_slot_zero(void) {
    TEST_ASSERT_FALSE(timer_any_extra_running());
    timer_start(T0, 3600); /* Screen RUNNING */
    TEST_ASSERT_FALSE(timer_any_extra_running());
    timer_start_break(T0 + 600, 900);
    TEST_ASSERT_TRUE(timer_select_next());
    TEST_ASSERT_FALSE(timer_any_extra_running()); /* Piano idle */
    timer_start(T0 + 700, 900);
    TEST_ASSERT_TRUE(timer_any_extra_running());
    timer_pause(T0 + 800);
    TEST_ASSERT_FALSE(timer_any_extra_running());
}

void test_next_slot_walks_enabled_slots_and_skips_holes(void) {
    TEST_ASSERT_EQUAL_INT(1, timer_next_slot()); /* Screen -> Piano */
    TEST_ASSERT_TRUE(timer_select_next());
    TEST_ASSERT_EQUAL_INT(3, timer_next_slot()); /* Piano -> Laundry (2 disabled) */
    TEST_ASSERT_TRUE(timer_select_next());
    TEST_ASSERT_EQUAL_INT(4, timer_next_slot());
    TEST_ASSERT_TRUE(timer_select_next());
    TEST_ASSERT_EQUAL_INT(0, timer_next_slot()); /* Violin wraps to Screen */
}

void test_next_slot_is_minus_one_without_extras(void) {
    timer_set_defs(DEFS_NO_EXTRAS, TIMER_SLOT_COUNT);
    TEST_ASSERT_EQUAL_INT(-1, timer_next_slot());
}

void test_next_slot_unaffected_by_a_running_refusal(void) {
    /* next_slot answers "which one", not "may I" — the refusal is
       timer_swap_allowed's job (display draws the hint from both). */
    timer_start(T0, 3600);
    TEST_ASSERT_FALSE(timer_swap_allowed());
    TEST_ASSERT_EQUAL_INT(1, timer_next_slot());
}

/* ---- snapshot: a break behind a running extra survives a power cycle ---- */

void test_snapshot_round_trips_background_break_with_running_extra(void) {
    timer_start(T0, 3600);
    timer_start_break(T0 + 600, 900); /* ends T0+1500, 3000 frozen */
    TEST_ASSERT_TRUE(timer_select_next());
    timer_start(T0 + 700, 1200); /* Piano expires T0+1900 */
    timer_record_date(T0);
    timer_snapshot_t snap;
    timer_make_snapshot(&snap);

    timer_reset();
    TEST_ASSERT_TRUE(timer_restore_snapshot(&snap, T0 + 1000));
    TEST_ASSERT_EQUAL_INT(1, timer_active_slot());
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
    TEST_ASSERT_EQUAL(TIMER_BREAK, timer_slot_state(0));
    TEST_ASSERT_TRUE(timer_break_active());
    TEST_ASSERT_EQUAL_INT32(500, timer_break_remaining(T0 + 1000));
    TEST_ASSERT_EQUAL_INT32(3000, g_rtc_state.slots[0].remaining_at_pause);
}

void test_snapshot_break_ended_while_powered_off_restores_paused_non_active(void) {
    /* Same silent conversion as the active case — the chime never replays
       after a power cycle. */
    timer_start(T0, 3600);
    timer_start_break(T0 + 600, 900);
    TEST_ASSERT_TRUE(timer_select_next());
    timer_start(T0 + 700, 3600);
    timer_record_date(T0);
    timer_snapshot_t snap;
    timer_make_snapshot(&snap);

    timer_reset();
    TEST_ASSERT_TRUE(timer_restore_snapshot(&snap, T0 + 2000)); /* past T0+1500 */
    TEST_ASSERT_EQUAL(TIMER_PAUSED, timer_slot_state(0));
    TEST_ASSERT_EQUAL_INT64(0, g_rtc_state.slots[0].break_expiry_wall);
    TEST_ASSERT_FALSE(timer_break_active());
    TEST_ASSERT_EQUAL_INT(1, timer_active_slot());
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
}

void test_snapshot_rejects_a_break_on_an_extra_slot(void) {
    /* I1 is a storage invariant too: corruption that slips past the
       checksum must not resurrect a break on the wrong slot. */
    timer_start(T0, 3600);
    timer_record_date(T0);
    timer_snapshot_t snap;
    timer_make_snapshot(&snap);
    snap.slots[1].state = TIMER_BREAK;
    snap.slots[1].break_expiry_wall = (int64_t)T0 + 900;
    snap.checksum = timer_snapshot_checksum(&snap);

    timer_reset();
    TEST_ASSERT_FALSE(timer_restore_snapshot(&snap, T0 + 100));
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state());
}

/* v6: the interrupted slot round-trips, so a break that spans a crash
   still knows where to put the selection back. */
void test_snapshot_round_trips_the_interrupted_slot(void) {
    select_slot(SLOT_LAUNDRY);
    timer_start(T0, 3600);
    timer_start_break(T0 + 1800, 900);
    timer_record_date(T0);
    timer_snapshot_t snap;
    timer_make_snapshot(&snap);
    TEST_ASSERT_EQUAL_UINT8(SLOT_LAUNDRY, snap.break_interrupted_slot);

    timer_reset();
    TEST_ASSERT_TRUE(timer_restore_snapshot(&snap, T0 + 2000));
    TEST_ASSERT_EQUAL_INT(SLOT_LAUNDRY, timer_break_interrupted_slot());
}

void test_snapshot_rejects_an_out_of_range_interrupted_slot(void) {
    timer_start(T0, 3600);
    timer_record_date(T0);
    timer_snapshot_t snap;
    timer_make_snapshot(&snap);
    snap.break_interrupted_slot = TIMER_SLOT_COUNT; /* one past the end */
    snap.checksum = timer_snapshot_checksum(&snap);

    timer_reset();
    TEST_ASSERT_FALSE(timer_restore_snapshot(&snap, T0 + 100));
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state());
}

/* A run that expired while the device was powered off is still a fold
   point: the balance must land at the EXPIRY, not keep accruing across
   the dark gap. */
void test_snapshot_restore_folds_a_powered_off_expiry(void) {
    select_slot(SLOT_LAUNDRY);
    timer_start(T0, 600);
    timer_record_date(T0);
    timer_snapshot_t snap;
    timer_make_snapshot(&snap);

    timer_reset();
    TEST_ASSERT_TRUE(timer_restore_snapshot(&snap, T0 + 5000));
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_slot_state(SLOT_LAUNDRY));
    TEST_ASSERT_EQUAL_INT32(600, timer_run_accum(T0 + 5000));
    TEST_ASSERT_EQUAL_INT64(0, g_rtc_state.slots[0].run_started_wall);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_bonus_reconcile_grants_only_the_delta);
    RUN_TEST(test_bonus_reconcile_lowering_target_reclaims_delta);
    RUN_TEST(test_bonus_reconcile_negative_target_applies_delta_once);
    RUN_TEST(test_bonus_reconcile_stale_replay_across_rollover_is_noop);
    RUN_TEST(test_adjust_running_deducts_expiry_and_allocation);
    RUN_TEST(test_adjust_running_past_zero_expires_on_next_tick);
    RUN_TEST(test_adjust_paused_past_zero_expires);
    RUN_TEST(test_adjust_paused_partial_deduction_stays_paused);
    RUN_TEST(test_adjust_break_clamps_frozen_remaining);
    RUN_TEST(test_adjust_expired_negative_is_noop);
    RUN_TEST(test_adjust_idle_banks_negative_start_clamps_at_zero);
    RUN_TEST(test_snapshot_round_trips_negative_bonus);
    RUN_TEST(test_bonus_applied_resets_at_rollover);
    RUN_TEST(test_bonus_applied_survives_snapshot_roundtrip);
    RUN_TEST(test_reset_state_is_idle);
    RUN_TEST(test_reset_expiry_is_zero);
    RUN_TEST(test_reset_remaining_at_pause_is_zero);
    RUN_TEST(test_start_sets_state_running);
    RUN_TEST(test_start_sets_expiry_wall_time);
    RUN_TEST(test_start_sets_allocation_sec);
    RUN_TEST(test_tick_does_not_modify_expiry_wall_time);
    RUN_TEST(test_tick_returns_correct_remaining_seconds);
    RUN_TEST(test_tick_returns_remaining_near_zero);
    RUN_TEST(test_tick_transitions_to_expired_when_time_elapsed);
    RUN_TEST(test_tick_returns_non_positive_when_expired);
    RUN_TEST(test_tick_at_exact_expiry_transitions);
    RUN_TEST(test_tick_while_paused_returns_remaining_at_pause);
    RUN_TEST(test_pause_sets_state_paused);
    RUN_TEST(test_pause_saves_remaining_at_pause);
    RUN_TEST(test_pause_clears_expiry_wall_time);
    RUN_TEST(test_resume_sets_state_running);
    RUN_TEST(test_resume_sets_expiry_from_remaining_at_pause);
    RUN_TEST(test_resume_preserves_remaining_within_one_second);
    RUN_TEST(test_full_cycle_idle_run_pause_resume_expire);
    RUN_TEST(test_record_date_formats_yyyy_mm_dd);
    RUN_TEST(test_is_new_day_false_when_same_date);
    RUN_TEST(test_is_new_day_true_after_midnight);
    RUN_TEST(test_is_new_day_true_on_cold_boot);
    RUN_TEST(test_needs_ntp_sync_false_immediately_after_sync);
    RUN_TEST(test_needs_ntp_sync_true_after_10_minutes);
    RUN_TEST(test_shift_expiry_forward_while_running);
    RUN_TEST(test_shift_expiry_backward_while_running);
    RUN_TEST(test_shift_expiry_noop_when_paused);
    RUN_TEST(test_shift_expiry_noop_when_idle);
    RUN_TEST(test_run_accum_counts_running_time);
    RUN_TEST(test_run_accum_excludes_pause_gaps);
    RUN_TEST(test_break_due_at_interval_survives_a_pause);
    RUN_TEST(test_row2_a_laundry_run_earns_the_break_and_is_paused_by_it);
    RUN_TEST(test_row3_expiry_and_a_due_break_in_one_tick);
    RUN_TEST(test_row4_a_break_starts_an_eligible_timer_normally);
    RUN_TEST(test_row5_a_break_refuses_a_non_eligible_timer);
    RUN_TEST(test_row6_a_break_refuses_screen_itself);
    RUN_TEST(test_row7_break_end_returns_to_the_interrupted_slot);
    RUN_TEST(test_row8_break_end_does_not_snap_while_a_timer_runs);
    RUN_TEST(test_row9_break_end_stays_on_screen_when_screen_was_interrupted);
    RUN_TEST(test_row10_break_exit_is_idle_when_screen_never_started);
    RUN_TEST(test_row11_expiry_folds_the_segment_and_stops_the_balance);
    RUN_TEST(test_row12_starting_a_timer_does_not_reset_the_balance);
    RUN_TEST(test_row13_break_start_preserves_a_paused_screens_banked_time);
    RUN_TEST(test_row14_an_eligible_run_drains_the_balance);
    RUN_TEST(test_row15_the_balance_floors_at_zero);
    RUN_TEST(test_row16_an_eligibility_edit_folds_at_the_old_sign);
    RUN_TEST(test_row17_idle_neither_adds_nor_drains);
    RUN_TEST(test_balance_motivating_case_laundry_then_violin_then_screen);
    RUN_TEST(test_an_eligible_run_never_makes_a_break_due);
    RUN_TEST(test_break_is_not_due_while_screen_holds_no_startable_time);
    RUN_TEST(test_break_is_not_due_while_one_is_already_running);
    RUN_TEST(test_start_allowed_outside_a_break_is_always_true);
    RUN_TEST(test_eligible_extra_count_counts_only_break_eligible_slots);
    RUN_TEST(test_shift_expiry_moves_the_balance_segment_on_slot_zero);
    RUN_TEST(test_reconcile_reset_folds_the_live_segment);
    RUN_TEST(test_start_break_freezes_timer_and_arms_break);
    RUN_TEST(test_break_remaining_counts_down);
    RUN_TEST(test_tick_during_break_holds_then_transitions_to_paused);
    RUN_TEST(test_shift_expiry_shifts_run_started_wall_when_running);
    RUN_TEST(test_shift_expiry_shifts_break_expiry_when_in_break);
    RUN_TEST(test_make_snapshot_captures_running_state);
    RUN_TEST(test_restore_snapshot_running_when_date_matches);
    RUN_TEST(test_restore_snapshot_paused_preserves_remaining);
    RUN_TEST(test_restore_snapshot_rejected_when_date_differs);
    RUN_TEST(test_restore_snapshot_rejected_on_version_mismatch);
    RUN_TEST(test_restore_snapshot_rejected_on_checksum_mismatch);
    RUN_TEST(test_restore_snapshot_rejected_on_invalid_state_enum);
    RUN_TEST(test_restore_snapshot_rejected_on_implausible_expiry);
    RUN_TEST(test_restore_snapshot_rejected_all_zeros);
    RUN_TEST(test_snapshot_restores_mid_break_with_same_end_time);
    RUN_TEST(test_snapshot_restore_break_past_end_becomes_paused);
    RUN_TEST(test_snapshot_restore_running_preserves_accrual);
    RUN_TEST(test_restore_snapshot_running_past_expiry_becomes_expired);
    RUN_TEST(test_restore_snapshot_expired_state_restores);
    RUN_TEST(test_default_active_slot_is_zero);
    RUN_TEST(test_select_next_cycles_enabled_slots_skipping_disabled);
    RUN_TEST(test_select_next_refused_while_running);
    RUN_TEST(test_select_next_allowed_during_break);
    RUN_TEST(test_select_next_allowed_when_paused_or_expired);
    RUN_TEST(test_select_next_noop_without_extras);
    RUN_TEST(test_slot_def_accessor);
    RUN_TEST(test_slot_by_name);
    RUN_TEST(test_swap_allowed_tracks_state_and_extras);
    RUN_TEST(test_swap_allowed_false_without_extras);
    RUN_TEST(test_reload_allowed_reloadable_timer_except_running);
    RUN_TEST(test_reload_allowed_non_reloadable_needs_parent_testing);
    RUN_TEST(test_slot_states_are_independent);
    RUN_TEST(test_expiry_wall_accessor_tracks_active_slot);
    RUN_TEST(test_break_never_due_on_extra_slot);
    RUN_TEST(test_reload_refused_while_running);
    RUN_TEST(test_reload_from_paused_returns_to_idle);
    RUN_TEST(test_reload_from_expired_returns_to_idle);
    RUN_TEST(test_reload_only_touches_active_slot);
    RUN_TEST(test_completions_increment_on_expiry_only);
    RUN_TEST(test_reload_midway_does_not_increment_completions);
    RUN_TEST(test_reload_preserves_completions);
    RUN_TEST(test_completions_are_per_slot);
    RUN_TEST(test_reset_clears_all_slots_and_reverts_to_screen);
    RUN_TEST(test_snapshot_v3_roundtrip_multi_slot);
    RUN_TEST(test_snapshot_restores_active_extra_slot);
    RUN_TEST(test_snapshot_restore_expired_while_off_increments_completions);
    RUN_TEST(test_snapshot_rejected_on_bad_active_slot);
    RUN_TEST(test_snapshot_rejected_on_invalid_state_in_any_slot);
    RUN_TEST(test_slot_remaining_per_state);
    RUN_TEST(test_slot_remaining_reads_the_named_slot);
    RUN_TEST(test_screen_used_sec_per_state);
    RUN_TEST(test_screen_used_sec_is_slot_zero_only);
    RUN_TEST(test_grant_idle_banks_bonus_realized_at_start);
    RUN_TEST(test_grant_running_extends_expiry_and_allocation);
    RUN_TEST(test_grant_paused_extends_remaining);
    RUN_TEST(test_grant_expired_becomes_paused_holding_grant);
    RUN_TEST(test_grant_targets_named_non_active_slot);
    RUN_TEST(test_grant_break_extends_frozen_screen_time);
    RUN_TEST(test_snapshot_v4_round_trips_bonus);
    RUN_TEST(test_reload_screen_slot_escapes_break_and_keeps_date);
    RUN_TEST(test_set_defs_count_clamped_to_slot_count);
    RUN_TEST(test_set_defs_shorter_table_disables_missing_slots);
    RUN_TEST(test_is_new_day_tracks_local_date_across_dst);
    RUN_TEST(test_start_with_zero_allocation_expires_on_first_tick);
    RUN_TEST(test_tick_with_clock_stepped_backwards_stays_running);
    RUN_TEST(test_completions_saturate_at_uint16_max);
    RUN_TEST(test_snapshot_restore_falls_back_when_active_slot_disabled);
    RUN_TEST(test_reconcile_rename_running_resets_with_was_running);
    RUN_TEST(test_reconcile_rename_paused_resets_without_was_running);
    RUN_TEST(test_reconcile_rename_preserves_completions);
    RUN_TEST(test_reconcile_disable_resets_like_rename);
    RUN_TEST(test_reconcile_duration_grow_running_delta_shifts_expiry);
    RUN_TEST(test_reconcile_duration_grow_after_pause_resume_history);
    RUN_TEST(test_reconcile_duration_shrink_past_elapsed_expires);
    RUN_TEST(test_reconcile_duration_shrink_running_still_ahead_updates);
    RUN_TEST(test_reconcile_duration_grow_paused_extends_remaining);
    RUN_TEST(test_reconcile_duration_shrink_paused_past_elapsed_expires);
    RUN_TEST(test_reconcile_duration_shift_preserves_granted_time);
    RUN_TEST(test_reconcile_reload_flag_only_is_none);
    RUN_TEST(test_reconcile_identical_def_is_none);
    RUN_TEST(test_reconcile_screen_slot_exempt);
    RUN_TEST(test_reconcile_non_active_paused_slot);
    RUN_TEST(test_reconcile_idle_and_expired_slots_are_none);
    RUN_TEST(test_last_ntp_sync_zero_when_never_synced);
    RUN_TEST(test_last_ntp_sync_derived_from_record);
    RUN_TEST(test_last_ntp_sync_cleared_by_reset);
    RUN_TEST(test_ensure_active_slot_keeps_enabled_slot);
    RUN_TEST(test_ensure_active_slot_reverts_when_disabled);
    RUN_TEST(test_slot_accessors_read_state_alloc_completions);
    RUN_TEST(test_slot_accessors_out_of_range_are_benign);
    RUN_TEST(test_current_date_tracks_record_date);
    RUN_TEST(test_break_due_lands_inside_final_minute);
    /* ---- non-blocking Screen Break ---- */
    RUN_TEST(test_state_matrix);
    RUN_TEST(test_select_next_off_slot_zero_and_back_preserves_break);
    RUN_TEST(test_select_screen_returns_from_non_running_extra);
    RUN_TEST(test_select_screen_refused_while_extra_running);
    RUN_TEST(test_select_screen_already_on_screen_is_a_noop);
    RUN_TEST(test_extra_slot_lifecycle_leaves_break_untouched);
    RUN_TEST(test_break_end_latches_at_the_transition);
    RUN_TEST(test_break_end_latch_survives_an_intervening_tick);
    RUN_TEST(test_break_end_overdue_is_measured_at_drain_time);
    RUN_TEST(test_break_tick_does_nothing_without_a_break);
    RUN_TEST(test_break_tick_leaves_the_active_extra_alone);
    RUN_TEST(test_day_rollover_clears_a_pending_break_end_latch);
    RUN_TEST(test_snapshot_restore_of_an_elapsed_break_never_latches);
    RUN_TEST(test_tick_on_an_extra_slot_still_ends_an_elapsed_break);
    RUN_TEST(test_shift_expiry_moves_extra_expiry_and_background_break);
    RUN_TEST(test_shift_expiry_moves_a_latched_break_end);
    RUN_TEST(test_shift_expiry_moves_a_latched_break_end_backwards);
    RUN_TEST(test_shift_expiry_leaves_an_undrained_latch_alone);
    RUN_TEST(test_shift_expiry_never_double_shifts_slot_zero);
    RUN_TEST(test_any_extra_running_ignores_slot_zero);
    RUN_TEST(test_next_slot_walks_enabled_slots_and_skips_holes);
    RUN_TEST(test_next_slot_is_minus_one_without_extras);
    RUN_TEST(test_next_slot_unaffected_by_a_running_refusal);
    RUN_TEST(test_snapshot_round_trips_background_break_with_running_extra);
    RUN_TEST(test_snapshot_break_ended_while_powered_off_restores_paused_non_active);
    RUN_TEST(test_snapshot_rejects_a_break_on_an_extra_slot);
    RUN_TEST(test_snapshot_round_trips_the_interrupted_slot);
    RUN_TEST(test_snapshot_rejects_an_out_of_range_interrupted_slot);
    RUN_TEST(test_snapshot_restore_folds_a_powered_off_expiry);
    return UNITY_END();
}
