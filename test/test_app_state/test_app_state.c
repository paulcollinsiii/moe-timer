#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unity.h>

/* Single-TU: the state-assembly rules over the real timer, schedule and
   nvs_config modules (mock HAL underneath); ADC/app-descriptor reads are
   injected through app_state_in_t. */
// clang-format off
#include "mock_hal_time.c"
#include "mock_hal_nvs.c"
#include "../../main/timer.c"
#include "../../main/schedule.c"
#include "../../main/nvs_config.c"
#include "../../main/battery_soc.c"
#include "../../main/battery_policy.c"
#include "../../main/app_state.c"
// clang-format on

/* Base timestamp: 2026-01-05 00:00:00 UTC (Monday) */
#define T0 ((time_t)1767571200)

/* Slot 2 left disabled to prove the 0/0 stats rule. */
static const timer_def_t TEST_DEFS[TIMER_SLOT_COUNT] = {
    {"Screen", 0, false, false}, {"Piano", 900, true, true},
    {"", 0, false, false},       {"Laundry", 600, false, false}, /* not break-eligible */
    {"", 0, false, false},
};

static const app_state_in_t IN_HEALTHY = {
    .batt_mv = 4100, /* well above the warn band */
    .light_mv = 321,
    .charge_locked = false,
    .fw_version = "1.2.3",
    .reset_reason = "DEEPSLEEP",
};

static const app_state_in_t IN_LOW_BATT = {
    .batt_mv = 3200, /* deep in the warn band */
    .light_mv = 0,
    .charge_locked = true,
    .fw_version = "1.2.3",
    .reset_reason = "PANIC",
};

void setUp(void) {
    mock_nvs_reset();
    schedule_cache_invalidate();
    setenv("TZ", "UTC0", 1);
    tzset();
    hal_nvs_write_u16("weekday_min", 60);
    hal_nvs_write_u16("weekend_min", 120);
    timer_set_defs(TEST_DEFS, TIMER_SLOT_COUNT);
    timer_reset();
    mock_time_set(T0);
}

void tearDown(void) {}

static void select_slot(int slot) {
    while (timer_active_slot() != slot) {
        TEST_ASSERT_TRUE(timer_select_next());
    }
}

/* ---- display state ------------------------------------------------------ */

void test_display_idle_shows_full_allocation(void) {
    /* Monday: weekday 60 min. The 0 passed as remaining must be replaced
       by the full allocation so the IDLE bar renders full. */
    display_state_t st = app_state_display(&IN_HEALTHY, 0, T0);
    TEST_ASSERT_EQUAL_INT32(3600, st.remaining_sec);
    TEST_ASSERT_EQUAL_UINT32(3600, st.allocation_sec);
    TEST_ASSERT_EQUAL(TIMER_IDLE, st.timer_state);
    TEST_ASSERT_NULL(st.timer_name); /* Screen renders no mode line */
}

/* ---- the day's default vs today's adjustment ----------------------------

   An adjustment applied while Screen is IDLE sits in the bank until
   timer_start folds it, so today's REMAINING has to add it — otherwise a
   -45 set from HA in the morning shows nothing on the panel until the kid
   presses B, which reads exactly like the set never landed.

   allocation_sec must NOT absorb it, though: it is the day's default, and
   the status line renders it as such. Folding the two together made a
   60-minute weekday with -45 applied say "Weekday - 15 min", a figure
   that is neither the day's default nor anything anyone configured. */
void test_display_idle_splits_the_days_default_from_the_adjustment(void) {
    timer_bonus_reconcile(0, -2700); /* HA: -45 min, Screen still IDLE */
    display_state_t st = app_state_display(&IN_HEALTHY, 0, T0);
    TEST_ASSERT_EQUAL_UINT32(3600, st.allocation_sec); /* a weekday is still 60 min */
    TEST_ASSERT_EQUAL_INT32(-2700, st.adjust_sec);
    TEST_ASSERT_EQUAL_INT32(900, st.remaining_sec); /* ...but only 15 min of it left */
}

void test_display_no_adjustment_reports_zero(void) {
    /* The common case, and the one the render goldens pin: with nothing
       adjusted the status line must render exactly as it did before the
       field existed. */
    display_state_t st = app_state_display(&IN_HEALTHY, 0, T0);
    TEST_ASSERT_EQUAL_INT32(0, st.adjust_sec);
    TEST_ASSERT_EQUAL_UINT32(3600, st.allocation_sec);
    TEST_ASSERT_EQUAL_INT32(3600, st.remaining_sec);
}

/* The adjustment has to survive timer_start folding bonus_sec away —
   otherwise the parenthetical vanishes the moment the kid presses B,
   while the clock beside it still counts the adjusted day. Derived as
   (today's limit - the day's default) rather than read off the bank,
   precisely so the start fold cannot lose it. */
void test_display_adjustment_survives_the_start_fold(void) {
    timer_bonus_reconcile(0, -1800);                        /* -30 min, banked */
    timer_start(T0, 3600);                                  /* button_actions passes the schedule figure */
    TEST_ASSERT_EQUAL_INT32(0, timer_slot_banked_bonus(0)); /* bank is gone... */
    display_state_t st = app_state_display(&IN_HEALTHY, 1800, T0);
    TEST_ASSERT_EQUAL_UINT32(3600, st.allocation_sec);
    TEST_ASSERT_EQUAL_INT32(-1800, st.adjust_sec); /* ...the line still says -30 */
}

void test_display_adjustment_covers_a_grant_landing_mid_run(void) {
    timer_start(T0, 3600);
    timer_adjust(0, 600); /* +10 min while RUNNING: in place, never banked */
    display_state_t st = app_state_display(&IN_HEALTHY, 4200, T0);
    TEST_ASSERT_EQUAL_UINT32(3600, st.allocation_sec);
    TEST_ASSERT_EQUAL_INT32(600, st.adjust_sec);
}

void test_display_idle_adjustment_clamps_a_deduction_past_zero(void) {
    /* Same clamp timer_start applies: a bank deeper than the allocation
       shows an empty day, never a negative one — and the line reports
       what the day actually LOST (60), not what was asked for (120), so
       default + adjustment always equals the remaining on screen. */
    timer_bonus_reconcile(0, -7200); /* -120 min against a 60 min day */
    display_state_t st = app_state_display(&IN_HEALTHY, 0, T0);
    TEST_ASSERT_EQUAL_UINT32(3600, st.allocation_sec);
    TEST_ASSERT_EQUAL_INT32(-3600, st.adjust_sec);
    TEST_ASSERT_EQUAL_INT32(0, st.remaining_sec);
}

/* The clamp is a DISPLAY clamp, and it has to hold in every state, not
   just IDLE: the stored total records what was asked for (-120 min), and
   only the seam that has to make three numbers on one screen agree
   truncates it to what the day could actually lose. */
void test_display_clamps_a_deduction_past_zero_while_running(void) {
    timer_start(T0, 3600);
    timer_adjust(0, -7200); /* -120 min against a 60 min day */
    display_state_t st = app_state_display(&IN_HEALTHY, 0, T0 + 60);
    TEST_ASSERT_EQUAL_UINT32(3600, st.allocation_sec);
    TEST_ASSERT_EQUAL_INT32(-3600, st.adjust_sec); /* not -7200 */
    TEST_ASSERT_EQUAL_INT32(0, st.remaining_sec);
    /* ...while the timer keeps the truthful record. */
    TEST_ASSERT_EQUAL_INT32(-7200, timer_slot_adjust_today(0));
}

/* A grant is never clamped — the bar pins full and the line says +240. */
void test_display_does_not_clamp_a_grant(void) {
    timer_bonus_reconcile(0, 14400);
    display_state_t st = app_state_display(&IN_HEALTHY, 0, T0);
    TEST_ASSERT_EQUAL_UINT32(3600, st.allocation_sec);
    TEST_ASSERT_EQUAL_INT32(14400, st.adjust_sec);
    TEST_ASSERT_EQUAL_INT32(18000, st.remaining_sec);
}

void test_display_idle_extra_timer_reports_its_own_bank(void) {
    /* A cmd grant can bank on any slot; the split reads the ACTIVE slot's
       bank, not slot 0's. */
    select_slot(1);                  /* Piano, 15 min */
    timer_adjust(1, 300);            /* +5 min banked while IDLE */
    timer_bonus_reconcile(0, -2700); /* Screen's bank must not leak in */
    display_state_t st = app_state_display(&IN_HEALTHY, 0, T0);
    TEST_ASSERT_EQUAL_UINT32(900, st.allocation_sec); /* Piano's configured duration */
    TEST_ASSERT_EQUAL_INT32(300, st.adjust_sec);
    TEST_ASSERT_EQUAL_INT32(1200, st.remaining_sec);
    TEST_ASSERT_EQUAL_STRING("Piano", st.timer_name);
}

/* ---- the adjustment is TRACKED, never derived ---------------------------

   adjust_sec once came out of (today's effective limit - the day's
   default). Those two are read at different TIMES: the default is read
   LIVE from config/schedule on every paint, while the effective limit was
   frozen into the slot at timer_start. Anything that moves the default
   underneath a running day therefore manufactured an adjustment nobody
   made — and, worse, rewrote a real one. The tests below are the four
   ways that happened; every one of them printed a parenthetical where the
   pre-split panel printed nothing at all. */

void test_display_no_phantom_adjustment_when_the_parent_edits_the_limit(void) {
    /* Screen starts on a 60 min weekday; the parent then raises HA's
       "weekday minutes" to 90. Nothing was adjusted, so nothing may be
       reported — the derivation read "Weekday - 90 min (-30 min today)". */
    timer_start(T0, 3600);
    hal_nvs_write_u16("weekday_min", 90);
    schedule_cache_invalidate();
    display_state_t st = app_state_display(&IN_HEALTHY, 3600, T0);
    TEST_ASSERT_EQUAL_UINT32(5400, st.allocation_sec);
    TEST_ASSERT_EQUAL_INT32(0, st.adjust_sec);
}

void test_display_no_phantom_adjustment_when_the_day_type_changes_mid_run(void) {
    /* A holiday landing for today (or school dates flipping to Summer)
       moves the day's default under a run that already started. The
       derivation read the whole difference as an adjustment: "Weekend -
       120 min (-60 min today)". */
    timer_start(T0, 3600); /* Monday, 60 min */
    display_state_t st = app_state_display(&IN_HEALTHY, 3600, T0 + 5 * 86400 /* Saturday */);
    TEST_ASSERT_EQUAL_UINT32(7200, st.allocation_sec); /* the weekend default */
    TEST_ASSERT_EQUAL_INT32(0, st.adjust_sec);
}

void test_display_a_real_adjustment_is_not_rewritten_by_a_limit_edit(void) {
    /* The damaging half: -30 was genuinely applied and folded at start,
       and then the weekday limit moved 60 -> 90. The derivation reported
       "(-60 min today)" for an adjustment that was -30. */
    timer_bonus_reconcile(0, -1800);
    timer_start(T0, 3600);
    hal_nvs_write_u16("weekday_min", 90);
    schedule_cache_invalidate();
    display_state_t st = app_state_display(&IN_HEALTHY, 1800, T0);
    TEST_ASSERT_EQUAL_UINT32(5400, st.allocation_sec);
    TEST_ASSERT_EQUAL_INT32(-1800, st.adjust_sec); /* what was actually applied */
}

void test_display_break_on_a_never_started_screen_reports_no_adjustment(void) {
    /* timer_start_break moves slot 0 to BREAK without ever running it, so
       its allocation_sec is still 0 while the day's default is 3600. The
       derivation answered -3600 — invisible only because the break screen
       happens to draw no mode line, which is not a property this field
       may rely on. */
    timer_start_break(T0, 900);
    display_state_t st = app_state_display(&IN_HEALTHY, 0, T0);
    TEST_ASSERT_EQUAL(TIMER_BREAK, st.timer_state);
    TEST_ASSERT_EQUAL_INT32(0, st.adjust_sec);
}

/* An adjustment against a slot the panel is NOT drawing must not surface
   on it: the field is per-slot, like every other one on this struct. */
void test_display_adjustment_is_read_from_the_drawn_slot_only(void) {
    select_slot(1);       /* Piano is what the panel draws */
    timer_adjust(0, 600); /* Screen, off-screen */
    display_state_t st = app_state_display(&IN_HEALTHY, 0, T0);
    TEST_ASSERT_EQUAL_INT32(0, st.adjust_sec);
}

/* Repeatable cmd-topic grants accumulate: two +10s are a +20 day, not a
   +10 day. (timer_bonus_reconcile's HA number is a TARGET and settles at
   the target; the cmd topic is a delta and stacks.) */
void test_display_adjustment_accumulates_across_grants(void) {
    timer_adjust(0, 600);
    timer_adjust(0, 600);
    display_state_t st = app_state_display(&IN_HEALTHY, 0, T0);
    TEST_ASSERT_EQUAL_INT32(1200, st.adjust_sec);
    TEST_ASSERT_EQUAL_UINT32(3600, st.allocation_sec);
    TEST_ASSERT_EQUAL_INT32(4800, st.remaining_sec);
}

/* The day rollover is where the day's adjustment goes. */
void test_display_adjustment_clears_at_the_day_rollover(void) {
    timer_bonus_reconcile(0, -1800);
    timer_reset();
    display_state_t st = app_state_display(&IN_HEALTHY, 0, T0);
    TEST_ASSERT_EQUAL_INT32(0, st.adjust_sec);
}

/* ---- the chore gate's release, seen from the display layer --------------

   timer_release_gated is the second writer into slot 0 and the only one
   that moves time WITHOUT recording an adjustment, which makes it the one
   caller able to manufacture the state display.h rules out: adjust_sec ==
   0 is documented to mean "the day is running on its default, and the
   status line then renders exactly as it did before this field existed",
   so a remaining_sec above allocation_sec + adjust_sec renders 90 minutes
   against a 60 minute day with no parenthetical to explain it — and the
   bar, which divides by the default, draws 150%.

   timer.c's IDLE refusal is what keeps that unreachable, and bonus_sec is
   RTC-persisted, so without it the overgrant survives deep sleep. This is
   the display half of that guard; the timer half lives in test_timer. */
static void assert_remaining_within_the_day(display_state_t st) {
    TEST_ASSERT_TRUE_MESSAGE(st.remaining_sec <= (int32_t)st.allocation_sec + st.adjust_sec,
                             "display.h: remaining_sec above allocation_sec + adjust_sec");
}

void test_display_gate_release_cannot_outrun_the_days_default(void) {
    /* C5's day: Screen still IDLE, so the day's allocation is read live at
       the next start and the gate's own latch already makes it full. */
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state());
    TEST_ASSERT_TRUE(timer_release_gated(1800)); /* true = repaint, not "it moved" */

    display_state_t st = app_state_display(&IN_HEALTHY, 0, T0);
    TEST_ASSERT_EQUAL_UINT32(3600, st.allocation_sec); /* Monday, 60 min */
    TEST_ASSERT_EQUAL_INT32(0, st.adjust_sec);         /* no parent asked for anything */
    TEST_ASSERT_EQUAL_INT32(3600, st.remaining_sec);   /* not 5400 */
    assert_remaining_within_the_day(st);

    /* ...and across the day's first start, which is where a banked release
       would have surfaced: timer_start folds bonus_sec, so the 90-minute
       day appears here and nowhere earlier. */
    timer_start(T0, 3600);
    st = app_state_display(&IN_HEALTHY, timer_tick(T0), T0);
    TEST_ASSERT_EQUAL_UINT32(3600, st.allocation_sec);
    TEST_ASSERT_EQUAL_INT32(0, st.adjust_sec);
    TEST_ASSERT_EQUAL_INT32(3600, st.remaining_sec);
    assert_remaining_within_the_day(st);
}

void test_display_running_passes_remaining_through(void) {
    timer_start(T0, 3600);
    display_state_t st = app_state_display(&IN_HEALTHY, 1234, T0 + 100);
    TEST_ASSERT_EQUAL_INT32(1234, st.remaining_sec);
    TEST_ASSERT_EQUAL(TIMER_RUNNING, st.timer_state);
}

void test_display_extra_timer_uses_def_duration_and_name(void) {
    select_slot(1);
    display_state_t st = app_state_display(&IN_HEALTHY, 0, T0);
    TEST_ASSERT_EQUAL_UINT32(900, st.allocation_sec);
    TEST_ASSERT_EQUAL_STRING("Piano", st.timer_name);
    TEST_ASSERT_TRUE(st.reloadable);
}

void test_display_charge_warn_tracks_battery_band(void) {
    TEST_ASSERT_FALSE(app_state_display(&IN_HEALTHY, 0, T0).charge_warn);
    TEST_ASSERT_TRUE(app_state_display(&IN_LOW_BATT, 0, T0).charge_warn);
}

void test_display_fw_version_passes_through(void) {
    /* The battery row renders it; app_state is the only place the injected
       app-descriptor string reaches display_state_t, so a dropped
       assignment would silently blank the version on the panel. */
    TEST_ASSERT_EQUAL_STRING("1.2.3", app_state_display(&IN_HEALTHY, 0, T0).fw_version);
    app_state_in_t other = IN_HEALTHY;
    other.fw_version = "9.9.9-rc4";
    TEST_ASSERT_EQUAL_STRING("9.9.9-rc4", app_state_display(&other, 0, T0).fw_version);
}

void test_display_break_duration_from_nvs(void) {
    hal_nvs_write_u16("break_dur", 20);
    display_state_t st = app_state_display(&IN_HEALTHY, 0, T0);
    TEST_ASSERT_EQUAL_UINT32(20 * 60, st.break_duration_sec);
}

void test_display_swap_blocked_while_running(void) {
    TEST_ASSERT_TRUE(app_state_display(&IN_HEALTHY, 0, T0).swap_available);
    timer_start(T0, 3600);
    TEST_ASSERT_FALSE(app_state_display(&IN_HEALTHY, 0, T0).swap_available);
}

void test_display_reload_follows_the_timer_gate(void) {
    /* Screen carries no def, so it is never reloadable; Piano is. No
       build flag can turn Screen on — it is refused by construction. */
    TEST_ASSERT_FALSE(app_state_display(&IN_HEALTHY, 0, T0).reload_available);
    select_slot(1); /* Piano (reloadable) */
    TEST_ASSERT_TRUE(app_state_display(&IN_HEALTHY, 0, T0).reload_available);
}

/* ---- background Screen Break --------------------------------------------
   The break lives on slot 0 and keeps running while another timer is
   selected, so the render state must carry it across the swap. */

/* Screen runs from T0, break starts T0+600 and ends T0+1500. */
static void arm_break(void) {
    timer_start(T0, 3600);
    timer_start_break(T0 + 600, 900);
}

void test_display_break_remaining_survives_the_swap_to_an_extra(void) {
    arm_break();
    TEST_ASSERT_TRUE(timer_select_next()); /* -> Piano */
    display_state_t st = app_state_display(&IN_HEALTHY, 900, T0 + 800);
    TEST_ASSERT_EQUAL_INT32(700, st.break_remaining_sec);
    TEST_ASSERT_EQUAL(TIMER_IDLE, st.timer_state); /* Piano's own state */
    TEST_ASSERT_EQUAL_STRING("Piano", st.timer_name);
}

void test_display_break_banner_only_when_screen_is_not_selected(void) {
    /* Screen selected: the break SCREEN is drawn, so no chip (it would be
       drawing the same fact twice, in a layout that has no header). */
    arm_break();
    TEST_ASSERT_FALSE(app_state_display(&IN_HEALTHY, 0, T0 + 800).break_banner);
    TEST_ASSERT_TRUE(timer_select_next()); /* -> Piano: main layout + chip */
    TEST_ASSERT_TRUE(app_state_display(&IN_HEALTHY, 0, T0 + 800).break_banner);
}

void test_display_break_banner_false_without_a_break(void) {
    TEST_ASSERT_TRUE(timer_select_next()); /* Piano, no break anywhere */
    display_state_t st = app_state_display(&IN_HEALTHY, 0, T0);
    TEST_ASSERT_FALSE(st.break_banner);
    TEST_ASSERT_EQUAL_INT32(0, st.break_remaining_sec);
}

void test_display_break_banner_drops_when_the_break_ends(void) {
    arm_break();
    TEST_ASSERT_TRUE(timer_select_next());
    timer_break_tick(T0 + 1500);
    TEST_ASSERT_TRUE(timer_break_take_ended(T0 + 1500, NULL));
    display_state_t st = app_state_display(&IN_HEALTHY, 0, T0 + 1500);
    TEST_ASSERT_FALSE(st.break_banner);
    TEST_ASSERT_EQUAL_INT32(0, st.break_remaining_sec);
}

void test_display_swap_next_name_is_the_slot_c_would_pick(void) {
    /* Screen selected: C lands on Piano (slot 1) */
    TEST_ASSERT_EQUAL_STRING("Piano", app_state_display(&IN_HEALTHY, 0, T0).swap_next_name);
    TEST_ASSERT_TRUE(timer_select_next()); /* -> Piano; next is Laundry (2 disabled) */
    TEST_ASSERT_EQUAL_STRING("Laundry", app_state_display(&IN_HEALTHY, 0, T0).swap_next_name);
}

void test_display_swap_next_name_null_without_extras(void) {
    static const timer_def_t NO_EXTRAS[TIMER_SLOT_COUNT] = {
        {"Screen", 0, false, false}, {"", 0, false, false}, {"", 0, false, false},
        {"", 0, false, false},       {"", 0, false, false},
    };
    timer_set_defs(NO_EXTRAS, TIMER_SLOT_COUNT);
    TEST_ASSERT_NULL(app_state_display(&IN_HEALTHY, 0, T0).swap_next_name);
}

void test_display_swap_available_during_a_break(void) {
    /* The chip's companion: C is live during a break (the whole point). */
    arm_break();
    TEST_ASSERT_TRUE(app_state_display(&IN_HEALTHY, 0, T0 + 800).swap_available);
}

/* ---- start_available: the break gates Button B per slot ---- */

void test_display_start_available_outside_a_break(void) {
    /* No break: every slot is startable, so the play glyph always shows. */
    TEST_ASSERT_TRUE(app_state_display(&IN_HEALTHY, 0, T0).start_available);
    TEST_ASSERT_TRUE(timer_select_next()); /* -> Piano */
    TEST_ASSERT_TRUE(app_state_display(&IN_HEALTHY, 0, T0).start_available);
}

void test_display_start_available_is_false_for_screen_during_a_break(void) {
    arm_break();
    TEST_ASSERT_FALSE(app_state_display(&IN_HEALTHY, 0, T0 + 800).start_available);
}

void test_display_start_available_tracks_break_eligible_during_a_break(void) {
    arm_break();
    TEST_ASSERT_TRUE(timer_select_next()); /* -> Piano: break-eligible */
    TEST_ASSERT_TRUE(app_state_display(&IN_HEALTHY, 0, T0 + 800).start_available);
    TEST_ASSERT_TRUE(timer_select_next()); /* -> Laundry: a chore, refused */
    display_state_t st = app_state_display(&IN_HEALTHY, 0, T0 + 800);
    TEST_ASSERT_FALSE(st.start_available);
    TEST_ASSERT_TRUE(st.swap_available); /* still reachable by C (rule 8) */
    TEST_ASSERT_TRUE(st.break_banner);
}

/* The break screen's swap hint must promise a timer you can actually
   start. With only non-eligible extras the break offers nothing, so the
   hint is suppressed and the break falls back to its centred footer. */
void test_display_swap_hint_suppressed_when_no_eligible_extra(void) {
    static const timer_def_t ONLY_A_CHORE[TIMER_SLOT_COUNT] = {
        {"Screen", 0, false, false},    {"", 0, false, false}, {"", 0, false, false},
        {"Laundry", 600, false, false}, {"", 0, false, false},
    };
    timer_set_defs(ONLY_A_CHORE, TIMER_SLOT_COUNT);
    /* Outside a break the hint is not drawn at all, so it stays honest. */
    TEST_ASSERT_EQUAL_STRING("Laundry", app_state_display(&IN_HEALTHY, 0, T0).swap_next_name);
    arm_break();
    TEST_ASSERT_NULL(app_state_display(&IN_HEALTHY, 0, T0 + 800).swap_next_name);
}

void test_display_swap_hint_survives_when_an_eligible_extra_exists(void) {
    arm_break();
    TEST_ASSERT_EQUAL_STRING("Piano", app_state_display(&IN_HEALTHY, 0, T0 + 800).swap_next_name);
}

/* ---- stats snapshot ----------------------------------------------------- */

void test_stats_disabled_slot_reports_zero_zero(void) {
    stats_snapshot_t s;
    app_state_stats(&IN_HEALTHY, T0, &s);
    TEST_ASSERT_EQUAL_INT32(0, s.remaining_s[2]);
    TEST_ASSERT_EQUAL_UINT32(0, s.allocation_s[2]);
}

void test_stats_idle_screen_falls_back_to_schedule(void) {
    stats_snapshot_t s;
    app_state_stats(&IN_HEALTHY, T0, &s);
    TEST_ASSERT_EQUAL_UINT32(3600, s.allocation_s[0]);
    TEST_ASSERT_EQUAL_INT32(3600, s.remaining_s[0]);
    TEST_ASSERT_EQUAL_STRING("Screen", s.active_timer);
    TEST_ASSERT_EQUAL_STRING("IDLE", s.state);
    TEST_ASSERT_EQUAL_STRING("Weekday", s.day_type);
}

/* The HA limit/remaining sensors read the same IDLE fallback the panel
   does, and must fold the bank for the same reason. */
void test_stats_idle_screen_allocation_folds_a_banked_adjustment(void) {
    timer_bonus_reconcile(0, -2700);
    stats_snapshot_t s;
    app_state_stats(&IN_HEALTHY, T0, &s);
    TEST_ASSERT_EQUAL_UINT32(900, s.allocation_s[0]);
    TEST_ASSERT_EQUAL_INT32(900, s.remaining_s[0]);
}

void test_stats_idle_screen_allocation_clamps_at_zero(void) {
    timer_bonus_reconcile(0, -7200);
    stats_snapshot_t s;
    app_state_stats(&IN_HEALTHY, T0, &s);
    TEST_ASSERT_EQUAL_UINT32(0, s.allocation_s[0]);
    TEST_ASSERT_EQUAL_INT32(0, s.remaining_s[0]);
}

/* allocation_s[] is uint32_t and the fallback is computed signed, so a
   negative that escapes the clamp is not a small error — it publishes
   4294963696 as "Screen time limit" and every automation reading it
   believes the day has 49700 hours in it. Pinned as a magnitude, not
   just as == 0, so a future split that reintroduces the signed path
   cannot pass by coincidence. */
void test_stats_allocation_never_wraps_through_the_uint32_cast(void) {
    timer_bonus_reconcile(0, -86400); /* -24 h against a 60 min day */
    stats_snapshot_t s;
    app_state_stats(&IN_HEALTHY, T0, &s);
    TEST_ASSERT_EQUAL_UINT32(0, s.allocation_s[0]);
    TEST_ASSERT_TRUE_MESSAGE(s.allocation_s[0] <= 86400u, "allocation wrapped through the uint32 cast");
}

/* The display split is a DISPLAY split: the HA sensors keep publishing
   the effective limit, because that is what an automation asking "how
   much screen time is there today" wants. If these ever start reporting
   the day's default, STATS_JSON_DISC_SCHEMA_VER owes a bump. */
void test_stats_allocation_stays_the_effective_limit_not_the_default(void) {
    timer_bonus_reconcile(0, -1800); /* -30 min against a 60 min day */
    stats_snapshot_t s;
    app_state_stats(&IN_HEALTHY, T0, &s);
    TEST_ASSERT_EQUAL_UINT32(1800, s.allocation_s[0]);
    TEST_ASSERT_EQUAL_INT32(1800, s.remaining_s[0]);
    /* ...while the panel is told 60 min with a -30 today. */
    display_state_t st = app_state_display(&IN_HEALTHY, 0, T0);
    TEST_ASSERT_EQUAL_UINT32(3600, st.allocation_sec);
    TEST_ASSERT_EQUAL_INT32(-1800, st.adjust_sec);
}

void test_stats_started_slot_allocation_includes_grant(void) {
    timer_start(T0, 3600);
    timer_adjust(0, 300); /* HA grant mid-run */
    stats_snapshot_t s;
    app_state_stats(&IN_HEALTHY, T0 + 600, &s);
    TEST_ASSERT_EQUAL_UINT32(3900, s.allocation_s[0]);
    TEST_ASSERT_EQUAL_INT32(3300, s.remaining_s[0]);
    TEST_ASSERT_EQUAL_STRING("RUNNING", s.state);
}

void test_stats_completions_map_extra_slots(void) {
    select_slot(1);
    timer_start(T0, 900);
    timer_tick(T0 + 901); /* expire: Piano completion #1 */
    stats_snapshot_t s;
    app_state_stats(&IN_HEALTHY, T0 + 902, &s);
    TEST_ASSERT_EQUAL_UINT16(1, s.completions[0]); /* [0] = slot 1 */
    TEST_ASSERT_EQUAL_UINT16(0, s.completions[2]);
    TEST_ASSERT_EQUAL_STRING("Piano", s.active_timer);
}

void test_stats_injected_device_fields_pass_through(void) {
    stats_snapshot_t s;
    app_state_stats(&IN_LOW_BATT, T0, &s);
    TEST_ASSERT_EQUAL_INT(3200, s.batt_mv);
    TEST_ASSERT_TRUE(s.charge_lock);
    TEST_ASSERT_EQUAL_STRING("1.2.3", s.fw);
    TEST_ASSERT_EQUAL_STRING("PANIC", s.reset_reason);
    TEST_ASSERT_EQUAL_INT(0, s.light_mv);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_display_idle_shows_full_allocation);
    RUN_TEST(test_display_idle_splits_the_days_default_from_the_adjustment);
    RUN_TEST(test_display_no_adjustment_reports_zero);
    RUN_TEST(test_display_adjustment_survives_the_start_fold);
    RUN_TEST(test_display_adjustment_covers_a_grant_landing_mid_run);
    RUN_TEST(test_display_idle_adjustment_clamps_a_deduction_past_zero);
    RUN_TEST(test_display_clamps_a_deduction_past_zero_while_running);
    RUN_TEST(test_display_does_not_clamp_a_grant);
    RUN_TEST(test_display_idle_extra_timer_reports_its_own_bank);
    RUN_TEST(test_display_no_phantom_adjustment_when_the_parent_edits_the_limit);
    RUN_TEST(test_display_no_phantom_adjustment_when_the_day_type_changes_mid_run);
    RUN_TEST(test_display_a_real_adjustment_is_not_rewritten_by_a_limit_edit);
    RUN_TEST(test_display_break_on_a_never_started_screen_reports_no_adjustment);
    RUN_TEST(test_display_adjustment_is_read_from_the_drawn_slot_only);
    RUN_TEST(test_display_adjustment_accumulates_across_grants);
    RUN_TEST(test_display_adjustment_clears_at_the_day_rollover);
    RUN_TEST(test_display_gate_release_cannot_outrun_the_days_default);
    RUN_TEST(test_display_running_passes_remaining_through);
    RUN_TEST(test_display_extra_timer_uses_def_duration_and_name);
    RUN_TEST(test_display_charge_warn_tracks_battery_band);
    RUN_TEST(test_display_fw_version_passes_through);
    RUN_TEST(test_display_break_duration_from_nvs);
    RUN_TEST(test_display_swap_blocked_while_running);
    RUN_TEST(test_display_reload_follows_the_timer_gate);
    RUN_TEST(test_display_break_remaining_survives_the_swap_to_an_extra);
    RUN_TEST(test_display_break_banner_only_when_screen_is_not_selected);
    RUN_TEST(test_display_break_banner_false_without_a_break);
    RUN_TEST(test_display_break_banner_drops_when_the_break_ends);
    RUN_TEST(test_display_swap_next_name_is_the_slot_c_would_pick);
    RUN_TEST(test_display_swap_next_name_null_without_extras);
    RUN_TEST(test_display_swap_available_during_a_break);
    RUN_TEST(test_display_start_available_outside_a_break);
    RUN_TEST(test_display_start_available_is_false_for_screen_during_a_break);
    RUN_TEST(test_display_start_available_tracks_break_eligible_during_a_break);
    RUN_TEST(test_display_swap_hint_suppressed_when_no_eligible_extra);
    RUN_TEST(test_display_swap_hint_survives_when_an_eligible_extra_exists);
    RUN_TEST(test_stats_disabled_slot_reports_zero_zero);
    RUN_TEST(test_stats_idle_screen_falls_back_to_schedule);
    RUN_TEST(test_stats_idle_screen_allocation_folds_a_banked_adjustment);
    RUN_TEST(test_stats_idle_screen_allocation_clamps_at_zero);
    RUN_TEST(test_stats_allocation_never_wraps_through_the_uint32_cast);
    RUN_TEST(test_stats_allocation_stays_the_effective_limit_not_the_default);
    RUN_TEST(test_stats_started_slot_allocation_includes_grant);
    RUN_TEST(test_stats_completions_map_extra_slots);
    RUN_TEST(test_stats_injected_device_fields_pass_through);
    return UNITY_END();
}
