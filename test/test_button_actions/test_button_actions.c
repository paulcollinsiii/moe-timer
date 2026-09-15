#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unity.h>

/* Single-TU compilation: mocks, then the modules under test.

   chores.c and chore_store.c come along for Button A: its second refusal
   reason is "no chores configured", and that count lives in NVS and
   nowhere else (the RTC block holds the acks, the release and the mode —
   timer.h — but no count). Compiling the REAL store rather than stubbing
   it is what makes "no chores configured" here mean the same thing it
   means on device, including the case that matters most: every failure
   mode of chore_store_load_names() reports n = 0, so a blob this firmware
   cannot read is indistinguishable from an empty list. */
// clang-format off
#include "mock_hal_time.c"
#include "mock_hal_nvs.c"
#include "../../main/timer.c"
#include "../../main/schedule.c"
#include "../../main/chores.c"
#include "../../main/chore_store.c"
#include "../../main/button_actions.c"
// clang-format on

/* 2026-01-05 00:00:00 UTC (Monday) */
#define T0 ((time_t)1767571200)

static const timer_def_t TEST_DEFS[TIMER_SLOT_COUNT] = {
    {"Screen", 0, false, false},   {"Piano", 900, true, true},   {"", 0, false, false}, /* disabled */
    {"Laundry", 600, true, false}, {"Violin", 900, false, true},
};

void setUp(void) {
    mock_nvs_reset();
    schedule_cache_invalidate();
    setenv("TZ", "UTC0", 1);
    tzset();
    hal_nvs_write_u16("weekday_min", 60);
    timer_set_defs(TEST_DEFS, TIMER_SLOT_COUNT);
    timer_reset();
    mock_time_set(T0);
}

void tearDown(void) {}

void test_idle_screen_starts_with_schedule_allocation(void) {
    TEST_ASSERT_EQUAL(BTN_B_STARTED, button_b_apply(T0));
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
    TEST_ASSERT_EQUAL_INT32(3600, g_rtc_state.slots[0].allocation_sec);
}

void test_idle_extra_slot_starts_with_def_duration(void) {
    g_rtc_state.active_slot = 1; /* Piano, 900 s */
    TEST_ASSERT_EQUAL(BTN_B_STARTED, button_b_apply(T0));
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
    TEST_ASSERT_EQUAL_INT32(900, g_rtc_state.slots[1].allocation_sec);
}

void test_running_pauses(void) {
    timer_start(T0, 3600);
    TEST_ASSERT_EQUAL(BTN_B_PAUSED, button_b_apply(T0 + 60));
    TEST_ASSERT_EQUAL(TIMER_PAUSED, timer_get_state());
}

void test_paused_resumes(void) {
    timer_start(T0, 3600);
    timer_pause(T0 + 60);
    TEST_ASSERT_EQUAL(BTN_B_RESUMED, button_b_apply(T0 + 120));
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
}

void test_break_is_none_and_stays_break(void) {
    timer_start(T0, 3600);
    timer_start_break(T0 + 60, 300);
    TEST_ASSERT_EQUAL(BTN_B_NONE, button_b_apply(T0 + 90));
    TEST_ASSERT_EQUAL(TIMER_BREAK, timer_get_state());
}

/* Screen (slot 0) carries no def, so timer_reload_allowed() is false for
   it and the EXPIRED->reload leg can never fire: a kid cannot reset their
   own screen timer. This is the property PARENT_TESTING=n used to protect
   with a build flag; it is now structural. */
void test_expired_screen_never_reloads(void) {
    timer_start(T0, 60);
    timer_tick(T0 + 120); /* RUNNING -> EXPIRED */
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
    TEST_ASSERT_FALSE(timer_reload_allowed());
    TEST_ASSERT_EQUAL(BTN_B_NONE, button_b_apply(T0 + 130));
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
}

/* EXPIRED is the one state where B has no start/pause/resume job, so it
   is where Reload lives (design 2.3). */
void test_expired_reloadable_extra_reloads_to_full_duration(void) {
    g_rtc_state.active_slot = 1; /* Piano, 900 s, reloadable */
    timer_start(T0, 60);
    timer_tick(T0 + 120); /* RUNNING -> EXPIRED */
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
    TEST_ASSERT_EQUAL(BTN_B_RELOADED, button_b_apply(T0 + 130));
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state());
    /* "Full duration" is the def's, realized at the next start — the same
       press again now starts a whole fresh 900 s Piano. */
    TEST_ASSERT_EQUAL(BTN_B_STARTED, button_b_apply(T0 + 140));
    TEST_ASSERT_EQUAL_INT32(900, g_rtc_state.slots[1].allocation_sec);
}

void test_expired_non_reloadable_extra_is_none(void) {
    g_rtc_state.active_slot = 4; /* Violin, 900 s, NOT reloadable */
    timer_start(T0, 60);
    timer_tick(T0 + 120); /* RUNNING -> EXPIRED */
    TEST_ASSERT_EQUAL(BTN_B_NONE, button_b_apply(T0 + 130));
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
    TEST_ASSERT_EQUAL_INT32(60, g_rtc_state.slots[4].allocation_sec); /* untouched */
}

/* Ordering: the reload leg is evaluated BEFORE the timer_start_allowed()
   break gate, because a reload is not a start. Laundry is reloadable but
   NOT break-eligible, so the gate would refuse it — and today's direct
   wake_flow dispatch, gated only by timer_reload_allowed(), reloads it
   anyway. That behaviour has to survive the remap. */
void test_expired_reloadable_extra_reloads_during_a_screen_break(void) {
    timer_start(T0, 3600);           /* Screen RUNNING */
    timer_start_break(T0 + 60, 900); /* Screen Break, ends T0+960 */
    g_rtc_state.active_slot = 3;     /* Laundry, 600 s, reloadable, not break-eligible */
    timer_start(T0 + 70, 60);
    timer_tick(T0 + 200); /* RUNNING -> EXPIRED */
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
    TEST_ASSERT_FALSE(timer_start_allowed()); /* the start gate WOULD refuse */
    TEST_ASSERT_EQUAL(BTN_B_RELOADED, button_b_apply(T0 + 210));
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state());
    TEST_ASSERT_EQUAL(TIMER_BREAK, timer_slot_state(0)); /* the break is untouched */
}

void test_start_allocation_prefers_def_duration(void) {
    g_rtc_state.active_slot = 3; /* Laundry, 600 s */
    TEST_ASSERT_EQUAL_INT32(600, button_b_start_allocation(T0));
    g_rtc_state.active_slot = 0; /* Screen: schedule-fed */
    TEST_ASSERT_EQUAL_INT32(3600, button_b_start_allocation(T0));
}

/* ======================================================================
   BUTTON A — the mode toggle (design 4.2, plan row M2-T3, row C3)
   ====================================================================== */

/* The predicate has exactly two refusal reasons and both of them have to
   suppress the WAKE SOURCE as well as the action, which is why it is a
   function of its own rather than an `if` inside button_a_apply(): the
   sleep-entry call site in buttons.c hands the same answer to
   buttons_policy_wake_mask(), and a press that could only be refused must
   not burn a wake and a panel refresh. One definition, two callers — the
   failure this shape exists to prevent is the two drifting apart, which
   reads in the field either as a button that wakes the device to do
   nothing or, worse, as one that is armed at sleep and refused on arrival. */

static void with_chores(uint8_t n) {
    char names[CHORE_MAX][CHORE_NAME_BUF];
    memset(names, 0, sizeof names);
    for (uint8_t i = 0; i < n && i < CHORE_MAX; i++) {
        snprintf(names[i], CHORE_NAME_BUF, "chore %u", (unsigned)(i + 1));
    }
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_save_names(names, n));
}

void test_a_toggles_timers_to_chores(void) {
    with_chores(3);
    TEST_ASSERT_EQUAL(APP_MODE_TIMERS, timer_mode()); /* the premise */
    TEST_ASSERT_EQUAL(BTN_A_CHORES, button_a_apply());
    TEST_ASSERT_EQUAL(APP_MODE_CHORES, timer_mode());
}

void test_a_toggles_chores_back_to_timers(void) {
    with_chores(3);
    timer_set_mode(APP_MODE_CHORES);
    TEST_ASSERT_EQUAL(BTN_A_TIMERS, button_a_apply());
    TEST_ASSERT_EQUAL(APP_MODE_TIMERS, timer_mode());
}

/* The toggle really is a toggle and not a one-way trip: two presses come
   back where they started. */
void test_two_presses_of_a_return_to_the_starting_mode(void) {
    with_chores(1);
    TEST_ASSERT_EQUAL(BTN_A_CHORES, button_a_apply());
    TEST_ASSERT_EQUAL(BTN_A_TIMERS, button_a_apply());
    TEST_ASSERT_EQUAL(APP_MODE_TIMERS, timer_mode());
}

/* WHY button_a_apply() compares against APP_MODE_CHORES and not
   APP_MODE_TIMERS. The two look interchangeable and are not, because the
   stored byte is NOT clamped (timer.h says the RTC field is deliberately
   not the place to bound it) while every painter treats anything that is
   not APP_MODE_CHORES as Timers (display.h). So an out-of-enum byte — a
   corrupted RTC word, a future enumerator written by an older image — is
   ALREADY being painted as the timer screen.

   Compare against CHORES, as the code does: 7 != CHORES, so the press
   selects CHORES, the painted screen changes, and the press visibly did
   something. Compare against TIMERS instead: 7 != TIMERS, so the press
   selects TIMERS — the same screen that was already showing. The first
   press out of a corrupted mode would then be a no-op on the glass and
   the user would need two, which is the "dead button" reading this
   milestone is trying not to ship.

   This case is the one that tells the two forms apart; the ordinary
   toggle cases above pass under either, which is how the APP_MODE_TIMERS
   form survived the whole suite once. Also carried as a mutant in
   m2t3-mutate.py with this test as its expected killer. */
void test_an_out_of_enum_mode_recovers_on_a_single_press(void) {
    with_chores(3);
    timer_set_mode((app_mode_t)7); /* not clamped: stored verbatim */
    TEST_ASSERT_EQUAL_MESSAGE(7, (int)timer_mode(), "the premise: the stored byte really is unclamped");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(APP_MODE_CHORES, (int)timer_mode(),
                                  "the premise: an out-of-enum byte paints as the timer screen");

    TEST_ASSERT_EQUAL_MESSAGE(BTN_A_CHORES, button_a_apply(),
                              "one press from a corrupted mode did not select the chore screen");
    TEST_ASSERT_EQUAL_MESSAGE(APP_MODE_CHORES, timer_mode(),
                              "the press landed on the screen that was already painted - it reads as a dead button");

    /* and it is a toggle again from here, not stuck */
    TEST_ASSERT_EQUAL(BTN_A_TIMERS, button_a_apply());
    TEST_ASSERT_EQUAL(APP_MODE_TIMERS, timer_mode());
}

/* ---- refusal 1: the active slot is RUNNING (design 4.2, row C3) -------- */

void test_a_is_refused_while_the_active_slot_is_running(void) {
    with_chores(3);
    timer_start(T0, 3600);
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state()); /* the premise */

    TEST_ASSERT_FALSE(button_a_toggle_allowed());
    TEST_ASSERT_EQUAL(BTN_A_NONE, button_a_apply());
    TEST_ASSERT_EQUAL(APP_MODE_TIMERS, timer_mode());
}

/* BLANKET, and deliberately so: the gate refuses the way OUT of chore
   mode as well as the way in. That is what licenses dropping A from the
   wake mask while a timer runs — design 4.2's "a press that could only be
   refused must not burn a wake" is only true if BOTH directions are
   refused. An asymmetric gate would make A a live wake source for every
   RUNNING minute the device spends in chore mode.

   Not a trap: B still pauses (which reopens A on the next press), and the
   day rollover reverts the mode through timer_reset()'s memset. */
void test_a_is_refused_while_running_in_chore_mode_too(void) {
    with_chores(3);
    timer_set_mode(APP_MODE_CHORES);
    timer_start(T0, 3600);

    TEST_ASSERT_EQUAL(BTN_A_NONE, button_a_apply());
    TEST_ASSERT_EQUAL_MESSAGE(APP_MODE_CHORES, timer_mode(), "the RUNNING gate let the toggle out of chore mode");
}

/* Every state that is not RUNNING allows it. Swept rather than sampled
   because the gate is written as a single `== TIMER_RUNNING` comparison,
   and the mutation that matters most is `!=`. */
void test_every_non_running_state_allows_the_toggle(void) {
    const timer_state_t ok[] = {TIMER_IDLE, TIMER_PAUSED, TIMER_EXPIRED, TIMER_BREAK};
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        timer_reset();
        with_chores(3);
        g_rtc_state.slots[0].state = (uint8_t)ok[i];
        TEST_ASSERT_EQUAL_MESSAGE(ok[i], timer_get_state(), "fixture did not reach the state under test");
        TEST_ASSERT_TRUE_MESSAGE(button_a_toggle_allowed(), "a non-RUNNING state refused the mode toggle");
    }
}

/* Design 4.2 in one case: BREAK IS NOT RUNNING, so chore mode is reachable
   throughout a break — which is exactly where 2.6 wants it. Driven through
   the real timer_start_break() rather than by poking the slot, because the
   whole claim is about the state a real break leaves behind. */
void test_a_screen_break_is_not_running_so_chore_mode_is_reachable(void) {
    with_chores(3);
    timer_start(T0, 3600);
    timer_start_break(T0 + 60, 900);
    TEST_ASSERT_EQUAL(TIMER_BREAK, timer_get_state()); /* the premise */

    TEST_ASSERT_TRUE(button_a_toggle_allowed());
    TEST_ASSERT_EQUAL(BTN_A_CHORES, button_a_apply());
}

/* The other half of the same design bullet: a BACKGROUND break with an
   extra timer running blocks it, correctly — the kid is at the violin.
   The break lives on slot 0 and the RUNNING extra is the ACTIVE slot, so
   this is the case that proves the gate reads the active slot rather than
   slot 0. */
void test_a_background_break_with_an_extra_running_refuses(void) {
    with_chores(3);
    timer_start(T0, 3600);
    timer_start_break(T0 + 60, 900);
    g_rtc_state.active_slot = 1; /* Piano, break-eligible */
    timer_start(T0 + 70, 900);
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state()); /* the active slot */
    TEST_ASSERT_EQUAL(TIMER_BREAK, timer_slot_state(0)); /* still on behind it */

    TEST_ASSERT_FALSE(button_a_toggle_allowed());
    TEST_ASSERT_EQUAL(BTN_A_NONE, button_a_apply());
}

/* THE HALF OF timer_swap_allowed() THAT MUST NOT BE HERE. Design 4.2 says
   the predicate is that function's RUNNING half "without that function's
   second condition that extras must exist" — so a device with no extra
   timers configured, where C is dead and swap_allowed is false, must
   still reach the chore screen. Copying timer_swap_allowed() wholesale is
   the obvious implementation and this is the only case that rejects it. */
void test_the_gate_is_not_timer_swap_allowed(void) {
    const timer_def_t screen_only[TIMER_SLOT_COUNT] = {
        {"Screen", 0, false, false}, {"", 0, false, false}, {"", 0, false, false},
        {"", 0, false, false},       {"", 0, false, false},
    };
    timer_set_defs(screen_only, TIMER_SLOT_COUNT);
    timer_reset();
    with_chores(3);

    TEST_ASSERT_FALSE_MESSAGE(timer_swap_allowed(), "fixture still has extras: the case cannot fail");
    TEST_ASSERT_TRUE_MESSAGE(button_a_toggle_allowed(), "the mode gate inherited swap_allowed's extras condition");
    TEST_ASSERT_EQUAL(BTN_A_CHORES, button_a_apply());
}

/* ---- refusal 2: no chores configured (row C1) -------------------------- */

/* The shipped default on every device in the field: the names blob has
   never been written, so there is no checklist to paint and A has nothing
   to switch to. */
void test_a_is_refused_with_no_chores_configured(void) {
    TEST_ASSERT_FALSE(button_a_toggle_allowed());
    TEST_ASSERT_EQUAL(BTN_A_NONE, button_a_apply());
    TEST_ASSERT_EQUAL(APP_MODE_TIMERS, timer_mode());
}

/* A list emptied from Home Assistant is a written blob with n = 0, which
   is a different byte sequence from "never written" and reaches
   chore_store_load_names() down a different branch. Both must refuse. */
void test_an_emptied_list_refuses_too(void) {
    with_chores(3);
    TEST_ASSERT_TRUE(button_a_toggle_allowed()); /* the premise */
    with_chores(0);

    TEST_ASSERT_FALSE(button_a_toggle_allowed());
    TEST_ASSERT_EQUAL(BTN_A_NONE, button_a_apply());
}

/* One configured chore is a list. The count is a `> 0` test and not a
   `== CHORE_MAX` one. */
void test_a_single_configured_chore_is_enough(void) {
    with_chores(1);
    TEST_ASSERT_TRUE(button_a_toggle_allowed());
    TEST_ASSERT_EQUAL(BTN_A_CHORES, button_a_apply());
}

/* The inherited cost, written down in design 4.2 and NOT fixed here: an
   unreadable blob reports n = 0 exactly like an empty list, so a transient
   NVS fault refuses the toggle for as long as it lasts. What this case
   pins is that the binding does not AMPLIFY it — a refusal leaves the
   stored mode exactly where it was, so the next good read restores the
   button rather than the mode having been overwritten in the meantime. */
void test_an_unreadable_blob_refuses_without_touching_the_stored_mode(void) {
    with_chores(3);
    timer_set_mode(APP_MODE_CHORES);
    /* A blob this firmware cannot read: right key, wrong length. */
    const uint8_t junk[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    TEST_ASSERT_EQUAL(ESP_OK, hal_nvs_write_blob(NVS_KEY_CHORES, junk, sizeof junk));

    TEST_ASSERT_FALSE(button_a_toggle_allowed());
    TEST_ASSERT_EQUAL(BTN_A_NONE, button_a_apply());
    TEST_ASSERT_EQUAL_MESSAGE(APP_MODE_CHORES, timer_mode(), "a refused press rewrote the stored mode");
}

/* ---- the two callers must agree ---------------------------------------- */

/* The invariant the wake mask rests on, swept over the whole cross product
   of the two refusal reasons: whatever button_a_toggle_allowed() says at
   sleep entry is what button_a_apply() will do on arrival. A predicate
   that drifted from the map would arm A for a press the map then refuses
   (a wake and a refresh spent on nothing) or drop A for a press the map
   would have honoured (a dead button). */
void test_the_gate_and_the_map_never_disagree(void) {
    for (int running = 0; running <= 1; running++) {
        for (int configured = 0; configured <= 1; configured++) {
            timer_reset();
            mock_nvs_reset();
            with_chores(configured ? 2 : 0);
            if (running) {
                timer_start(T0, 3600);
            }
            const bool allowed = button_a_toggle_allowed();
            const btn_a_action_t act = button_a_apply();
            TEST_ASSERT_EQUAL_MESSAGE(allowed, act != BTN_A_NONE,
                                      "the sleep-entry gate and the button map disagreed about Button A");
        }
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_idle_screen_starts_with_schedule_allocation);
    RUN_TEST(test_idle_extra_slot_starts_with_def_duration);
    RUN_TEST(test_running_pauses);
    RUN_TEST(test_paused_resumes);
    RUN_TEST(test_break_is_none_and_stays_break);
    RUN_TEST(test_expired_screen_never_reloads);
    RUN_TEST(test_expired_reloadable_extra_reloads_to_full_duration);
    RUN_TEST(test_expired_non_reloadable_extra_is_none);
    RUN_TEST(test_expired_reloadable_extra_reloads_during_a_screen_break);
    RUN_TEST(test_start_allocation_prefers_def_duration);
    /* Button A — the mode toggle */
    RUN_TEST(test_a_toggles_timers_to_chores);
    RUN_TEST(test_a_toggles_chores_back_to_timers);
    RUN_TEST(test_two_presses_of_a_return_to_the_starting_mode);
    RUN_TEST(test_an_out_of_enum_mode_recovers_on_a_single_press);
    RUN_TEST(test_a_is_refused_while_the_active_slot_is_running);
    RUN_TEST(test_a_is_refused_while_running_in_chore_mode_too);
    RUN_TEST(test_every_non_running_state_allows_the_toggle);
    RUN_TEST(test_a_screen_break_is_not_running_so_chore_mode_is_reachable);
    RUN_TEST(test_a_background_break_with_an_extra_running_refuses);
    RUN_TEST(test_the_gate_is_not_timer_swap_allowed);
    RUN_TEST(test_a_is_refused_with_no_chores_configured);
    RUN_TEST(test_an_emptied_list_refuses_too);
    RUN_TEST(test_a_single_configured_chore_is_enough);
    RUN_TEST(test_an_unreadable_blob_refuses_without_touching_the_stored_mode);
    RUN_TEST(test_the_gate_and_the_map_never_disagree);
    return UNITY_END();
}
