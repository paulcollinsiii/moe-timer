#include <string.h>
#include <time.h>
#include <unity.h>

/* Single-TU: the real timer state machine under the net-window apply
   orchestration; net_window + mqtt_ha are mocked below, device effects
   are counted through the injected ops. */
// clang-format off
#include "../../main/timer.c"
#include "mock_hal_time.c"
// clang-format on

#include "net_window.h"

/* ---- net_window mock ---------------------------------------------------- */

static bool mock_nw_active;
static bool mock_nw_spawn_ok;
static bool mock_nw_wait_ntp_ok;
static bool mock_nw_join_ok;
static esp_err_t mock_nw_ntp_result;
static int64_t mock_nw_clock_step;
static int mock_nw_spawn_calls, mock_nw_post_calls, mock_nw_join_polls;

bool net_window_spawn(void) {
    if (!mock_nw_spawn_ok)
        return false;
    mock_nw_spawn_calls++;
    mock_nw_active = true;
    return true;
}

bool net_window_wait_ntp(void) {
    return mock_nw_active && mock_nw_wait_ntp_ok;
}

void net_window_post_snapshot(const stats_snapshot_t *snap) {
    (void)snap;
    mock_nw_post_calls++;
}

bool net_window_join(int timeout_ms, void (*poll_cb)(void)) {
    (void)timeout_ms;
    if (poll_cb != NULL) {
        poll_cb();
        mock_nw_join_polls++;
    }
    if (!mock_nw_join_ok)
        return false; /* wedged: stays active */
    mock_nw_active = false;
    return true;
}

bool net_window_active(void) {
    return mock_nw_active;
}

esp_err_t net_window_ntp_result(void) {
    return mock_nw_ntp_result;
}

int64_t net_window_take_clock_step(void) {
    int64_t step = mock_nw_clock_step;
    mock_nw_clock_step = 0;
    return step;
}

void net_window_log_last(void) {}

/* ---- mqtt_ha mock (the take-accessors net_apply consumes) --------------- */

static bool mock_ha_bonus_pending;
static int32_t mock_ha_bonus_target;
static bool mock_ha_grant_pending;
static int mock_ha_grant_slot;
static int32_t mock_ha_grant_sec;
static bool mock_ha_locate;

bool mqtt_ha_take_bonus_target(int32_t *target_sec) {
    if (!mock_ha_bonus_pending)
        return false;
    mock_ha_bonus_pending = false;
    *target_sec = mock_ha_bonus_target;
    return true;
}

bool mqtt_ha_take_grant(int *slot, int32_t *sec) {
    if (!mock_ha_grant_pending)
        return false;
    mock_ha_grant_pending = false;
    *slot = mock_ha_grant_slot;
    *sec = mock_ha_grant_sec;
    return true;
}

bool mqtt_ha_locate_pending(void) {
    bool p = mock_ha_locate;
    mock_ha_locate = false;
    return p;
}

/* ---- timer_defs_install mock: the "post-edit" table ---------------------- */

static timer_def_t mock_installed_defs[TIMER_SLOT_COUNT];
static int mock_installed_count;

void timer_defs_install(void) {
    timer_set_defs(mock_installed_defs, mock_installed_count);
}

// clang-format off
#include "../../main/net_apply.c"
// clang-format on

/* ---- injected device effects: counters only ----------------------------- */

static int n_chirp, n_expiry_alert, n_config_applied, n_post_stats, n_locate, n_join_poll;

static void ops_join_poll(void) {
    n_join_poll++;
}
static void ops_config_applied(void) {
    n_config_applied++;
}
static void ops_chirp(void) {
    n_chirp++;
}
static void ops_expiry_alert(void) {
    n_expiry_alert++;
}
static void ops_post_stats(void) {
    n_post_stats++;
}
static void ops_locate(void) {
    n_locate++;
}

/* Base timestamp: 2026-01-05 00:00:00 UTC (Monday) */
#define T0 ((time_t)1767571200)

/* Pre-window table: slot 1 Piano 15 min reloadable, slot 2 disabled,
   slot 3 Meditation 10 min. */
static const timer_def_t PRE_DEFS[TIMER_SLOT_COUNT] = {
    {"Screen", 0, false, false},     {"Piano", 900, true, true}, {"", 0, false, false},
    {"Meditation", 600, true, true}, {"", 0, false, false},
};

static void install_table(const timer_def_t *defs) {
    memcpy(mock_installed_defs, defs, sizeof(mock_installed_defs));
    mock_installed_count = TIMER_SLOT_COUNT;
}

void setUp(void) {
    install_table(PRE_DEFS);
    timer_set_defs(PRE_DEFS, TIMER_SLOT_COUNT);
    timer_reset();
    mock_time_set(T0);

    mock_nw_active = false;
    mock_nw_spawn_ok = true;
    mock_nw_wait_ntp_ok = true;
    mock_nw_join_ok = true;
    mock_nw_ntp_result = ESP_OK;
    mock_nw_clock_step = 0;
    mock_nw_spawn_calls = mock_nw_post_calls = mock_nw_join_polls = 0;

    mock_ha_bonus_pending = false;
    mock_ha_grant_pending = false;
    mock_ha_locate = false;

    n_chirp = n_expiry_alert = n_config_applied = n_post_stats = n_locate = n_join_poll = 0;

    static const net_apply_ops_t ops = {
        .join_poll = ops_join_poll,
        .on_config_applied = ops_config_applied,
        .on_active_reset_chirp = ops_chirp,
        .on_active_expired_alert = ops_expiry_alert,
        .post_stats = ops_post_stats,
        .on_locate = ops_locate,
    };
    net_apply_init(&ops);
}

void tearDown(void) {}

/* ---- no-window / failure paths ----------------------------------------- */

void test_finish_without_window_is_idle_and_consumes_nothing(void) {
    mock_ha_grant_pending = true;
    mock_ha_grant_slot = 0;
    mock_ha_grant_sec = 300;
    TEST_ASSERT_EQUAL(NET_FINISH_IDLE, net_apply_finish());
    TEST_ASSERT_TRUE(mock_ha_grant_pending); /* untouched: no window, no apply */
    TEST_ASSERT_EQUAL_INT(0, n_config_applied);
}

void test_finish_on_wedged_join_is_idle_and_applies_nothing(void) {
    TEST_ASSERT_TRUE(net_apply_open());
    mock_nw_join_ok = false;
    mock_ha_grant_pending = true;
    mock_ha_grant_slot = 0;
    mock_ha_grant_sec = 300;
    TEST_ASSERT_EQUAL(NET_FINISH_IDLE, net_apply_finish());
    TEST_ASSERT_TRUE(mock_ha_grant_pending); /* wedged: results not applied */
    TEST_ASSERT_EQUAL_INT(0, n_config_applied);
    TEST_ASSERT_TRUE(net_window_active()); /* stays active for the sleep join */
}

void test_open_spawn_failure_is_fail_open(void) {
    mock_nw_spawn_ok = false;
    TEST_ASSERT_FALSE(net_apply_open());
    TEST_ASSERT_EQUAL(NET_FINISH_IDLE, net_apply_finish());
}

void test_try_window_spawn_failure_returns_fail_without_stats(void) {
    mock_nw_spawn_ok = false;
    TEST_ASSERT_EQUAL(ESP_FAIL, net_apply_try_window());
    TEST_ASSERT_EQUAL_INT(0, n_post_stats);
}

void test_try_window_posts_stats_once_and_returns_ntp_result(void) {
    mock_nw_ntp_result = ESP_OK;
    TEST_ASSERT_EQUAL(ESP_OK, net_apply_try_window());
    TEST_ASSERT_EQUAL_INT(1, n_post_stats);
    TEST_ASSERT_EQUAL_INT(1, n_config_applied);
    TEST_ASSERT_FALSE(net_window_active());
}

void test_try_window_reports_sync_failure_but_still_finishes(void) {
    mock_nw_wait_ntp_ok = false;
    mock_nw_ntp_result = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, net_apply_try_window());
    TEST_ASSERT_EQUAL_INT(1, n_post_stats); /* MQTT is best-effort regardless */
    TEST_ASSERT_FALSE(net_window_active());
}

void test_finish_runs_join_poll_and_config_invalidate(void) {
    TEST_ASSERT_TRUE(net_apply_open());
    TEST_ASSERT_EQUAL(NET_FINISH_IDLE, net_apply_finish());
    TEST_ASSERT_EQUAL_INT(1, n_join_poll);
    TEST_ASSERT_EQUAL_INT(1, n_config_applied);
}

/* ---- buffered HA effects: bonus / grant / locate ------------------------ */

void test_grant_applied_to_running_screen_extends_remaining(void) {
    timer_start(T0, 3600);
    TEST_ASSERT_TRUE(net_apply_open());
    mock_ha_grant_pending = true;
    mock_ha_grant_slot = 0;
    mock_ha_grant_sec = 300;
    net_apply_finish();
    TEST_ASSERT_FALSE(mock_ha_grant_pending);
    TEST_ASSERT_EQUAL_INT32(3900, timer_tick(T0));
}

void test_bonus_target_reconciled_against_applied(void) {
    timer_start(T0, 3600);
    TEST_ASSERT_TRUE(net_apply_open());
    mock_ha_bonus_pending = true;
    mock_ha_bonus_target = 600; /* nothing applied yet: full delta lands */
    net_apply_finish();
    TEST_ASSERT_EQUAL_INT32(600, timer_screen_bonus_applied());
    TEST_ASSERT_EQUAL_INT32(4200, timer_tick(T0));
}

void test_locate_pending_fires_locate_after_apply(void) {
    TEST_ASSERT_TRUE(net_apply_open());
    mock_ha_locate = true;
    net_apply_finish();
    TEST_ASSERT_EQUAL_INT(1, n_locate);
}

void test_no_locate_when_not_pending(void) {
    TEST_ASSERT_TRUE(net_apply_open());
    net_apply_finish();
    TEST_ASSERT_EQUAL_INT(0, n_locate);
}

/* ---- def reconcile fan-out ---------------------------------------------- */

static void select_slot(int slot) {
    while (timer_active_slot() != slot) {
        TEST_ASSERT_TRUE(timer_select_next());
    }
}

void test_unchanged_defs_reconcile_to_idle(void) {
    select_slot(1);
    timer_start(T0, 900);
    TEST_ASSERT_TRUE(net_apply_open());
    TEST_ASSERT_EQUAL(NET_FINISH_IDLE, net_apply_finish());
    TEST_ASSERT_EQUAL_INT(0, n_chirp);
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
}

void test_active_running_slot_renamed_resets_and_chirps(void) {
    select_slot(1);
    timer_start(T0, 900);
    TEST_ASSERT_TRUE(net_apply_open());
    timer_def_t edited[TIMER_SLOT_COUNT];
    memcpy(edited, PRE_DEFS, sizeof(edited));
    edited[1].name = "Guitar"; /* rename = different timer: reset */
    install_table(edited);
    TEST_ASSERT_EQUAL(NET_FINISH_CHANGED, net_apply_finish());
    TEST_ASSERT_EQUAL_INT(1, n_chirp);
    TEST_ASSERT_EQUAL_INT(0, n_expiry_alert);
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state());
}

void test_active_slot_shrunk_below_elapsed_expires_with_alert(void) {
    select_slot(1);
    timer_start(T0, 900);
    mock_time_set(T0 + 300); /* 5 min in */
    TEST_ASSERT_TRUE(net_apply_open());
    timer_def_t edited[TIMER_SLOT_COUNT];
    memcpy(edited, PRE_DEFS, sizeof(edited));
    edited[1].duration_sec = 120; /* shrunk under the elapsed 300 s */
    install_table(edited);
    TEST_ASSERT_EQUAL(NET_FINISH_ALERTED, net_apply_finish());
    TEST_ASSERT_EQUAL_INT(1, n_expiry_alert);
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
}

void test_active_slot_grown_updates_without_chirp(void) {
    select_slot(1);
    timer_start(T0, 900);
    TEST_ASSERT_TRUE(net_apply_open());
    timer_def_t edited[TIMER_SLOT_COUNT];
    memcpy(edited, PRE_DEFS, sizeof(edited));
    edited[1].duration_sec = 1800; /* grow mid-run: remaining moves */
    install_table(edited);
    TEST_ASSERT_EQUAL(NET_FINISH_CHANGED, net_apply_finish());
    TEST_ASSERT_EQUAL_INT(0, n_chirp);
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
    TEST_ASSERT_EQUAL_INT32(1800, timer_tick(T0));
}

void test_background_paused_slot_fixed_silently(void) {
    select_slot(1);
    timer_start(T0, 900);
    timer_pause(T0 + 60);
    select_slot(3); /* Meditation becomes active; Piano is background PAUSED */
    TEST_ASSERT_TRUE(net_apply_open());
    timer_def_t edited[TIMER_SLOT_COUNT];
    memcpy(edited, PRE_DEFS, sizeof(edited));
    edited[1].name = "Guitar"; /* background redefine: reset, no sound */
    install_table(edited);
    TEST_ASSERT_EQUAL(NET_FINISH_IDLE, net_apply_finish());
    TEST_ASSERT_EQUAL_INT(0, n_chirp);
    TEST_ASSERT_EQUAL_INT(0, n_expiry_alert);
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_slot_state(1)); /* fixed for the next swap */
}

void test_active_slot_disabled_by_edit_reverts_selection(void) {
    select_slot(1);
    timer_start(T0, 900);
    timer_pause(T0 + 60);
    TEST_ASSERT_TRUE(net_apply_open());
    timer_def_t edited[TIMER_SLOT_COUNT];
    memcpy(edited, PRE_DEFS, sizeof(edited));
    edited[1].name = ""; /* slot disabled mid-window */
    install_table(edited);
    net_apply_finish();
    TEST_ASSERT_EQUAL_INT(0, timer_active_slot()); /* never stranded on a dead slot */
}

void test_slot_disabled_prewindow_is_not_reconciled(void) {
    /* Slot 2 starts disabled; an edit enabling it mid-window must not be
       treated as a redefinition of running state (nothing was running). */
    TEST_ASSERT_TRUE(net_apply_open());
    timer_def_t edited[TIMER_SLOT_COUNT];
    memcpy(edited, PRE_DEFS, sizeof(edited));
    edited[2].name = "Reading";
    edited[2].duration_sec = 1200;
    install_table(edited);
    TEST_ASSERT_EQUAL(NET_FINISH_IDLE, net_apply_finish());
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_slot_state(2)); /* fresh slot, clean state */
}

/* ---- C3: late-settling sync must still shift the expiry ----------------- */

void test_pending_shift_applied_when_sync_lands_late(void) {
    timer_start(T0, 3600); /* painted against the uncorrected clock */
    int64_t expiry_before = timer_expiry_wall();
    TEST_ASSERT_TRUE(net_apply_open());
    net_apply_note_start_unsynced(); /* wait_ntp timed out at paint time */
    mock_nw_ntp_result = ESP_OK;     /* ...but the sync landed during the tail */
    mock_nw_clock_step = 120;
    net_apply_finish();
    TEST_ASSERT_EQUAL_INT64(expiry_before + 120, timer_expiry_wall());
    TEST_ASSERT_EQUAL_INT64(0, mock_nw_clock_step); /* consumed */
}

void test_pending_shift_skipped_when_sync_failed(void) {
    timer_start(T0, 3600);
    int64_t expiry_before = timer_expiry_wall();
    TEST_ASSERT_TRUE(net_apply_open());
    net_apply_note_start_unsynced();
    mock_nw_ntp_result = ESP_FAIL; /* no sync: step meaningless */
    mock_nw_clock_step = 120;
    net_apply_finish();
    TEST_ASSERT_EQUAL_INT64(expiry_before, timer_expiry_wall());
}

void test_no_pending_shift_means_finish_never_shifts(void) {
    /* The wait-succeeded path already consumed the step in the handler;
       finish must not apply anything on its own. */
    timer_start(T0, 3600);
    int64_t expiry_before = timer_expiry_wall();
    TEST_ASSERT_TRUE(net_apply_open());
    mock_nw_clock_step = 120;
    net_apply_finish();
    TEST_ASSERT_EQUAL_INT64(expiry_before, timer_expiry_wall());
    TEST_ASSERT_EQUAL_INT64(120, mock_nw_clock_step); /* not even taken */
}

void test_pending_shift_cleared_by_next_open(void) {
    timer_start(T0, 3600);
    int64_t expiry_before = timer_expiry_wall();
    TEST_ASSERT_TRUE(net_apply_open());
    net_apply_note_start_unsynced();
    mock_nw_join_ok = false;
    net_apply_finish(); /* wedged: pending survives, nothing applied */
    mock_nw_join_ok = true;
    mock_nw_active = false;
    TEST_ASSERT_TRUE(net_apply_open()); /* new window: stale pending dropped */
    mock_nw_clock_step = 120;
    net_apply_finish();
    TEST_ASSERT_EQUAL_INT64(expiry_before, timer_expiry_wall());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_finish_without_window_is_idle_and_consumes_nothing);
    RUN_TEST(test_finish_on_wedged_join_is_idle_and_applies_nothing);
    RUN_TEST(test_open_spawn_failure_is_fail_open);
    RUN_TEST(test_try_window_spawn_failure_returns_fail_without_stats);
    RUN_TEST(test_try_window_posts_stats_once_and_returns_ntp_result);
    RUN_TEST(test_try_window_reports_sync_failure_but_still_finishes);
    RUN_TEST(test_finish_runs_join_poll_and_config_invalidate);
    RUN_TEST(test_grant_applied_to_running_screen_extends_remaining);
    RUN_TEST(test_bonus_target_reconciled_against_applied);
    RUN_TEST(test_locate_pending_fires_locate_after_apply);
    RUN_TEST(test_no_locate_when_not_pending);
    RUN_TEST(test_unchanged_defs_reconcile_to_idle);
    RUN_TEST(test_active_running_slot_renamed_resets_and_chirps);
    RUN_TEST(test_active_slot_shrunk_below_elapsed_expires_with_alert);
    RUN_TEST(test_active_slot_grown_updates_without_chirp);
    RUN_TEST(test_background_paused_slot_fixed_silently);
    RUN_TEST(test_active_slot_disabled_by_edit_reverts_selection);
    RUN_TEST(test_slot_disabled_prewindow_is_not_reconciled);
    RUN_TEST(test_pending_shift_applied_when_sync_lands_late);
    RUN_TEST(test_pending_shift_skipped_when_sync_failed);
    RUN_TEST(test_no_pending_shift_means_finish_never_shifts);
    RUN_TEST(test_pending_shift_cleared_by_next_open);
    return UNITY_END();
}
