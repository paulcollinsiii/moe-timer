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
   cannot read is indistinguishable from an empty list.

   display_layout.c is here for ONE function, display_screen_for(), and
   for one test: THE CORRESPONDENCE at the bottom of this file. The chore
   screen draws its "Timers" label unconditionally because the painter's
   gate implies Button A's, and until that test existed nothing anywhere
   checked the two against each other — they live in different modules and
   in different single-TU suites, and each is separately correct. It costs
   nothing to pull in: the file is pure layout math with no LVGL and no
   ESP dependencies, which is what makes it host-testable in the first
   place. */
// clang-format off
#include "mock_hal_time.c"
#include "mock_hal_nvs.c"
#include "../../main/timer.c"
#include "../../main/schedule.c"
#include "../../main/config_validate.c" /* schedule.c judges chore_free pairs with it */
#include "../../main/chores.c"
#include "../../main/chore_store.c"
#include "../../main/button_actions.c"
#include "../../main/display_layout.c"
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

/* ======================================================================
   BUTTONS B, C and D IN CHORE MODE — the ack (design 2.4, 5.1, 5.2)
   ====================================================================== */

/* The list hash the ack record is stamped with, rebuilt from the SAME
   rows with_chores() writes. Not read back out of the names blob,
   deliberately: the point of the stored hash is that the record can be
   checked against a list the caller builds itself, and a helper that
   re-read the blob would pass even if the apply stamped the record with
   something else entirely. */
static uint16_t chore_hash_for(uint8_t n) {
    char names[CHORE_MAX][CHORE_NAME_BUF];
    memset(names, 0, sizeof names);
    for (uint8_t i = 0; i < n && i < CHORE_MAX; i++) {
        snprintf(names[i], CHORE_NAME_BUF, "chore %u", (unsigned)(i + 1));
    }
    return chores_list_hash(names, n);
}

/* The weekday chore_free tranche, in minutes. Invalidates the schedule
   cache because schedule.c reads each key once per wake and T0 is a
   Monday, so a write that landed after the first read would be invisible. */
static void with_chore_free_min(uint16_t minutes) {
    TEST_ASSERT_EQUAL(ESP_OK, hal_nvs_write_u16(NVS_KEY_CHORE_FREE_WD, minutes));
    schedule_cache_invalidate();
}

/* A gated day in chore mode: 60 min allocation, 20 min free, so 2400 s
   are withheld until all three chores are acked. */
static void with_gated_day(void) {
    with_chores(3);
    with_chore_free_min(20);
    timer_set_mode(APP_MODE_CHORES);
}

static chore_ack_t stored_ack(uint8_t n) {
    chore_ack_t out = {0xFF, true}; /* poisoned: a load that writes nothing must not read as {0,false} */
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_load_ack("2026-01-05", chore_hash_for(n), &out));
    return out;
}

/* ---- the gate ---------------------------------------------------------- */

/* Outside chore mode B, C and D keep their timer jobs. The mode byte is
   the routing decision and it is checked here as well as in the wake
   flow: this function is also reached from Button D's own wake arm, which
   is not routed through the dispatch. */
void test_an_ack_is_refused_outside_chore_mode(void) {
    with_chores(3);
    TEST_ASSERT_EQUAL(APP_MODE_TIMERS, timer_mode()); /* the premise */

    TEST_ASSERT_FALSE(button_chore_ack_allowed(BUTTON_CHORE_IDX_B));
    TEST_ASSERT_EQUAL(BTN_ACK_NONE, button_chore_ack_apply(BUTTON_CHORE_IDX_B, T0));
    TEST_ASSERT_EQUAL_UINT8(0, timer_chore_acked());
}

/* Row C1's off switch reaches the ack buttons too. Chore mode is not
   supposed to be reachable with no list, but the mode byte survives deep
   sleep and a list can be emptied from HA under it. */
void test_an_ack_is_refused_with_no_chores_configured(void) {
    timer_set_mode(APP_MODE_CHORES);

    TEST_ASSERT_FALSE(button_chore_ack_allowed(BUTTON_CHORE_IDX_B));
    TEST_ASSERT_EQUAL(BTN_ACK_NONE, button_chore_ack_apply(BUTTON_CHORE_IDX_B, T0));
    TEST_ASSERT_EQUAL_UINT8(0, timer_chore_acked());
}

/* Two chores means no row 3, so D is unlabelled on the panel (design 2.4)
   and must do nothing when pressed. THE BOUND IS THE CONFIGURED COUNT and
   not CHORE_MAX: an out-of-range press that reached chores_toggle_ack()
   would be returned unchanged anyway, but it must not reach flash either,
   because a write is what makes the refusal observable as a real one. */
void test_an_ack_is_refused_for_a_row_that_is_not_configured(void) {
    with_chores(2);
    timer_set_mode(APP_MODE_CHORES);

    TEST_ASSERT_TRUE(button_chore_ack_allowed(BUTTON_CHORE_IDX_C));
    TEST_ASSERT_FALSE(button_chore_ack_allowed(BUTTON_CHORE_IDX_D));
    TEST_ASSERT_EQUAL(BTN_ACK_NONE, button_chore_ack_apply(BUTTON_CHORE_IDX_D, T0));
    TEST_ASSERT_EQUAL_UINT8(0, timer_chore_acked());
    /* nothing was stamped into flash either */
    chore_ack_t rec = {0xFF, true};
    TEST_ASSERT_NOT_EQUAL(ESP_OK, chore_store_load_ack("2026-01-05", chore_hash_for(2), &rec));
}

/* The same invariant Button A carries, for the same reason: this
   predicate is what arms Button C as a wake source (buttons_policy.c), so
   a gate that disagreed with the apply would either arm C for a press
   that is then refused or leave the middle checkbox dead from sleep. */
void test_the_ack_gate_and_the_ack_map_never_disagree(void) {
    for (int chores = 0; chores <= 3; chores++) {
        for (int mode = 0; mode <= 1; mode++) {
            for (uint8_t idx = 0; idx < CHORE_MAX; idx++) {
                timer_reset();
                mock_nvs_reset();
                schedule_cache_invalidate();
                hal_nvs_write_u16("weekday_min", 60);
                with_chores((uint8_t)chores);
                timer_set_mode(mode ? APP_MODE_CHORES : APP_MODE_TIMERS);

                const bool allowed = button_chore_ack_allowed(idx);
                const btn_ack_action_t act = button_chore_ack_apply(idx, T0);
                TEST_ASSERT_EQUAL_MESSAGE(allowed, act != BTN_ACK_NONE,
                                          "the wake-source gate and the ack map disagreed");
            }
        }
    }
}

/* ---- the toggle, RTC and flash (design 5.1) ---------------------------- */

/* B is row 1, C is row 2, D is row 3 — the fixed mapping the three fixed
   rows buy (design 2.4). Each button moves its OWN bit and no other. */
void test_each_ack_button_ticks_its_own_row(void) {
    const uint8_t idx[3] = {BUTTON_CHORE_IDX_B, BUTTON_CHORE_IDX_C, BUTTON_CHORE_IDX_D};
    for (int i = 0; i < 3; i++) {
        timer_reset();
        mock_nvs_reset();
        with_chores(3);
        timer_set_mode(APP_MODE_CHORES);

        TEST_ASSERT_EQUAL(BTN_ACK_TOGGLED, button_chore_ack_apply(idx[i], T0));
        TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)(1u << idx[i]), timer_chore_acked(),
                                        "an ack button moved the wrong row's bit");
    }
}

/* Flash is the AUTHORITY and RTC the working copy (design 5.1), so both
   have to move on every toggle. The record carries today's date and the
   current list hash, which is what lets the next boot believe it. */
void test_an_ack_reaches_rtc_and_flash(void) {
    with_chores(3);
    timer_set_mode(APP_MODE_CHORES);

    TEST_ASSERT_EQUAL(BTN_ACK_TOGGLED, button_chore_ack_apply(BUTTON_CHORE_IDX_C, T0));

    TEST_ASSERT_EQUAL_UINT8(0x02, timer_chore_acked());
    const chore_ack_t rec = stored_ack(3);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x02, rec.acked, "the ack never reached flash");
    TEST_ASSERT_FALSE(rec.released);
}

/* BUG-14: the ack record is ONE slot, so a toggle stamped by a clock that
   was never set ("1970-01-01") would destroy today's record, which the
   synced wake's restore is waiting to bring back. Refused on both ways in:
   the clock itself unset, and a clock NTP has just corrected mid-wake
   while RAM still holds the day the unset clock opened. The no-clock lock
   keeps presses off that day on device; this pins the belt behind it. */
void test_bug14_an_ack_on_an_unset_clock_day_leaves_todays_record_alone(void) {
    with_chores(3);
    timer_set_mode(APP_MODE_CHORES);
    timer_record_date(T0);
    TEST_ASSERT_EQUAL(BTN_ACK_TOGGLED, button_chore_ack_apply(BUTTON_CHORE_IDX_C, T0));
    const int writes = mock_nvs_write_count(NVS_KEY_CHORE_ACK);

    /* Power-on without NTP: RTC day reset and dated by the unset clock. */
    const time_t unset = 90;
    timer_reset();
    timer_record_date(unset);
    timer_set_mode(APP_MODE_CHORES);

    TEST_ASSERT_EQUAL(BTN_ACK_TOGGLED, button_chore_ack_apply(BUTTON_CHORE_IDX_B, unset));
    TEST_ASSERT_EQUAL_UINT8(0x01, timer_chore_acked());
    TEST_ASSERT_EQUAL_INT_MESSAGE(writes, mock_nvs_write_count(NVS_KEY_CHORE_ACK),
                                  "an unset clock stamped the ack record");

    /* NTP lands mid-wake: clock fine, RAM day still the placeholder. */
    TEST_ASSERT_EQUAL(BTN_ACK_TOGGLED, button_chore_ack_apply(BUTTON_CHORE_IDX_D, T0 + 600));
    TEST_ASSERT_EQUAL_INT_MESSAGE(writes, mock_nvs_write_count(NVS_KEY_CHORE_ACK),
                                  "the placeholder day's acks were stamped as today's");

    /* And the clock arm on its own: no day recorded yet, clock unset. */
    timer_reset();
    timer_set_mode(APP_MODE_CHORES);
    TEST_ASSERT_EQUAL(BTN_ACK_TOGGLED, button_chore_ack_apply(BUTTON_CHORE_IDX_B, unset));
    TEST_ASSERT_EQUAL_INT_MESSAGE(writes, mock_nvs_write_count(NVS_KEY_CHORE_ACK), "an unset clock stamped the record");

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x02, stored_ack(3).acked, "today's record was overwritten");
}

/* "Acks toggle, so a mis-press is undone by pressing the same button
   again" (design 2.4). The un-ack has to reach flash too, or a reboot
   restores a tick the kid took back. */
void test_a_second_press_of_the_same_button_un_acks(void) {
    with_chores(3);
    timer_set_mode(APP_MODE_CHORES);

    TEST_ASSERT_EQUAL(BTN_ACK_TOGGLED, button_chore_ack_apply(BUTTON_CHORE_IDX_B, T0));
    TEST_ASSERT_EQUAL(BTN_ACK_TOGGLED, button_chore_ack_apply(BUTTON_CHORE_IDX_B, T0 + 5));

    TEST_ASSERT_EQUAL_UINT8(0, timer_chore_acked());
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, stored_ack(3).acked, "the un-ack never reached flash");
}

/* ---- the release (design 5.2, rows C4-C8) ------------------------------ */

/* THE CASE THE WHOLE FEATURE EXISTS FOR, and the one that kills both
   traps at once.

   The day is 60 min with a 20 min free tranche, so 2400 s are withheld.
   Screen was started on the free tranche and paused with 900 s of it
   left; the third ack has to hand over the other 2400.

   TRAP: the amount must be read while `released` is still FALSE.
   chores_withheld_sec() returns 0 once the flag is latched, so an
   implementation that latches first grants nothing — and every assertion
   that only checks "released is true" still passes. The remaining and
   allocation figures below are what tell the two orders apart. */
void test_the_last_ack_releases_the_withheld_seconds(void) {
    with_gated_day();
    TEST_ASSERT_EQUAL(BTN_B_STARTED, button_b_apply(T0));
    TEST_ASSERT_EQUAL_INT32_MESSAGE(1200, g_rtc_state.slots[0].allocation_sec,
                                    "the premise: a gated start gets the free tranche only");
    timer_pause(T0 + 300); /* 900 s of the free tranche left */

    TEST_ASSERT_EQUAL(BTN_ACK_TOGGLED, button_chore_ack_apply(0, T0 + 400));
    TEST_ASSERT_EQUAL(BTN_ACK_TOGGLED, button_chore_ack_apply(1, T0 + 401));
    TEST_ASSERT_EQUAL_MESSAGE(BTN_ACK_RELEASED, button_chore_ack_apply(2, T0 + 402),
                              "the last ack did not report a release");

    TEST_ASSERT_TRUE(timer_chore_released());
    TEST_ASSERT_EQUAL_INT32_MESSAGE(900 + 2400, g_rtc_state.slots[0].remaining_at_pause,
                                    "the withheld seconds were not granted - was `released` latched first?");
    TEST_ASSERT_EQUAL_INT32_MESSAGE(3600, g_rtc_state.slots[0].allocation_sec,
                                    "the bar's denominator did not go back to the whole day");
}

/* TRAP ONE, design 5.2, verbatim: the release must not be an adjustment.
   adjust_today_sec is the truthful record of what a PARENT asked for and
   is what paints the panel's "(+40 min today)" parenthetical; a gate
   writing into it manufactures an adjustment nobody made, which is
   exactly the bug that field was introduced to fix. This is the case that
   rejects timer_adjust(0, +withheld). */
void test_the_release_is_not_recorded_as_an_adjustment(void) {
    with_gated_day();
    TEST_ASSERT_EQUAL(BTN_B_STARTED, button_b_apply(T0));
    timer_pause(T0 + 300);

    (void)button_chore_ack_apply(0, T0 + 400);
    (void)button_chore_ack_apply(1, T0 + 401);
    TEST_ASSERT_EQUAL(BTN_ACK_RELEASED, button_chore_ack_apply(2, T0 + 402));

    TEST_ASSERT_EQUAL_INT32_MESSAGE(0, timer_slot_adjust_today(0),
                                    "the release was recorded as a screen adjustment (timer_adjust, not "
                                    "timer_release_gated) - it will paint a phantom parenthetical");
}

/* The amount comes from the DAY's Screen allocation (slot 0), never from
   the active slot's. An extra timer's fixed duration is not the day's
   gate: Piano is selected and paused on 900 s here, and if that number
   reached chores_withheld_sec() the grant would be 0 (900 saturates
   against a 1200 s free tranche) instead of 2400. The extra must also be
   left completely alone - timer_release_gated() is slot 0 only. */
void test_the_release_reads_the_days_screen_allocation_not_the_active_slots(void) {
    with_gated_day();
    /* Slot 0 is driven with an EXPLICIT allocation rather than through
       button_b_apply(), so this case says nothing about the start cap and
       cannot be satisfied by it: the two numbers below would otherwise
       coincide under the mutant that reads the active slot's allocation
       (the start would hand slot 0 the whole day, and a release of 0
       would land on the same total). Verified by mutation — that mutant
       was killed by other cases but NOT by this one until the start was
       taken out of it. */
    timer_start(T0, 1200); /* Screen, on the free tranche */
    timer_pause(T0 + 300);
    TEST_ASSERT_EQUAL_INT32(900, g_rtc_state.slots[0].remaining_at_pause);
    g_rtc_state.active_slot = 1; /* Piano, 900 s */
    timer_start(T0 + 310, 900);
    timer_pause(T0 + 320);
    TEST_ASSERT_EQUAL_INT32(900, g_rtc_state.slots[1].allocation_sec); /* the premise */

    (void)button_chore_ack_apply(0, T0 + 400);
    (void)button_chore_ack_apply(1, T0 + 401);
    TEST_ASSERT_EQUAL(BTN_ACK_RELEASED, button_chore_ack_apply(2, T0 + 402));

    TEST_ASSERT_EQUAL_INT32_MESSAGE(900 + 2400, g_rtc_state.slots[0].remaining_at_pause,
                                    "the release was sized from the wrong allocation");
    TEST_ASSERT_EQUAL_INT32_MESSAGE(900, g_rtc_state.slots[1].allocation_sec,
                                    "the release landed on the active slot instead of Screen");
    TEST_ASSERT_EQUAL_INT32(890, g_rtc_state.slots[1].remaining_at_pause);
}

/* Row C8: `released` is LATCHED. Un-ticking a chore after the release
   re-arms nothing and claws nothing back - there is nothing to game. */
void test_un_acking_after_the_release_claws_nothing_back(void) {
    with_gated_day();
    TEST_ASSERT_EQUAL(BTN_B_STARTED, button_b_apply(T0));
    timer_pause(T0 + 300);
    (void)button_chore_ack_apply(0, T0 + 400);
    (void)button_chore_ack_apply(1, T0 + 401);
    TEST_ASSERT_EQUAL(BTN_ACK_RELEASED, button_chore_ack_apply(2, T0 + 402));

    TEST_ASSERT_EQUAL(BTN_ACK_TOGGLED, button_chore_ack_apply(2, T0 + 500)); /* un-tick it */

    TEST_ASSERT_TRUE_MESSAGE(timer_chore_released(), "an un-ack re-locked a released day");
    TEST_ASSERT_EQUAL_INT32_MESSAGE(900 + 2400, g_rtc_state.slots[0].remaining_at_pause,
                                    "an un-ack took the released time back");
    TEST_ASSERT_EQUAL_INT32(3600, g_rtc_state.slots[0].allocation_sec);
}

/* And re-ticking it does not pay out a second time: chores_release_due()
   is false once the flag is latched, so the toggle is an ordinary one. */
void test_re_acking_after_the_release_does_not_grant_twice(void) {
    with_gated_day();
    TEST_ASSERT_EQUAL(BTN_B_STARTED, button_b_apply(T0));
    timer_pause(T0 + 300);
    (void)button_chore_ack_apply(0, T0 + 400);
    (void)button_chore_ack_apply(1, T0 + 401);
    TEST_ASSERT_EQUAL(BTN_ACK_RELEASED, button_chore_ack_apply(2, T0 + 402));
    (void)button_chore_ack_apply(2, T0 + 500);

    TEST_ASSERT_EQUAL_MESSAGE(BTN_ACK_TOGGLED, button_chore_ack_apply(2, T0 + 501),
                              "a re-ack reported a second release");
    TEST_ASSERT_EQUAL_INT32_MESSAGE(900 + 2400, g_rtc_state.slots[0].remaining_at_pause, "the day paid out twice");
}

/* The release flag goes to flash with the acks - one record, one write.
   Without it a reboot mid-day would re-lock a day that had released, and
   the next full ack would pay out a second allocation. */
void test_the_release_is_persisted_with_the_acks(void) {
    with_gated_day();
    (void)button_chore_ack_apply(0, T0);
    (void)button_chore_ack_apply(1, T0 + 1);
    TEST_ASSERT_EQUAL(BTN_ACK_RELEASED, button_chore_ack_apply(2, T0 + 2));

    const chore_ack_t rec = stored_ack(3);
    TEST_ASSERT_EQUAL_UINT8(0x07, rec.acked);
    TEST_ASSERT_TRUE_MESSAGE(rec.released, "the release latch never reached flash");
}

/* The per-day-type off switch (chore_free == allocation): the edge still
   fires and the day still latches, but there is nothing to grant. This is
   chores_release_due()'s documented "a true here must not be read as
   'seconds were granted'". */
void test_a_day_with_the_gate_off_latches_without_granting(void) {
    with_chores(3);
    with_chore_free_min(60); /* == the whole allocation */
    timer_set_mode(APP_MODE_CHORES);
    TEST_ASSERT_EQUAL(BTN_B_STARTED, button_b_apply(T0));
    TEST_ASSERT_EQUAL_INT32(3600, g_rtc_state.slots[0].allocation_sec); /* nothing withheld */
    timer_pause(T0 + 300);

    (void)button_chore_ack_apply(0, T0 + 400);
    (void)button_chore_ack_apply(1, T0 + 401);
    TEST_ASSERT_EQUAL(BTN_ACK_RELEASED, button_chore_ack_apply(2, T0 + 402));

    TEST_ASSERT_TRUE(timer_chore_released());
    TEST_ASSERT_EQUAL_INT32(3300, g_rtc_state.slots[0].remaining_at_pause);
    TEST_ASSERT_EQUAL_INT32(3600, g_rtc_state.slots[0].allocation_sec);
    TEST_ASSERT_EQUAL_INT32(0, timer_slot_adjust_today(0));
}

/* Row C4: the free tranche ran out before the chores were done. The grant
   brings the slot back PAUSED holding it, so the kid presses B - it never
   auto-runs and the expiry alert must not re-fire. */
void test_the_release_onto_an_expired_screen_comes_back_paused(void) {
    with_gated_day();
    TEST_ASSERT_EQUAL(BTN_B_STARTED, button_b_apply(T0));
    timer_tick(T0 + 1300); /* past the 1200 s free tranche */
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());

    (void)button_chore_ack_apply(0, T0 + 1400);
    (void)button_chore_ack_apply(1, T0 + 1401);
    TEST_ASSERT_EQUAL(BTN_ACK_RELEASED, button_chore_ack_apply(2, T0 + 1402));

    TEST_ASSERT_EQUAL(TIMER_PAUSED, timer_get_state());
    TEST_ASSERT_EQUAL_INT32(2400, g_rtc_state.slots[0].remaining_at_pause);
    TEST_ASSERT_EQUAL_INT32(0, timer_slot_adjust_today(0));
}

/* The same row C4, but with the chores done DURING an eye-rest break -
 * and design 2.6 does not merely allow that sequence, it invites it: the
 * free tranche runs out, Screen EXPIRES, the kid runs an extra timer,
 * earns a break, and ticks the boxes while it runs.
 *
 * The slot is then BREAK, not EXPIRED, so the grant used to take the
 * PAUSED/BREAK arm of adjust_core and land in remaining_at_pause;
 * timer_break_tick() put sl->state back to break_prev_state (EXPIRED) and
 * timer_slot_remaining() reports 0 for EXPIRED. Forty minutes into a
 * field nobody reads, `released` latched so it can never be offered
 * again - the day simply ends at zero. Measured before the fix as
 * "DURING rap=2400 alloc=3600 / AFTER state=EXPIRED remaining=0 rap=2400
 * released=1". */
void test_the_release_during_a_break_over_an_expired_screen_survives_the_break(void) {
    with_gated_day();
    TEST_ASSERT_EQUAL(BTN_B_STARTED, button_b_apply(T0));
    timer_tick(T0 + 1300); /* past the 1200 s free tranche */
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());

    timer_start_break(T0 + 1400, 300); /* earned by an extra; parks slot 0 */
    TEST_ASSERT_EQUAL_INT_MESSAGE(TIMER_BREAK, g_rtc_state.slots[0].state, "the fixture is not a break");
    TEST_ASSERT_EQUAL_INT_MESSAGE(TIMER_EXPIRED, g_rtc_state.break_prev_state,
                                  "the break was not entered from an expired screen");

    (void)button_chore_ack_apply(0, T0 + 1500);
    (void)button_chore_ack_apply(1, T0 + 1501);
    TEST_ASSERT_EQUAL(BTN_ACK_RELEASED, button_chore_ack_apply(2, T0 + 1502));

    /* Rule 7: an ack does not cut the break short. The break runs on, and
       the grant is already visible as screen time behind it. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(TIMER_BREAK, g_rtc_state.slots[0].state, "the ack ended the break");
    TEST_ASSERT_EQUAL_INT32(2400, timer_slot_remaining(0, T0 + 1502, 0));

    timer_break_tick(T0 + 1700); /* the break's own end */

    TEST_ASSERT_EQUAL_INT_MESSAGE(TIMER_PAUSED, g_rtc_state.slots[0].state,
                                  "the break handed the slot back EXPIRED and the grant was discarded");
    TEST_ASSERT_EQUAL_INT32_MESSAGE(2400, timer_slot_remaining(0, T0 + 1700, 0),
                                    "the released seconds did not survive the break");
    TEST_ASSERT_EQUAL_INT32(2400, g_rtc_state.slots[0].remaining_at_pause);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(0, timer_slot_adjust_today(0), "the gate wrote into the parent's adjustment");
    /* And it comes back PAUSED rather than RUNNING, exactly as the
       no-break case does: the kid presses B, the expiry alert (already
       heard) does not re-fire. */
    TEST_ASSERT_EQUAL(BTN_B_RESUMED, button_b_apply(T0 + 1800));
}

/* Row C5: the day was never started. timer_release_gated() refuses IDLE
   on purpose - banking the seconds would hand timer_start a second copy
   of a remainder the live allocation already carries - so nothing is
   stored on the slot and the next start simply reads the full day. */
void test_the_release_on_an_idle_screen_banks_nothing(void) {
    with_gated_day();
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state()); /* the premise */

    (void)button_chore_ack_apply(0, T0);
    (void)button_chore_ack_apply(1, T0 + 1);
    TEST_ASSERT_EQUAL(BTN_ACK_RELEASED, button_chore_ack_apply(2, T0 + 2));

    TEST_ASSERT_EQUAL_INT32_MESSAGE(0, g_rtc_state.slots[0].bonus_sec,
                                    "the release banked seconds the live allocation already carries");
    TEST_ASSERT_EQUAL_INT32_MESSAGE(3600, button_b_start_allocation(T0 + 3),
                                    "the released day did not start on the whole allocation");
}

/* ---- the start cap: what makes the withheld seconds actually withheld -- */

/* Design 4.1's worked example is a 60 min day with chore_free 20 and ten
   minutes used, and the panel it draws reads 00:10:00 - the timer holds
   the FREE TRANCHE while the gate is shut, not the whole day. Without
   this the release is not a release at all: the kid already has the whole
   allocation, and finishing the chores would hand them a second one on
   top (chores.h: "either way: alloc, once"). */
void test_a_gated_day_starts_on_the_free_tranche_only(void) {
    with_gated_day();
    TEST_ASSERT_EQUAL_INT32(1200, button_b_start_allocation(T0));
}

/* Once the day has released the cap is gone and the allocation is read
   live, which is the whole of row C5's "no timer call needed". */
void test_a_released_day_starts_on_the_whole_allocation(void) {
    with_gated_day();
    timer_chore_set_released(true);
    TEST_ASSERT_EQUAL_INT32(3600, button_b_start_allocation(T0));
}

/* The shipped fleet: no chore list, so the gate is inert (row C1) and the
   start allocation is exactly what it was before this feature existed.
   Paired with test_idle_screen_starts_with_schedule_allocation above,
   which asserts the same number through the whole B map. */
void test_a_day_with_no_chores_starts_on_the_whole_allocation(void) {
    TEST_ASSERT_EQUAL_INT32(3600, button_b_start_allocation(T0));
    with_chore_free_min(20); /* a stray tranche with no list must not bite */
    TEST_ASSERT_EQUAL_INT32(3600, button_b_start_allocation(T0));
}

/* The gate withholds SCREEN time and only that. An extra's duration comes
   from its def and no chore setting may touch it. */
void test_the_gate_never_caps_an_extra_timers_duration(void) {
    with_gated_day();
    g_rtc_state.active_slot = 1; /* Piano, 900 s */
    TEST_ASSERT_EQUAL_INT32(900, button_b_start_allocation(T0));
}

/* ---- THE CORRESPONDENCE: the painter's gate implies Button A's --------

   display_screens.c draws the chore screen's "Timers" label
   UNCONDITIONALLY, on the stated ground that display_screen_for() has
   already established everything button_a_toggle_allowed() would check
   (display.h). If that ever stops being true the device paints a
   checklist with a dead button on it: the label offers a way out, the
   press is refused, and the kid is stuck on the screen with no feedback
   at all. It is the exact failure button_actions.h forbids a restatement
   for — and display_screen_for() IS a restatement, struck deliberately,
   because the painter is handed a display_state_t snapshot and must not
   reach live state or NVS from the render path.

   Nothing enforced it before this case. The two functions live in
   different modules and, until display_layout.c was pulled into this TU,
   in suites that could not see each other; each is separately correct and
   separately tested, and the IMPLICATION between them was documentation.

   Swept over the whole product space rather than sampled, because the
   divergence this guards against is a term ADDED to one side. M2-T10 adds
   a third device lock; if it gives button_a_toggle_allowed() a new
   refusal reason and display_screen_for() does not learn it, every state
   in which that reason fires lands here. A sampled case would have to
   have guessed the new term in advance.

   ONE ARM ONLY, and the other claimants are deliberately NOT swept here.
   M2-T6 gave the implication a second display-side spelling and
   M2-HW-FIX a third: the break screen labels button A "Chores" off
   `st->chore_count > 0`, and the main screen's button row labels it off
   `timer_state != TIMER_RUNNING && chore_count > 0` (both
   display_screens.c), with the same failure mode and less noise — the
   chore screen at least strands the kid on a screen that says "Timers",
   whereas a dead "Chores" on either painter just does nothing when
   pressed. Neither can be swept from this case, because this suite is a
   single TU and display_screens.c is not in it (see the includes: the
   painters need LVGL, which this suite does not link). Any arm
   added here could only compare button_a_toggle_allowed() against a
   COPY of those literals retyped into the test, which constrains the
   source not at all — a `> 0` -> `> 1` mutation in display_screens.c
   does not even relink this binary. Such an arm was written and removed;
   its rows ({TIMER_BREAK} x {1,2,3}) are in any case a strict subset of
   the rows the sweep below already asserts the same predicate on.

   Where the two painters' gates ARE held: test_display_render, which does
   compile display_screens.c —
   test_the_break_screen_offers_chores_exactly_when_the_list_is_non_empty
   and test_the_main_screen_offers_chores_exactly_when_button_a_would_act
   pin the painted label across the whole boundary of each. What NOTHING
   holds, in either suite, is the cross-check between those gates and
   button_a_toggle_allowed(): the predicate is unavailable in the render
   suite and the painters are unavailable here. So a device lock that adds
   a refusal reason will be caught on the chore screen by the sweep below
   and NOT on either painter by anything, and teaching both painters
   such a lock is a manual obligation of whichever task adds it.

   M2-T10 SHIPPED ITS DEVICE LOCK AND DID NOT TRIGGER THAT OBLIGATION,
   which is why the sentence above is written for a future task and not
   for that one. The config-error lock (design 5.3) adds no refusal
   reason to button_a_toggle_allowed(): it narrows the EXT1 WAKE MASK
   (buttons_policy.c) and leaves the predicate alone, deliberately, so
   that display_screen_for() — a pure layout function — does not grow a
   device-state dependency. Predicate and every display-side spelling
   still agree, so the original framing of the obligation is retired.
   display_screens.c's break-screen comment says the same thing; the two
   are a pair and were out of step for one review cycle.

   AND THIS CASE CANNOT SEE THAT LOCK AT ALL, which is worth stating
   where someone might reach for it as evidence. The sweep below compares
   display_screen_for() against button_a_toggle_allowed(); M2-T10 changed
   neither, this suite never sets the lock flag, and lock_gate.c is not
   linked into this binary. It is green here for reasons that have
   nothing to do with the lock, and it would stay green if the lock were
   deleted. What holds the lock is test_lock_gate (the gate's own
   behaviour, including what is left on the panel before each locked
   sleep) and test_sleep_plan (the mask narrowing). */
void test_the_chore_screen_is_never_painted_where_button_a_would_be_refused(void) {
    const timer_state_t states[] = {TIMER_IDLE, TIMER_RUNNING, TIMER_PAUSED, TIMER_EXPIRED, TIMER_BREAK};
    /* NON-VACUITY, and it is not ceremony here: the assertion lives under
       a `continue`, so a fixture that never reaches the chore screen — a
       seeding helper that stops working, a mode byte that stops sticking
       — would leave this case green with nothing asserted at all. That
       exact shape has shipped on this plan before. The expected figure is
       the product space minus the rows display_screen_for() sends
       elsewhere: 4 non-RUNNING states x 3 non-empty counts = 12. */
    int painted = 0;
    for (size_t i = 0; i < sizeof states / sizeof states[0]; i++) {
        for (uint8_t n = 0; n <= CHORE_MAX; n++) {
            timer_reset();
            mock_nvs_reset();
            hal_nvs_write_u16("weekday_min", 60);
            if (n > 0)
                with_chores(n);
            timer_set_mode(APP_MODE_CHORES);
            g_rtc_state.slots[0].state = (uint8_t)states[i];

            /* The painter is given the SNAPSHOT — the same two values
               app_state.c puts in display_state_t — while the gate reads
               live state and NVS. Feeding both from one fixture is what
               makes the comparison meaningful. */
            if (display_screen_for(timer_get_state(), APP_MODE_CHORES, n) != DISPLAY_SCREEN_CHORES)
                continue;
            painted++;
            TEST_ASSERT_TRUE_MESSAGE(button_a_toggle_allowed(),
                                     "the painter chose the chore screen in a state where Button A is refused - "
                                     "the checklist's Timers label is dead");
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(12, painted, "the sweep never reached the chore screen - the case asserted nothing");
}

/* The converse is NOT asserted, and its absence is deliberate rather than
   an oversight: A being allowed while the timer screen is painted is the
   ordinary Timers-mode case, and it is what lets A be armed to ENTER
   chore mode at all. Only one direction can be true, and it is the one
   the label depends on. */
void test_button_a_is_deliberately_allowed_where_no_checklist_is_painted(void) {
    with_chores(3);
    timer_set_mode(APP_MODE_TIMERS);

    TEST_ASSERT_EQUAL_MESSAGE(DISPLAY_SCREEN_MAIN, display_screen_for(timer_get_state(), APP_MODE_TIMERS, 3),
                              "the fixture is not on the timer screen");
    TEST_ASSERT_TRUE_MESSAGE(button_a_toggle_allowed(), "A cannot be armed to enter chore mode");
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
    /* Buttons B/C/D in chore mode — the ack */
    RUN_TEST(test_an_ack_is_refused_outside_chore_mode);
    RUN_TEST(test_an_ack_is_refused_with_no_chores_configured);
    RUN_TEST(test_an_ack_is_refused_for_a_row_that_is_not_configured);
    RUN_TEST(test_the_ack_gate_and_the_ack_map_never_disagree);
    RUN_TEST(test_each_ack_button_ticks_its_own_row);
    RUN_TEST(test_an_ack_reaches_rtc_and_flash);
    RUN_TEST(test_bug14_an_ack_on_an_unset_clock_day_leaves_todays_record_alone);
    RUN_TEST(test_a_second_press_of_the_same_button_un_acks);
    RUN_TEST(test_the_last_ack_releases_the_withheld_seconds);
    RUN_TEST(test_the_release_is_not_recorded_as_an_adjustment);
    RUN_TEST(test_the_release_reads_the_days_screen_allocation_not_the_active_slots);
    RUN_TEST(test_un_acking_after_the_release_claws_nothing_back);
    RUN_TEST(test_re_acking_after_the_release_does_not_grant_twice);
    RUN_TEST(test_the_release_is_persisted_with_the_acks);
    RUN_TEST(test_a_day_with_the_gate_off_latches_without_granting);
    RUN_TEST(test_the_release_onto_an_expired_screen_comes_back_paused);
    RUN_TEST(test_the_release_during_a_break_over_an_expired_screen_survives_the_break);
    RUN_TEST(test_the_release_on_an_idle_screen_banks_nothing);
    RUN_TEST(test_a_gated_day_starts_on_the_free_tranche_only);
    RUN_TEST(test_a_released_day_starts_on_the_whole_allocation);
    RUN_TEST(test_a_day_with_no_chores_starts_on_the_whole_allocation);
    RUN_TEST(test_the_gate_never_caps_an_extra_timers_duration);
    RUN_TEST(test_the_chore_screen_is_never_painted_where_button_a_would_be_refused);
    RUN_TEST(test_button_a_is_deliberately_allowed_where_no_checklist_is_painted);
    return UNITY_END();
}
