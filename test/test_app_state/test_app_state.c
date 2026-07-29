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
    .parent_testing = false,
    .fw_version = "1.2.3",
    .reset_reason = "DEEPSLEEP",
};

static const app_state_in_t IN_LOW_BATT = {
    .batt_mv = 3200, /* deep in the warn band */
    .light_mv = 0,
    .charge_locked = true,
    .parent_testing = false,
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

void test_display_reload_follows_parent_testing_gate(void) {
    /* Screen (not reloadable) only resets under ParentTesting */
    TEST_ASSERT_FALSE(app_state_display(&IN_HEALTHY, 0, T0).reload_available);
    app_state_in_t parent = IN_HEALTHY;
    parent.parent_testing = true;
    TEST_ASSERT_TRUE(app_state_display(&parent, 0, T0).reload_available);
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

/* ---- start_available: the break gates Button A per slot ---- */

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
    RUN_TEST(test_display_running_passes_remaining_through);
    RUN_TEST(test_display_extra_timer_uses_def_duration_and_name);
    RUN_TEST(test_display_charge_warn_tracks_battery_band);
    RUN_TEST(test_display_break_duration_from_nvs);
    RUN_TEST(test_display_swap_blocked_while_running);
    RUN_TEST(test_display_reload_follows_parent_testing_gate);
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
    RUN_TEST(test_stats_started_slot_allocation_includes_grant);
    RUN_TEST(test_stats_completions_map_extra_slots);
    RUN_TEST(test_stats_injected_device_fields_pass_through);
    return UNITY_END();
}
