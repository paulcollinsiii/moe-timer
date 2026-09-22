#include <unity.h>

/* Single-TU compilation of the pure sleep planner */
#include "../../main/sleep_plan.c"

void setUp(void) {}
void tearDown(void) {}

static int32_t plan(timer_state_t st, int sec, int32_t event_rem, bool sync_due) {
    sleep_plan_in_t in = {
        .state = st,
        .sec_into_minute = sec,
        .event_remaining_sec = event_rem,
        .sync_due_by_next_wake = sync_due,
        .break_remaining_sec = 0,
    };
    return sleep_plan_seconds(&in);
}

/* Same, with the optional secondary event: a background break running
   behind another state (main.c populates it only when the break end will
   actually chime). */
static int32_t plan_brk(timer_state_t st, int sec, int32_t event_rem, int32_t break_rem) {
    sleep_plan_in_t in = {
        .state = st,
        .sec_into_minute = sec,
        .event_remaining_sec = event_rem,
        .sync_due_by_next_wake = false,
        .break_remaining_sec = break_rem,
    };
    return sleep_plan_seconds(&in);
}

void test_clock_states_align_to_minute_boundary(void) {
    TEST_ASSERT_EQUAL_INT32(43, plan(TIMER_IDLE, 17, 0, false));
    TEST_ASSERT_EQUAL_INT32(60, plan(TIMER_IDLE, 0, 0, false));
    TEST_ASSERT_EQUAL_INT32(43, plan(TIMER_PAUSED, 17, 0, false));
    TEST_ASSERT_EQUAL_INT32(43, plan(TIMER_EXPIRED, 17, 0, false));
}

void test_boundary_too_close_takes_following_minute(void) {
    TEST_ASSERT_EQUAL_INT32(63, plan(TIMER_IDLE, 57, 0, false)); /* 3 < MIN -> +60 */
    TEST_ASSERT_EQUAL_INT32(5, plan(TIMER_IDLE, 55, 0, false));  /* exactly MIN stays */
}

/* RUNNING/BREAK align to the COUNTDOWN's minute grid, not the wall clock:
   wake when event_remaining hits a round minute so the displayed value is
   truly 1:11:00 (user flow: resume shows 1:12:23 precise, next render
   1:12:00, then 1:11:00...). sec_into_minute is ignored for these. */

void test_running_aligns_to_countdown_grid(void) {
    /* remaining 1:12:23 -> the round value is 23 s away */
    TEST_ASSERT_EQUAL_INT32(23, plan(TIMER_RUNNING, 17, 4343, false));
    /* already on the grid -> full minute */
    TEST_ASSERT_EQUAL_INT32(60, plan(TIMER_RUNNING, 17, 3600, false));
    /* grid point too close -> take the following one */
    TEST_ASSERT_EQUAL_INT32(63, plan(TIMER_RUNNING, 17, 3603, false));
}

void test_running_sync_due_wakes_early_on_countdown_grid(void) {
    /* remaining X:XX:40 -> grid in 40 s, minus 20 s sync lead */
    TEST_ASSERT_EQUAL_INT32(20, plan(TIMER_RUNNING, 17, 3640, true));
    /* lead would leave <5 s (23-20=3): take the previous grid minute —
       wake at remaining 3560, i.e. exactly lead(20) before the 3540 grid */
    TEST_ASSERT_EQUAL_INT32(63, plan(TIMER_RUNNING, 17, 3623, true)); /* 23-20+60 */
    /* on-grid with sync: 60-20 */
    TEST_ASSERT_EQUAL_INT32(40, plan(TIMER_RUNNING, 17, 3600, true));
}

void test_event_wake_lands_before_expiry(void) {
    /* expiry in 100 s: event lead (100-70=30) beats the grid sleep (40) */
    TEST_ASSERT_EQUAL_INT32(30, plan(TIMER_RUNNING, 17, 100, false));
    /* grid wins when it is sooner: remaining 130 -> grid in 10, event in 60 */
    TEST_ASSERT_EQUAL_INT32(10, plan(TIMER_RUNNING, 17, 130, false));
}

void test_final_countdown_handoff_sequence(void) {
    /* corner: successive planner calls walk cleanly into the watch window.
       rem 190 -> grid 10 (wake at 180 = 3:00) */
    TEST_ASSERT_EQUAL_INT32(10, plan(TIMER_RUNNING, 0, 190, false));
    /* rem 180 (just rendered 3:00) -> grid 60 beats event 110 */
    TEST_ASSERT_EQUAL_INT32(60, plan(TIMER_RUNNING, 0, 180, false));
    /* rem 120 (rendered 2:00) -> event 50 beats grid 60: wake at 70 */
    TEST_ASSERT_EQUAL_INT32(50, plan(TIMER_RUNNING, 0, 120, false));
    /* rem 90 -> event 20 beats grid 30: wake at 70, watch shows the 60 step */
    TEST_ASSERT_EQUAL_INT32(20, plan(TIMER_RUNNING, 0, 90, false));
}

void test_event_wake_clamps_when_event_imminent(void) {
    TEST_ASSERT_EQUAL_INT32(5, plan(TIMER_RUNNING, 17, 72, false)); /* 72-70=2 -> 5 */
}

/* remaining EXACTLY 0 takes NO event handoff: the event is happening this
   wake, so there is nothing left to lead up to. Leading to an event 70 s
   in the past would clamp to MIN_SEC and burn a pointless wake 5 s later;
   the grid wake is the right answer. The `> 0` that expresses this is a
   live boundary — RUNNING lands on 0 at expiry and timer_break_remaining()
   returns 0 the moment a break ends — so it is pinned here. */
void test_event_wake_skipped_when_the_event_is_exactly_now(void) {
    TEST_ASSERT_EQUAL_INT32(60, plan(TIMER_RUNNING, 17, 0, false));
    TEST_ASSERT_EQUAL_INT32(40, plan(TIMER_RUNNING, 17, 0, true)); /* 60 - sync lead */
    TEST_ASSERT_EQUAL_INT32(60, plan(TIMER_BREAK, 17, 0, false));
}

void test_break_aligns_to_break_grid_no_sync_lead(void) {
    TEST_ASSERT_EQUAL_INT32(20, plan(TIMER_BREAK, 17, 500, false)); /* 8:20 -> 8:00 */
    TEST_ASSERT_EQUAL_INT32(30, plan(TIMER_BREAK, 17, 100, false)); /* event lead wins */
    TEST_ASSERT_EQUAL_INT32(60, plan(TIMER_BREAK, 17, 900, false)); /* on-grid */
}

/* ---- secondary event: a Screen Break running behind another state ----
   Slot 0 can hold a break while an extra timer is selected, so a wake may
   have TWO future events. main.c fills break_remaining_sec only when the
   break end will chime (nothing RUNNING); a suppressed end needs no wake
   and lands at whatever the next tick wake is. */

void test_break_secondary_pulls_the_wake_in(void) {
    /* Piano PAUSED (wall grid: 43 s) but the break ends in 100 s: wake at
       100-70=30 so the awake watch owns the chime. */
    TEST_ASSERT_EQUAL_INT32(30, plan_brk(TIMER_PAUSED, 17, 0, 100));
    /* IDLE too — the state does not matter, the break end does */
    TEST_ASSERT_EQUAL_INT32(30, plan_brk(TIMER_IDLE, 17, 0, 100));
}

void test_break_secondary_ignored_when_later_than_the_primary(void) {
    /* Break end 10 min out: the ordinary wall-grid wake is sooner */
    TEST_ASSERT_EQUAL_INT32(43, plan_brk(TIMER_PAUSED, 17, 0, 600));
}

void test_break_secondary_ignored_when_zero(void) {
    /* No break (or a suppressed one): today's plan, unchanged */
    TEST_ASSERT_EQUAL_INT32(43, plan_brk(TIMER_PAUSED, 17, 0, 0));
    TEST_ASSERT_EQUAL_INT32(43, plan_brk(TIMER_IDLE, 17, 0, -5));
}

void test_break_secondary_never_naps_below_the_minimum(void) {
    /* Break end already inside the lead: clamp, never 0 or negative */
    TEST_ASSERT_EQUAL_INT32(SLEEP_PLAN_MIN_SEC, plan_brk(TIMER_PAUSED, 17, 0, 72));
    TEST_ASSERT_EQUAL_INT32(SLEEP_PLAN_MIN_SEC, plan_brk(TIMER_PAUSED, 17, 0, 1));
}

void test_break_secondary_can_beat_a_running_expiry(void) {
    /* Screen paused behind... no: RUNNING extra with a chiming break is
       impossible (a RUNNING extra suppresses the chime), but the planner
       must still take the sooner of the two if main.c ever passes both. */
    TEST_ASSERT_EQUAL_INT32(20, plan_brk(TIMER_RUNNING, 0, 300, 90));
    TEST_ASSERT_EQUAL_INT32(30, plan_brk(TIMER_RUNNING, 0, 100, 600));
}

void test_break_as_primary_state_is_unchanged(void) {
    /* Screen selected during its own break: BREAK is the state and the
       break end is the PRIMARY event — the secondary field stays 0 and
       today's cases must not move. */
    TEST_ASSERT_EQUAL_INT32(20, plan(TIMER_BREAK, 17, 500, false));
    TEST_ASSERT_EQUAL_INT32(30, plan(TIMER_BREAK, 17, 100, false));
    TEST_ASSERT_EQUAL_INT32(60, plan(TIMER_BREAK, 17, 900, false));
}

/* A break becomes DUE (rather than ending) while a non-eligible extra
   runs — e.g. Laundry 60 min, balance crossing at 30. That moment needs
   no dedicated wake event: a RUNNING slot always sleeps on the countdown
   minute grid, so it is capped at 60 s whatever the expiry horizon, and
   the per-wake wake_flow_maybe_start_break check catches the crossing in a
   minute. Exactly the fidelity the Screen timer has always had.

   This is a CONTRACT, not an observation: raise the RUNNING cap above a
   minute and a laundry-driven break silently starts arriving late.

   The bound is 64 s, not 60: when the next grid point (or the grid point
   less the sync lead) falls inside SLEEP_PLAN_MIN_SEC the planner takes
   the PREVIOUS grid minute instead, which can add up to MIN_SEC-1. */
#define RUNNING_SLEEP_MAX (60 + SLEEP_PLAN_MIN_SEC - 1)
void test_running_never_sleeps_past_the_minute_grid(void) {
    for (int32_t rem = 61; rem <= 7200; rem++) {
        TEST_ASSERT_TRUE_MESSAGE(plan(TIMER_RUNNING, 17, rem, false) <= RUNNING_SLEEP_MAX,
                                 "RUNNING slept past a minute");
        TEST_ASSERT_TRUE_MESSAGE(plan(TIMER_RUNNING, 17, rem, true) <= RUNNING_SLEEP_MAX,
                                 "RUNNING slept past a minute (sync due)");
    }
}

/* The balance is frozen unless something is RUNNING, so the clock-only
   states cannot cross the interval while asleep however long they nap —
   and they are minute-aligned anyway. */
void test_clock_only_states_stay_minute_aligned(void) {
    for (int sec = 0; sec < 60; sec++) {
        TEST_ASSERT_TRUE(plan(TIMER_IDLE, sec, 0, false) <= RUNNING_SLEEP_MAX);
        TEST_ASSERT_TRUE(plan(TIMER_PAUSED, sec, 0, false) <= RUNNING_SLEEP_MAX);
        TEST_ASSERT_TRUE(plan(TIMER_EXPIRED, sec, 0, false) <= RUNNING_SLEEP_MAX);
    }
}

/* ---- plan assembly: raw timer readings -> sleep_plan_in_t ----
   main.c reads the timer module unconditionally at sleep entry (every one
   of those reads is side-effect free) and hands the readings here; the
   branching that decides which of them matter lives below, where it can
   be tested. The readings stay raw — the selected slot's state and
   expiry, slot 0's break, whether any extra runs — precisely so main.c
   holds no `if`.

   Note what these tests pin that the device cannot: the assembly's
   guards are exercised on input combinations the real timer module never
   produces (a break_remaining with no break active, a RUNNING selection
   over a chiming break). That is the point — the guards are the
   contract, and a guard whose only defence is "the caller never does
   that" is a guard the next caller deletes. */

#define NOW ((time_t)1753800017) /* :17 past the minute */
#define NOW_SEC_INTO_MINUTE 17
/* Distinct on purpose: a field transposition between the expiry horizon,
   the break horizon and the clock must change an assertion, so no two of
   them may share a value or a residue mod 60. */
#define EXPIRY_HORIZON 323 /* %60 = 23, wall %60 = 40 */
#define BREAK_HORIZON 100  /* %60 = 40 */

static sleep_plan_in_t from(sleep_plan_timer_in_t in) {
    return sleep_plan_from_timer(&in);
}

void test_from_timer_running_takes_expiry_and_sync(void) {
    sleep_plan_in_t p = from((sleep_plan_timer_in_t){
        .state = TIMER_RUNNING, .now = NOW, .expiry_wall = NOW + EXPIRY_HORIZON, .ntp_recheck_due = true});
    TEST_ASSERT_EQUAL_INT(TIMER_RUNNING, p.state);
    TEST_ASSERT_EQUAL_INT(NOW_SEC_INTO_MINUTE, p.sec_into_minute);
    TEST_ASSERT_EQUAL_INT32(EXPIRY_HORIZON, p.event_remaining_sec);
    TEST_ASSERT_TRUE(p.sync_due_by_next_wake);
    TEST_ASSERT_EQUAL_INT32(0, p.break_remaining_sec);

    /* the recheck flag is carried, not invented */
    p = from((sleep_plan_timer_in_t){
        .state = TIMER_RUNNING, .now = NOW, .expiry_wall = NOW + EXPIRY_HORIZON, .ntp_recheck_due = false});
    TEST_ASSERT_FALSE(p.sync_due_by_next_wake);
}

/* A RUNNING slot whose expiry already passed (a late wake) keeps a
   NEGATIVE remaining — the planner's own clamp handles it. Folding it to
   0 here would silently move the grid. */
void test_from_timer_running_expiry_may_be_negative(void) {
    sleep_plan_in_t p = from((sleep_plan_timer_in_t){.state = TIMER_RUNNING, .now = NOW, .expiry_wall = NOW - 5});
    TEST_ASSERT_EQUAL_INT32(-5, p.event_remaining_sec);
}

void test_from_timer_break_takes_break_remaining_as_the_primary_event(void) {
    /* Screen selected during its own break. The break end is the PRIMARY
       event, so it lands in event_remaining_sec and the secondary field
       stays 0; the far-future expiry_wall must be ignored, and BREAK
       never syncs however due the recheck is. */
    sleep_plan_in_t p = from((sleep_plan_timer_in_t){.state = TIMER_BREAK,
                                                     .now = NOW,
                                                     .expiry_wall = NOW + 9999,
                                                     .ntp_recheck_due = true,
                                                     .break_active = true,
                                                     .break_remaining_sec = BREAK_HORIZON});
    TEST_ASSERT_EQUAL_INT(TIMER_BREAK, p.state);
    TEST_ASSERT_EQUAL_INT(NOW_SEC_INTO_MINUTE, p.sec_into_minute);
    TEST_ASSERT_EQUAL_INT32(BREAK_HORIZON, p.event_remaining_sec);
    TEST_ASSERT_FALSE(p.sync_due_by_next_wake);
    TEST_ASSERT_EQUAL_INT32(0, p.break_remaining_sec);
}

void test_from_timer_background_break_without_extra_running_gets_a_wake(void) {
    /* Piano paused, Screen on a break behind it: the end will chime, so
       it earns its own wake as the SECONDARY event. */
    const timer_state_t states[] = {TIMER_IDLE, TIMER_PAUSED, TIMER_EXPIRED};
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        sleep_plan_in_t p = from((sleep_plan_timer_in_t){.state = states[i],
                                                         .now = NOW,
                                                         .expiry_wall = NOW + 9999,
                                                         .ntp_recheck_due = true,
                                                         .break_active = true,
                                                         .break_remaining_sec = BREAK_HORIZON,
                                                         .extra_running = false});
        TEST_ASSERT_EQUAL_INT(states[i], p.state);
        TEST_ASSERT_EQUAL_INT32(BREAK_HORIZON, p.break_remaining_sec);
        TEST_ASSERT_EQUAL_INT32(0, p.event_remaining_sec);
        TEST_ASSERT_FALSE(p.sync_due_by_next_wake);
    }
}

void test_from_timer_background_break_with_extra_running_is_suppressed(void) {
    /* A RUNNING extra suppresses the chime (rule 4), so the end is silent
       and needs no dedicated wake — it drops the chip at whatever the
       next tick wake is. */
    sleep_plan_in_t p = from((sleep_plan_timer_in_t){.state = TIMER_PAUSED,
                                                     .now = NOW,
                                                     .break_active = true,
                                                     .break_remaining_sec = BREAK_HORIZON,
                                                     .extra_running = true});
    TEST_ASSERT_EQUAL_INT32(0, p.break_remaining_sec);

    /* the shape the device actually reaches: the running extra IS the
       selection, so state is RUNNING and its expiry is the primary */
    p = from((sleep_plan_timer_in_t){.state = TIMER_RUNNING,
                                     .now = NOW,
                                     .expiry_wall = NOW + EXPIRY_HORIZON,
                                     .break_active = true,
                                     .break_remaining_sec = BREAK_HORIZON,
                                     .extra_running = true});
    TEST_ASSERT_EQUAL_INT32(EXPIRY_HORIZON, p.event_remaining_sec);
    TEST_ASSERT_EQUAL_INT32(0, p.break_remaining_sec);
}

/* The secondary rule keys off break_active, NOT off the remaining value
   being non-zero. timer_break_remaining() happens to return 0 when slot 0
   is not on a break, but that is timer.c's business: the assembly must
   not inherit its correctness from another module's internal guard. */
void test_from_timer_no_break_active_leaves_the_secondary_zero(void) {
    sleep_plan_in_t p = from((sleep_plan_timer_in_t){.state = TIMER_PAUSED,
                                                     .now = NOW,
                                                     .break_active = false,
                                                     .break_remaining_sec = BREAK_HORIZON,
                                                     .extra_running = false});
    TEST_ASSERT_EQUAL_INT32(0, p.break_remaining_sec);
}

/* The `state != TIMER_BREAK` half of the secondary rule, pinned over the
   whole enum: only the state in which the break IS the primary event
   suppresses the secondary field. */
void test_from_timer_secondary_rule_across_every_state(void) {
    const timer_state_t states[] = {TIMER_IDLE, TIMER_RUNNING, TIMER_PAUSED, TIMER_EXPIRED, TIMER_BREAK};
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        sleep_plan_in_t p = from((sleep_plan_timer_in_t){.state = states[i],
                                                         .now = NOW,
                                                         .expiry_wall = NOW + EXPIRY_HORIZON,
                                                         .break_active = true,
                                                         .break_remaining_sec = BREAK_HORIZON,
                                                         .extra_running = false});
        int32_t expect = (states[i] == TIMER_BREAK) ? 0 : BREAK_HORIZON;
        TEST_ASSERT_EQUAL_INT32(expect, p.break_remaining_sec);
    }
}

/* The secondary rule asks "is this the state where the break is the
   PRIMARY event", not "is this state below TIMER_BREAK" — so an
   out-of-range state must still take the secondary wake. timer.c's
   snapshot validator happens to reject a restored state above
   TIMER_BREAK today, but that is its guard, not this function's, and a
   pure function's contract may not be inherited from another module's
   internals. Written as `<` instead of `!=` this silently drops the
   chime wake, and nothing else in the suite notices. */
void test_from_timer_secondary_rule_keys_on_break_not_on_ordering(void) {
    sleep_plan_in_t p = from((sleep_plan_timer_in_t){.state = (timer_state_t)(TIMER_BREAK + 1),
                                                     .now = NOW,
                                                     .break_active = true,
                                                     .break_remaining_sec = BREAK_HORIZON,
                                                     .extra_running = false});
    TEST_ASSERT_EQUAL_INT32(BREAK_HORIZON, p.break_remaining_sec);
}

/* Only RUNNING has an expiry and only RUNNING syncs: the clock-only
   states must ignore both readings however loudly they are set. */
void test_from_timer_clock_only_states_have_no_event(void) {
    const timer_state_t states[] = {TIMER_IDLE, TIMER_PAUSED, TIMER_EXPIRED};
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        sleep_plan_in_t p = from((sleep_plan_timer_in_t){
            .state = states[i], .now = NOW, .expiry_wall = NOW + 9999, .ntp_recheck_due = true});
        TEST_ASSERT_EQUAL_INT32(0, p.event_remaining_sec);
        TEST_ASSERT_FALSE(p.sync_due_by_next_wake);
        TEST_ASSERT_EQUAL_INT32(0, p.break_remaining_sec);
    }
}

/* sec_into_minute folds the CLOCK, not either event horizon — both of
   which are held fixed here so a transposition cannot pass. */
void test_from_timer_sec_into_minute_tracks_the_clock(void) {
    const time_t minute_top = NOW - NOW_SEC_INTO_MINUTE; /* :00 */
    for (int sec = 0; sec < 60; sec++) {
        sleep_plan_in_t p = from((sleep_plan_timer_in_t){.state = TIMER_IDLE,
                                                         .now = minute_top + sec,
                                                         .expiry_wall = NOW + EXPIRY_HORIZON,
                                                         .break_active = true,
                                                         .break_remaining_sec = BREAK_HORIZON});
        TEST_ASSERT_EQUAL_INT(sec, p.sec_into_minute);
    }
}

/* End-to-end: the assembly feeds the planner. Catches any field swap
   that happens to keep the struct self-consistent but moves the nap. */
void test_from_timer_feeds_the_planner(void) {
    sleep_plan_timer_in_t running = {
        .state = TIMER_RUNNING, .now = NOW, .expiry_wall = NOW + EXPIRY_HORIZON, .ntp_recheck_due = false};
    sleep_plan_in_t p = sleep_plan_from_timer(&running);
    TEST_ASSERT_EQUAL_INT32(23, sleep_plan_seconds(&p)); /* 323 -> countdown grid */

    sleep_plan_timer_in_t brk = {.state = TIMER_BREAK,
                                 .now = NOW,
                                 .expiry_wall = NOW + 9999,
                                 .break_active = true,
                                 .break_remaining_sec = BREAK_HORIZON};
    p = sleep_plan_from_timer(&brk);
    TEST_ASSERT_EQUAL_INT32(30, sleep_plan_seconds(&p)); /* 100-70 event lead */

    sleep_plan_timer_in_t bg_chimes = {.state = TIMER_PAUSED,
                                       .now = NOW,
                                       .break_active = true,
                                       .break_remaining_sec = BREAK_HORIZON,
                                       .extra_running = false};
    p = sleep_plan_from_timer(&bg_chimes);
    TEST_ASSERT_EQUAL_INT32(30, sleep_plan_seconds(&p)); /* secondary pulls it in */

    sleep_plan_timer_in_t bg_silent = {.state = TIMER_PAUSED,
                                       .now = NOW,
                                       .break_active = true,
                                       .break_remaining_sec = BREAK_HORIZON,
                                       .extra_running = true};
    p = sleep_plan_from_timer(&bg_silent);
    TEST_ASSERT_EQUAL_INT32(43, sleep_plan_seconds(&p)); /* suppressed: wall grid */
}

/* ---- sleep mode: which policy a wake ends under ------------------------ */

static sleep_plan_in_t idle_at(int sec_into_minute) {
    sleep_plan_in_t in = {
        .state = TIMER_IDLE,
        .sec_into_minute = sec_into_minute,
        .event_remaining_sec = 0,
        .sync_due_by_next_wake = false,
        .break_remaining_sec = 0,
    };
    return in;
}

/* Both locks can be engaged at once: bed time engages at night and
   survives in RTC memory, so a later wake can find the battery in the
   lock band while the night is still on. Charge lock has to win — its
   whole point is that the battery cannot afford the 2 h cadence. */
void test_charge_lock_wins_over_bedtime(void) {
    TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_CHARGE_LOCK, wake_sleep_mode_select(true, true, false));
}

void test_mode_select_covers_every_lock_combination(void) {
    for (int cfg = 0; cfg <= 1; cfg++) {
        /* The config-error lock is the LOWEST of the three, so sweeping it
           across every older combination says the two that came first are
           untouched by it — which is the only thing the eight rows below
           could get wrong. */
        TEST_ASSERT_EQUAL_INT(cfg ? WAKE_SLEEP_CONFIG_ERR : WAKE_SLEEP_NORMAL,
                              wake_sleep_mode_select(false, false, cfg));
        TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_CHARGE_LOCK, wake_sleep_mode_select(true, false, cfg));
        TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_BEDTIME, wake_sleep_mode_select(false, true, cfg));
        TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_CHARGE_LOCK, wake_sleep_mode_select(true, true, cfg));
    }
}

/* The config-error lock is last in precedence and that is deliberate in
   both directions. At bed time nobody is editing config and the panel is
   already saying "not in service"; below the charge band a press cannot
   be afforded at all. It only outranks NORMAL. */
void test_the_config_lock_outranks_nothing_but_a_normal_sleep(void) {
    TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_CONFIG_ERR, wake_sleep_mode_select(false, false, true));
    TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_BEDTIME, wake_sleep_mode_select(false, true, true));
    TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_CHARGE_LOCK, wake_sleep_mode_select(true, false, true));
}

/* THE ONE LOCK THAT ARMS A BUTTON, and the difference is the whole
   recovery story: a device whose config cannot be fixed remotely and
   whose buttons are dark needs a serial cable. Design 5.3 keeps D alive
   so the fix is one press away rather than one interval away.

   The interval sits between the other two on purpose. Like the bed-time
   lock (and unlike the charge lock, which opens no window on a re-wake at
   all) this one runs a FULL network window on EVERY wake, so 600 s would
   put a device left broken for a week through ~1000 of them; and unlike
   bed time the fault is expected to be fixed within minutes, so 7200 s
   would be a long wait for an unattended HA edit. D covers the attended
   case, so the interval only has to serve the unattended one. */
void test_the_config_lock_outcome_keeps_the_buttons_armed(void) {
    sleep_plan_in_t in = idle_at(17);
    sleep_outcome_t out = sleep_plan_outcome(WAKE_SLEEP_CONFIG_ERR, &in);
    TEST_ASSERT_TRUE_MESSAGE(out.enable_buttons,
                             "the config lock armed nothing - Button D is dead and so is the device");
    TEST_ASSERT_EQUAL_UINT32(CONFIG_ERR_SLEEP_SEC, out.seconds);
    TEST_ASSERT_EQUAL_UINT32(1800, CONFIG_ERR_SLEEP_SEC);
    TEST_ASSERT_EQUAL_STRING("config error, ", out.reason);
    /* The planner's answer for this state is 43 s (the case below pins the
       same number for the fallback): a fixed interval, not a nap. */
    TEST_ASSERT_NOT_EQUAL_UINT32(43, out.seconds);
}

void test_charge_lock_outcome_is_a_fixed_buttonless_interval(void) {
    sleep_plan_in_t in = idle_at(17);
    sleep_outcome_t out = sleep_plan_outcome(WAKE_SLEEP_CHARGE_LOCK, &in);
    TEST_ASSERT_EQUAL_UINT32(600, out.seconds);
    TEST_ASSERT_FALSE(out.enable_buttons);
    TEST_ASSERT_EQUAL_STRING("charge lock, ", out.reason);
}

void test_bedtime_outcome_is_a_fixed_buttonless_interval(void) {
    sleep_plan_in_t in = idle_at(17);
    sleep_outcome_t out = sleep_plan_outcome(WAKE_SLEEP_BEDTIME, &in);
    TEST_ASSERT_EQUAL_UINT32(7200, out.seconds);
    TEST_ASSERT_FALSE(out.enable_buttons);
    TEST_ASSERT_EQUAL_STRING("bed time, ", out.reason);
}

/* The normal path is the planner, unchanged, with buttons armed. */
void test_normal_outcome_defers_to_the_planner(void) {
    sleep_plan_in_t in = idle_at(17);
    sleep_outcome_t out = sleep_plan_outcome(WAKE_SLEEP_NORMAL, &in);
    TEST_ASSERT_EQUAL_UINT32(43, out.seconds); /* == plan(TIMER_IDLE, 17, ...) */
    TEST_ASSERT_TRUE(out.enable_buttons);
    TEST_ASSERT_EQUAL_STRING("", out.reason);
}

/* A locked wake does no timer work at all, so the planner's answer must
   not leak into it — main.c gathers the readings unconditionally. */
void test_lock_outcomes_ignore_the_planner_input(void) {
    sleep_plan_in_t running = {
        .state = TIMER_RUNNING,
        .sec_into_minute = 30,
        .event_remaining_sec = 3600,
        .sync_due_by_next_wake = true,
        .break_remaining_sec = 90,
    };
    TEST_ASSERT_EQUAL_UINT32(600, sleep_plan_outcome(WAKE_SLEEP_CHARGE_LOCK, &running).seconds);
    TEST_ASSERT_EQUAL_UINT32(7200, sleep_plan_outcome(WAKE_SLEEP_BEDTIME, &running).seconds);
    /* same input, normal mode: the planner really would have said 20 */
    TEST_ASSERT_EQUAL_UINT32(20, sleep_plan_outcome(WAKE_SLEEP_NORMAL, &running).seconds);
}

/* The two soak knobs in include/panic_soak.h, reached from here because
   sleep_plan.h includes it. Both are hand-flipped #defines, so the way
   they fail is by being committed flipped, and neither has any business
   in a shipped image: MAGTAG_PANIC_SOAK_FAST_LOCKS collapses
   BEDTIME_SLEEP_SEC to 90 s (a device waking every ~2 min all night
   through a full network window is a battery problem, not a debugging
   aid), and MAGTAG_PANIC_SOAK turns app_main into a reset loop. The
   header makes them mutually exclusive with an #error; nothing outside
   this assertion notices either one left at 1.

   The interval assertions above would already catch the first. These are
   here so the failure NAMES the knob instead of reading as "the bedtime
   lock changed length", so the OTHER knob is pinned at all - no suite
   asserted it before - and so the rest of the claim is pinned too: the
   fast-lock knob is one duration, and it must not have reached the
   policy that decides WHICH mode a wake ends under, nor whether the
   buttons are armed while a lock holds, nor the charge lock.

   CHARGE_LOCK_SLEEP_SEC is deliberately asserted here even though it is
   now unconditional: an earlier draft of the knob shortened it too, and
   600 is both the battery-recheck margin and the fail-closed fallback
   for an unrecognised mode below. */
void test_the_fast_lock_soak_knob_ships_off_and_touches_only_the_interval(void) {
    TEST_ASSERT_EQUAL_INT(0, MAGTAG_PANIC_SOAK_FAST_LOCKS);
    TEST_ASSERT_EQUAL_INT(0, MAGTAG_PANIC_SOAK);
    TEST_ASSERT_EQUAL_UINT32(600, CHARGE_LOCK_SLEEP_SEC);
    TEST_ASSERT_EQUAL_UINT32(7200, BEDTIME_SLEEP_SEC);

    /* The selection it must not have touched. */
    TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_NORMAL, wake_sleep_mode_select(false, false, false));
    TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_CHARGE_LOCK, wake_sleep_mode_select(true, false, false));
    TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_BEDTIME, wake_sleep_mode_select(false, true, false));
    TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_CHARGE_LOCK, wake_sleep_mode_select(true, true, false));

    /* And the property that makes an HA edit the only exit from a
       bedtime lock: buttons stay dark whatever the interval is. */
    sleep_plan_in_t in = idle_at(17);
    TEST_ASSERT_FALSE(sleep_plan_outcome(WAKE_SLEEP_BEDTIME, &in).enable_buttons);
    TEST_ASSERT_FALSE(sleep_plan_outcome(WAKE_SLEEP_CHARGE_LOCK, &in).enable_buttons);
}

/* An out-of-range mode cannot arise while wake_sleep_mode_select() is the
   only producer, but a cast value would reach the fallback — and it must
   point the safe way. Buttons dark on a lock-length interval, never the
   planner's answer with the buttons armed. */
void test_unknown_mode_fails_closed(void) {
    sleep_plan_in_t in = idle_at(17);
    sleep_outcome_t out = sleep_plan_outcome((wake_sleep_mode_t)99, &in);
    TEST_ASSERT_FALSE(out.enable_buttons);
    TEST_ASSERT_EQUAL_UINT32(600, out.seconds); /* not 43, the planner's answer */
    TEST_ASSERT_EQUAL_STRING("unknown mode, ", out.reason);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_running_never_sleeps_past_the_minute_grid);
    RUN_TEST(test_clock_only_states_stay_minute_aligned);
    RUN_TEST(test_break_secondary_pulls_the_wake_in);
    RUN_TEST(test_break_secondary_ignored_when_later_than_the_primary);
    RUN_TEST(test_break_secondary_ignored_when_zero);
    RUN_TEST(test_break_secondary_never_naps_below_the_minimum);
    RUN_TEST(test_break_secondary_can_beat_a_running_expiry);
    RUN_TEST(test_break_as_primary_state_is_unchanged);
    RUN_TEST(test_clock_states_align_to_minute_boundary);
    RUN_TEST(test_boundary_too_close_takes_following_minute);
    RUN_TEST(test_running_aligns_to_countdown_grid);
    RUN_TEST(test_running_sync_due_wakes_early_on_countdown_grid);
    RUN_TEST(test_event_wake_lands_before_expiry);
    RUN_TEST(test_final_countdown_handoff_sequence);
    RUN_TEST(test_event_wake_clamps_when_event_imminent);
    RUN_TEST(test_event_wake_skipped_when_the_event_is_exactly_now);
    RUN_TEST(test_break_aligns_to_break_grid_no_sync_lead);
    RUN_TEST(test_from_timer_running_takes_expiry_and_sync);
    RUN_TEST(test_from_timer_running_expiry_may_be_negative);
    RUN_TEST(test_from_timer_break_takes_break_remaining_as_the_primary_event);
    RUN_TEST(test_from_timer_background_break_without_extra_running_gets_a_wake);
    RUN_TEST(test_from_timer_background_break_with_extra_running_is_suppressed);
    RUN_TEST(test_from_timer_no_break_active_leaves_the_secondary_zero);
    RUN_TEST(test_from_timer_secondary_rule_across_every_state);
    RUN_TEST(test_from_timer_secondary_rule_keys_on_break_not_on_ordering);
    RUN_TEST(test_from_timer_clock_only_states_have_no_event);
    RUN_TEST(test_from_timer_sec_into_minute_tracks_the_clock);
    RUN_TEST(test_from_timer_feeds_the_planner);
    RUN_TEST(test_charge_lock_wins_over_bedtime);
    RUN_TEST(test_mode_select_covers_every_lock_combination);
    RUN_TEST(test_the_config_lock_outranks_nothing_but_a_normal_sleep);
    RUN_TEST(test_the_config_lock_outcome_keeps_the_buttons_armed);
    RUN_TEST(test_charge_lock_outcome_is_a_fixed_buttonless_interval);
    RUN_TEST(test_bedtime_outcome_is_a_fixed_buttonless_interval);
    RUN_TEST(test_normal_outcome_defers_to_the_planner);
    RUN_TEST(test_lock_outcomes_ignore_the_planner_input);
    RUN_TEST(test_the_fast_lock_soak_knob_ships_off_and_touches_only_the_interval);
    RUN_TEST(test_unknown_mode_fails_closed);
    return UNITY_END();
}
