#include <string.h>
#include <time.h>
#include <unity.h>

/* Single-TU compilation */
#include "../../main/timer.c"
#include "mock_hal_time.c"

/* Base timestamp: 2026-01-05 00:00:00 UTC (Monday) */
#define T0 ((time_t)1767571200)

/* Slot table used by the multi-timer tests: slot 0 = Screen (schedule-fed),
   slot 2 left disabled to prove select_next skips holes. */
static const timer_def_t TEST_DEFS[TIMER_SLOT_COUNT] = {
    {"Screen", 0, false},      {"Piano", 900, true},   {"", 0, false}, /* disabled */
    {"Meditation", 600, true}, {"Violin", 900, false},
};

void setUp(void) {
    timer_set_defs(TEST_DEFS, TIMER_SLOT_COUNT);
    timer_reset();
    mock_time_set(T0);
}

void tearDown(void) {}

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

void test_break_due_at_interval_only_when_running(void) {
    timer_start(T0, 7200);
    TEST_ASSERT_FALSE(timer_break_due(T0 + 1799, 1800));
    TEST_ASSERT_TRUE(timer_break_due(T0 + 1800, 1800));
    timer_pause(T0 + 1800); /* accum 1800, but PAUSED never triggers */
    TEST_ASSERT_FALSE(timer_break_due(T0 + 2000, 1800));
    timer_reset();
    TEST_ASSERT_FALSE(timer_break_due(T0 + 9999, 1800)); /* IDLE */
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

void test_select_next_refused_during_break(void) {
    timer_start(T0, 3600);
    timer_start_break(T0 + 1800, 900);
    TEST_ASSERT_FALSE(timer_select_next());
    TEST_ASSERT_EQUAL_INT(0, timer_active_slot());
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
    TEST_ASSERT_FALSE(timer_swap_allowed()); /* BREAK (enforced) */
    timer_reset();
    timer_start(T0, 100);
    timer_tick(T0 + 200); /* -> EXPIRED */
    TEST_ASSERT_TRUE(timer_swap_allowed());
}

void test_swap_allowed_false_without_extras(void) {
    timer_set_defs(NULL, 0);
    TEST_ASSERT_FALSE(timer_swap_allowed());
}

/* ---- multi-timer slots: per-slot independence ---- */

void test_slot_states_are_independent(void) {
    /* Run Piano down to 700 s, pause it, swap away, run Screen, swap back */
    timer_select_next(); /* -> Piano */
    timer_start(T0, 900);
    timer_pause(T0 + 200); /* Piano paused, 700 left */

    timer_select_next(); /* -> Meditation */
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
    timer_select_next(); /* Meditation, idle */
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
    timer_select_next(); /* Meditation */
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

int main(void) {
    UNITY_BEGIN();
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
    RUN_TEST(test_break_due_at_interval_only_when_running);
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
    RUN_TEST(test_select_next_refused_during_break);
    RUN_TEST(test_select_next_allowed_when_paused_or_expired);
    RUN_TEST(test_select_next_noop_without_extras);
    RUN_TEST(test_swap_allowed_tracks_state_and_extras);
    RUN_TEST(test_swap_allowed_false_without_extras);
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
    return UNITY_END();
}
