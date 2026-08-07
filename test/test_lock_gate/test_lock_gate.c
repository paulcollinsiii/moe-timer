#include <setjmp.h>
#include <stdlib.h>
#include <unity.h>

/* Single-TU: the real policies the gates consult (the battery lock band
   and its hysteresis, the bed-time window and alert rule, the precedence
   between the two locks) compiled in alongside the module under test, so
   the edges below are pinned end to end rather than against a restatement
   of the rules. Everything with a device behind it is stubbed. */
// clang-format off
#include "../../main/battery_policy.c"
#include "../../main/bedtime.c"
#include "../../main/quiet_hours.c"
#include "../../main/sleep_plan.c"
#include "mock_hal_time.c"
// clang-format on

#include "alerts.h"
#include "battery.h"
#include "config_cache.h"
#include "display.h"
#include "net_apply.h"
#include "timer.h"
#include "timer_persist.h"

/* ---- the effect log -----------------------------------------------------

   Two of the four behaviours this suite exists for are ORDERINGS, not
   outcomes ("paused before the paint"), so the stubs append to a shared
   log rather than each keeping its own counter. Names are all gate_/stub_
   prefixed: cppcheck's shadowFunction check runs across files, and the
   module under test has locals called pct, pol, now and mode. */

typedef enum {
    EV_TIMER_PAUSE = 1,
    EV_PERSIST_SAVE,
    EV_CHARGE_ME,
    EV_BEDTIME_SCREEN,
    EV_ALERT,
    EV_NET_WINDOW,
    EV_SLEEP,
} gate_event_t;

static gate_event_t gate_log[16];
static int gate_log_n;

static void gate_log_push(gate_event_t ev) {
    if (gate_log_n < (int)(sizeof gate_log / sizeof gate_log[0])) {
        gate_log[gate_log_n++] = ev;
    }
}

/* Position of the first occurrence, or -1. Ordering assertions compare
   two of these; -1 on either side makes the comparison fail loudly
   instead of quietly passing on an effect that never happened. */
static int gate_log_at(gate_event_t ev) {
    for (int i = 0; i < gate_log_n; i++) {
        if (gate_log[i] == ev)
            return i;
    }
    return -1;
}

static int gate_log_count(gate_event_t ev) {
    int n = 0;
    for (int i = 0; i < gate_log_n; i++) {
        if (gate_log[i] == ev)
            n++;
    }
    return n;
}

/* ---- injected device state ---------------------------------------------- */

static int gate_batt_pct;
static timer_state_t gate_timer_state;
static bool gate_break_active;
static bool gate_alert_dismissed;
static time_t gate_pause_arg;
static int gate_bedtime_min;              /* what the cache answers right now */
static int gate_bedtime_min_after_window; /* what an HA edit installs mid-window */
static time_t gate_clock_after_window;    /* 0 = the window does not step the clock */

/* Cases inject a percentage, because the lock band is stated in percent
   and the SoC curve is test_battery's business. The pair below is a fake
   curve rather than an identity on purpose: dropping either half of
   `battery_percent_from_mv(battery_read_mv())` then yields a nonsense
   percentage instead of the right answer by luck, which is what an
   identity stub would quietly permit. */
#define GATE_MV_OFFSET 2500

int battery_read_mv(void) {
    return gate_batt_pct * 10 + GATE_MV_OFFSET;
}

int battery_percent_from_mv(int mv) {
    return (mv - GATE_MV_OFFSET) / 10;
}

timer_state_t timer_get_state(void) {
    return gate_timer_state;
}

void timer_pause(time_t now) {
    gate_pause_arg = now;
    gate_timer_state = TIMER_PAUSED;
    gate_log_push(EV_TIMER_PAUSE);
}

bool timer_break_active(void) {
    return gate_break_active;
}

void timer_persist_save(void) {
    gate_log_push(EV_PERSIST_SAVE);
}

void display_charge_me(void) {
    gate_log_push(EV_CHARGE_ME);
}

void display_bedtime(void) {
    gate_log_push(EV_BEDTIME_SCREEN);
}

bool alert_run(alert_kind_t kind) {
    TEST_ASSERT_EQUAL_INT(ALERT_BEDTIME, kind); /* the gates raise no other kind */
    gate_log_push(EV_ALERT);
    return gate_alert_dismissed;
}

int config_cache_bedtime_minutes(void) {
    return gate_bedtime_min;
}

/* The real window's finish invalidates the config cache and can apply a
   measured NTP step, so BOTH operands of the bed-time re-check can differ
   after this call. Modelling only the config half would leave the clock
   half of the fall-through untested. */
esp_err_t net_apply_try_window(void) {
    gate_log_push(EV_NET_WINDOW);
    gate_bedtime_min = gate_bedtime_min_after_window;
    if (gate_clock_after_window != 0) {
        mock_time_set(gate_clock_after_window);
    }
    return ESP_OK;
}

/* ---- the sleep seam ----------------------------------------------------- */

static jmp_buf gate_sleep_jmp;
static int gate_sleep_calls;
static wake_sleep_mode_t gate_sleep_mode;

/* Models the real contract rather than papering over it: on device this
   never comes back, so here it unwinds to gate_run(). That is what makes
   "the gate ended the wake" and "the gate returned and the wake carries
   on" two distinguishable results — the distinction the whole module
   turns on, and the one a returning stub would silently erase. */
void enter_deep_sleep(wake_sleep_mode_t mode) {
    gate_sleep_calls++;
    gate_sleep_mode = mode;
    gate_log_push(EV_SLEEP);
    longjmp(gate_sleep_jmp, 1);
}

// clang-format off
#include "../../main/lock_gate.c"
// clang-format on

/* ---- harness ------------------------------------------------------------ */

/* Every case pins the clock to a fixed UTC day: minutes-of-day is then
   arithmetic instead of a calendar question, and the bed-time window
   comparisons read literally. */
#define GATE_DAY_BASE 1785283200 /* 2026-07-29 00:00:00 UTC */
#define GATE_BEDTIME_2000 (20 * 60)

static time_t gate_at(int hour, int minute) {
    return (time_t)GATE_DAY_BASE + (time_t)hour * 3600 + (time_t)minute * 60;
}

static time_t gate_next_day_at(int hour, int minute) {
    return gate_at(hour, minute) + 86400;
}

static void gate_set_bedtime(int minutes) {
    gate_bedtime_min = minutes;
    gate_bedtime_min_after_window = minutes; /* no edit unless a case asks for one */
}

void setUp(void) {
    setenv("TZ", "UTC0", 1);
    tzset();
    mock_time_reset();
    gate_log_n = 0;
    gate_sleep_calls = 0;
    gate_sleep_mode = WAKE_SLEEP_NORMAL;
    gate_batt_pct = 80;
    gate_timer_state = TIMER_IDLE;
    gate_break_active = false;
    gate_alert_dismissed = false;
    gate_pause_arg = 0;
    gate_clock_after_window = 0;
    gate_set_bedtime(-1); /* bed time disabled unless a case configures it */
    /* The module's own state is RTC-backed on device and plain statics
       here, so the suite zeroes them directly (as test_timer does) rather
       than making main.c carry a reset entry point it would never call. */
    s_charge_locked = false;
    s_charge_lock_released = false;
    s_bedtime_locked = false;
    s_bedtime_released = false;
}

void tearDown(void) {}

/* Runs one gate entry point with the non-returning sleep modelled.
   Returns true when the gate ended the wake. */
static bool gate_run(void (*body)(void)) {
    if (setjmp(gate_sleep_jmp) != 0)
        return true;
    body();
    return false;
}

static time_t gate_body_now;
static bool gate_body_alert;

/* The wake handlers take `now` from the same wall clock the gate later
   re-reads, so cases move them together by default. The two that pull
   them apart — a stale caller instant, and an NTP step landing during the
   window — do so explicitly, which is the point of those cases. */
static void gate_set_now(time_t t) {
    gate_body_now = t;
    mock_time_set(t);
}

static void gate_body_check_charge(void) {
    lock_gate_check_charge();
}

static void gate_body_check_bedtime(void) {
    lock_gate_check_bedtime(gate_body_now);
}

static void gate_body_engage(void) {
    lock_gate_bedtime_engage(gate_body_now, gate_body_alert);
}

/* ---- charge lock: the engage edge (row 15) ------------------------------ */

void test_healthy_battery_leaves_the_wake_alone(void) {
    gate_batt_pct = 80;
    TEST_ASSERT_FALSE(gate_run(gate_body_check_charge));
    TEST_ASSERT_FALSE(lock_gate_charge_locked());
    TEST_ASSERT_EQUAL_INT(0, gate_log_n);
}

/* The WARN band badges the progress bar; it must not lock. 15% is the top
   of it and 11% the bottom (10% locks), so both ends are checked — a
   comparison slipped by one shows up at exactly one of them. */
void test_warn_band_does_not_lock(void) {
    gate_batt_pct = 15;
    TEST_ASSERT_FALSE(gate_run(gate_body_check_charge));
    TEST_ASSERT_FALSE(lock_gate_charge_locked());
    gate_batt_pct = 11;
    TEST_ASSERT_FALSE(gate_run(gate_body_check_charge));
    TEST_ASSERT_FALSE(lock_gate_charge_locked());
    TEST_ASSERT_EQUAL_INT(0, gate_log_n);
}

void test_lock_band_engages_and_ends_the_wake(void) {
    gate_batt_pct = 10;
    TEST_ASSERT_TRUE(gate_run(gate_body_check_charge));
    TEST_ASSERT_TRUE(lock_gate_charge_locked());
    TEST_ASSERT_EQUAL_INT(1, gate_log_count(EV_CHARGE_ME));
    TEST_ASSERT_EQUAL_INT(1, gate_log_count(EV_NET_WINDOW));
    TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_CHARGE_LOCK, gate_sleep_mode);
}

/* Row 15. The allocation must stop accruing before the panel stops being
   able to show it — a paint-then-pause order would bill the child for the
   e-ink refresh and for however long the brownout lasts. */
void test_running_timer_is_paused_before_the_charge_me_paint(void) {
    gate_batt_pct = 5;
    gate_timer_state = TIMER_RUNNING;
    mock_time_set(gate_at(14, 30));

    TEST_ASSERT_TRUE(gate_run(gate_body_check_charge));

    TEST_ASSERT_EQUAL_INT(1, gate_log_count(EV_TIMER_PAUSE));
    TEST_ASSERT_TRUE(gate_log_at(EV_TIMER_PAUSE) < gate_log_at(EV_CHARGE_ME));
    /* and the paint is what the HA notification follows, not precedes */
    TEST_ASSERT_TRUE(gate_log_at(EV_CHARGE_ME) < gate_log_at(EV_NET_WINDOW));
    TEST_ASSERT_TRUE(gate_log_at(EV_NET_WINDOW) < gate_log_at(EV_SLEEP));
    /* the pause is stamped with the live clock, not a stale capture */
    TEST_ASSERT_EQUAL_INT64(gate_at(14, 30), gate_pause_arg);
}

void test_engage_does_not_pause_a_timer_that_is_not_running(void) {
    gate_batt_pct = 5;
    gate_timer_state = TIMER_PAUSED;
    TEST_ASSERT_TRUE(gate_run(gate_body_check_charge));
    TEST_ASSERT_EQUAL_INT(0, gate_log_count(EV_TIMER_PAUSE));
    TEST_ASSERT_EQUAL_INT(1, gate_log_count(EV_CHARGE_ME)); /* still paints */
}

/* The 600 s re-wakes must leave the panel and the radio alone: an e-ink
   refresh per wake on a flat battery is the artifact risk the lock exists
   to avoid, and a network window per wake is charge it cannot spare. */
void test_locked_rewake_only_sleeps_again(void) {
    s_charge_locked = true;
    gate_batt_pct = 5;
    gate_timer_state = TIMER_RUNNING;

    TEST_ASSERT_TRUE(gate_run(gate_body_check_charge));

    TEST_ASSERT_EQUAL_INT(1, gate_log_n);
    TEST_ASSERT_EQUAL_INT(EV_SLEEP, gate_log[0]);
    TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_CHARGE_LOCK, gate_sleep_mode);
}

/* ---- charge lock: the release edge (row 16) ----------------------------- */

/* Hysteresis: once locked the release needs the reading to clear the WARN
   band outright, so 15% stays locked and 16% releases. The pair is the
   test — either one alone passes against an off-by-one. */
void test_locked_at_fifteen_percent_stays_locked(void) {
    s_charge_locked = true;
    gate_batt_pct = 15;
    TEST_ASSERT_TRUE(gate_run(gate_body_check_charge));
    TEST_ASSERT_TRUE(lock_gate_charge_locked());
    TEST_ASSERT_FALSE(s_charge_lock_released);
}

void test_recovery_to_sixteen_percent_releases_the_lock(void) {
    s_charge_locked = true;
    gate_batt_pct = 16;

    TEST_ASSERT_FALSE(gate_run(gate_body_check_charge)); /* wake carries on */

    TEST_ASSERT_FALSE(lock_gate_charge_locked());
    TEST_ASSERT_TRUE(s_charge_lock_released);
    TEST_ASSERT_EQUAL_INT(0, gate_log_n); /* no paint here: the wake renders */
    TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_NORMAL, lock_gate_sleep_mode());
}

/* Row 16's tail: the panel is still showing Charge Me!, which a partial
   refresh cannot clear. */
void test_release_promotes_the_next_partial_render_to_full(void) {
    s_charge_locked = true;
    gate_batt_pct = 16;
    TEST_ASSERT_FALSE(gate_run(gate_body_check_charge));

    TEST_ASSERT_EQUAL_INT(WAKE_RENDER_FULL, lock_gate_promote_render(WAKE_RENDER_PARTIAL));
    TEST_ASSERT_EQUAL_INT(WAKE_RENDER_FULL, lock_gate_promote_render(WAKE_RENDER_FULL));
    /* An expiry alert owns the display end to end (TIME'S UP, alarm, then
       its own repaint); promoting it to a plain full refresh would drop
       the alarm entirely. */
    TEST_ASSERT_EQUAL_INT(WAKE_RENDER_EXPIRY_ALERT, lock_gate_promote_render(WAKE_RENDER_EXPIRY_ALERT));
}

/* The promotion is owed by the release EDGE, not by the unlocked LEVEL:
   a device that was never locked must not repaint fully on every tick. */
void test_no_promotion_without_a_release(void) {
    gate_batt_pct = 80;
    TEST_ASSERT_FALSE(gate_run(gate_body_check_charge));
    TEST_ASSERT_EQUAL_INT(WAKE_RENDER_PARTIAL, lock_gate_promote_render(WAKE_RENDER_PARTIAL));
}

void test_a_bedtime_release_promotes_the_render_too(void) {
    s_bedtime_locked = true;
    gate_set_bedtime(GATE_BEDTIME_2000);
    gate_set_now(gate_at(7, 30)); /* morning: window is over */

    TEST_ASSERT_FALSE(gate_run(gate_body_check_bedtime));

    TEST_ASSERT_TRUE(s_bedtime_released);
    TEST_ASSERT_EQUAL_INT(WAKE_RENDER_FULL, lock_gate_promote_render(WAKE_RENDER_PARTIAL));
}

/* ---- sleep mode (row 17) ------------------------------------------------ */

/* A battery that cannot afford a refresh cannot afford the 2 h cadence
   either, so the charge lock's 600 s wins whenever both flags are up —
   including when the sleep is the BED TIME path's own. */
void test_charge_lock_wins_when_both_locks_are_engaged(void) {
    s_bedtime_locked = true;
    s_charge_locked = true;
    TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_CHARGE_LOCK, lock_gate_sleep_mode());

    gate_set_bedtime(GATE_BEDTIME_2000);
    gate_set_now(gate_at(22, 0));
    s_bedtime_locked = false; /* let the engage below set it */
    TEST_ASSERT_TRUE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_CHARGE_LOCK, gate_sleep_mode);
}

void test_sleep_mode_tracks_each_lock_on_its_own(void) {
    TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_NORMAL, lock_gate_sleep_mode());
    s_bedtime_locked = true;
    TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_BEDTIME, lock_gate_sleep_mode());
    s_bedtime_locked = false;
    s_charge_locked = true;
    TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_CHARGE_LOCK, lock_gate_sleep_mode());
}

/* ---- bed time: the engage edge ------------------------------------------ */

void test_outside_the_window_is_a_no_op(void) {
    gate_set_bedtime(GATE_BEDTIME_2000);
    gate_set_now(gate_at(19, 59));
    TEST_ASSERT_FALSE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_EQUAL_INT(0, gate_log_n);
    TEST_ASSERT_FALSE(s_bedtime_locked);
}

void test_disabled_bedtime_never_engages(void) {
    gate_set_bedtime(-1);
    gate_set_now(gate_at(23, 59));
    TEST_ASSERT_FALSE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_FALSE(s_bedtime_locked);
    TEST_ASSERT_EQUAL_INT(0, gate_log_n);
}

/* The engage side reads the caller's `now`, not the live clock, and the
   two really can differ: the handlers capture `now` before day rollover
   and the break planner captures it before a render and an NTP window, so
   by the time the engage runs the wall clock has moved on. The window
   test and the pause stamp must both use the instant they were handed. */
void test_the_engage_judges_and_stamps_the_callers_instant(void) {
    gate_set_bedtime(GATE_BEDTIME_2000);
    gate_timer_state = TIMER_RUNNING;
    gate_body_now = gate_at(21, 0);
    mock_time_set(gate_at(10, 0)); /* live clock deliberately elsewhere */

    TEST_ASSERT_TRUE(gate_run(gate_body_check_bedtime)); /* engaged on `now` */

    TEST_ASSERT_TRUE(s_bedtime_locked);
    TEST_ASSERT_EQUAL_INT64(gate_at(21, 0), gate_pause_arg);
}

void test_engage_pauses_and_persists_before_painting_bed_time(void) {
    gate_set_bedtime(GATE_BEDTIME_2000);
    gate_timer_state = TIMER_RUNNING;
    gate_set_now(gate_at(20, 0));

    TEST_ASSERT_TRUE(gate_run(gate_body_check_bedtime));

    TEST_ASSERT_TRUE(s_bedtime_locked);
    TEST_ASSERT_TRUE(gate_log_at(EV_TIMER_PAUSE) < gate_log_at(EV_PERSIST_SAVE));
    TEST_ASSERT_TRUE(gate_log_at(EV_PERSIST_SAVE) < gate_log_at(EV_BEDTIME_SCREEN));
    TEST_ASSERT_TRUE(gate_log_at(EV_BEDTIME_SCREEN) < gate_log_at(EV_ALERT));
    TEST_ASSERT_TRUE(gate_log_at(EV_ALERT) < gate_log_at(EV_NET_WINDOW));
    TEST_ASSERT_TRUE(gate_log_at(EV_NET_WINDOW) < gate_log_at(EV_SLEEP));
    TEST_ASSERT_EQUAL_INT64(gate_at(20, 0), gate_pause_arg);
    TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_BEDTIME, gate_sleep_mode);
}

/* Never expired, only paused: day rollover resets the slots overnight, so
   expiring would put a phantom TIME'S UP in the daily summary. */
void test_engage_pauses_rather_than_expiring(void) {
    gate_set_bedtime(GATE_BEDTIME_2000);
    gate_timer_state = TIMER_RUNNING;
    gate_set_now(gate_at(20, 0));
    TEST_ASSERT_TRUE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_EQUAL_INT(TIMER_PAUSED, gate_timer_state);
}

/* Alerting is bedtime.c's rule, wired through here: it fires when the
   crossing interrupts someone. */
void test_an_idle_crossing_engages_silently(void) {
    gate_set_bedtime(GATE_BEDTIME_2000);
    gate_timer_state = TIMER_IDLE;
    gate_break_active = false;
    gate_set_now(gate_at(20, 0));
    TEST_ASSERT_TRUE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_EQUAL_INT(0, gate_log_count(EV_ALERT));
    TEST_ASSERT_EQUAL_INT(1, gate_log_count(EV_BEDTIME_SCREEN));
}

void test_a_break_running_behind_an_idle_slot_still_alerts(void) {
    gate_set_bedtime(GATE_BEDTIME_2000);
    gate_timer_state = TIMER_IDLE;
    gate_break_active = true;
    gate_set_now(gate_at(20, 0));
    TEST_ASSERT_TRUE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_EQUAL_INT(1, gate_log_count(EV_ALERT));
}

/* The break planner's direct entry: going to bed early, always audibly. */
void test_direct_engage_alerts_when_asked_regardless_of_state(void) {
    gate_timer_state = TIMER_IDLE;
    gate_break_active = false;
    gate_set_now(gate_at(19, 40));
    gate_body_alert = true;

    TEST_ASSERT_TRUE(gate_run(gate_body_engage));

    TEST_ASSERT_TRUE(s_bedtime_locked);
    TEST_ASSERT_EQUAL_INT(1, gate_log_count(EV_ALERT));
    TEST_ASSERT_EQUAL_INT(1, gate_log_count(EV_BEDTIME_SCREEN));
    TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_BEDTIME, gate_sleep_mode);
}

void test_direct_engage_stays_silent_when_asked(void) {
    gate_set_now(gate_at(19, 40));
    gate_body_alert = false;
    TEST_ASSERT_TRUE(gate_run(gate_body_engage));
    TEST_ASSERT_EQUAL_INT(0, gate_log_count(EV_ALERT));
}

/* ---- bed time: the locked re-wake and the release (row 14) -------------- */

void test_locked_rewake_runs_one_window_and_sleeps_again(void) {
    s_bedtime_locked = true;
    gate_set_bedtime(GATE_BEDTIME_2000);
    gate_set_now(gate_at(23, 0));

    TEST_ASSERT_TRUE(gate_run(gate_body_check_bedtime));

    TEST_ASSERT_EQUAL_INT(1, gate_log_count(EV_NET_WINDOW));
    TEST_ASSERT_EQUAL_INT(0, gate_log_count(EV_BEDTIME_SCREEN)); /* e-ink retains */
    TEST_ASSERT_EQUAL_INT(0, gate_log_count(EV_ALERT));
    TEST_ASSERT_EQUAL_INT(0, gate_log_count(EV_TIMER_PAUSE));
    TEST_ASSERT_TRUE(s_bedtime_locked);
    TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_BEDTIME, gate_sleep_mode);
}

/* ROW 14. The buttons are dark for the whole window, so an HA edit picked
   up by that one network window is the only remote fix path there is —
   and it has to take effect on THIS wake. Waiting for the next 2 h wake
   is the bug this fall-through exists to prevent. */
void test_a_bedtime_edit_during_the_window_releases_in_wake(void) {
    s_bedtime_locked = true;
    gate_set_bedtime(GATE_BEDTIME_2000);
    gate_bedtime_min_after_window = -1; /* parent disables bed time from HA */
    gate_set_now(gate_at(23, 0));

    TEST_ASSERT_FALSE(gate_run(gate_body_check_bedtime)); /* falls through */

    TEST_ASSERT_FALSE(s_bedtime_locked);
    TEST_ASSERT_TRUE(s_bedtime_released);
    TEST_ASSERT_EQUAL_INT(0, gate_sleep_calls); /* did NOT wait another 2 h */
    TEST_ASSERT_EQUAL_INT(1, gate_log_count(EV_NET_WINDOW));
    /* The gate does not repaint: the wake it fell through into does, and
       the promotion above is what makes that repaint a full one. */
    TEST_ASSERT_EQUAL_INT(0, gate_log_count(EV_BEDTIME_SCREEN));
    TEST_ASSERT_EQUAL_INT(WAKE_RENDER_FULL, lock_gate_promote_render(WAKE_RENDER_PARTIAL));
}

/* A later bed time is an edit too — the released state must follow the
   window, not merely the "disabled" sentinel. */
void test_a_bedtime_pushed_later_releases_in_wake(void) {
    s_bedtime_locked = true;
    gate_set_bedtime(GATE_BEDTIME_2000);
    gate_bedtime_min_after_window = 23 * 60 + 30; /* pushed to 23:30 */
    gate_set_now(gate_at(23, 0));

    TEST_ASSERT_FALSE(gate_run(gate_body_check_bedtime));

    TEST_ASSERT_FALSE(s_bedtime_locked);
    TEST_ASSERT_TRUE(s_bedtime_released);
}

/* The re-check's OTHER operand. The window's NTP sync can step the clock
   across the rollover; re-reading the config while reusing the entry
   instant would miss it and cost another 2 h. */
void test_a_clock_step_during_the_window_releases_in_wake(void) {
    s_bedtime_locked = true;
    gate_set_bedtime(GATE_BEDTIME_2000);
    gate_set_now(gate_at(23, 50));
    gate_clock_after_window = gate_next_day_at(0, 20); /* NTP lands past midnight */

    TEST_ASSERT_FALSE(gate_run(gate_body_check_bedtime));

    TEST_ASSERT_FALSE(s_bedtime_locked);
    TEST_ASSERT_TRUE(s_bedtime_released);
    TEST_ASSERT_EQUAL_INT(0, gate_sleep_calls);
}

/* An edit that leaves the window still active must NOT release: the
   fall-through is a release path, not an unconditional one. */
void test_an_edit_that_keeps_the_window_open_stays_locked(void) {
    s_bedtime_locked = true;
    gate_set_bedtime(GATE_BEDTIME_2000);
    gate_bedtime_min_after_window = 21 * 60; /* still before 23:00 */
    gate_set_now(gate_at(23, 0));

    TEST_ASSERT_TRUE(gate_run(gate_body_check_bedtime));

    TEST_ASSERT_TRUE(s_bedtime_locked);
    TEST_ASSERT_FALSE(s_bedtime_released);
    TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_BEDTIME, gate_sleep_mode);
}

/* Morning: the window is over before the gate even opens one, so no
   network window is spent on the release. */
void test_morning_release_costs_no_network_window(void) {
    s_bedtime_locked = true;
    gate_set_bedtime(GATE_BEDTIME_2000);
    gate_set_now(gate_at(7, 0));

    TEST_ASSERT_FALSE(gate_run(gate_body_check_bedtime));

    TEST_ASSERT_FALSE(s_bedtime_locked);
    TEST_ASSERT_TRUE(s_bedtime_released);
    TEST_ASSERT_EQUAL_INT(0, gate_log_n);
}

/* An unlocked device waking outside the window is the common case: it
   must not claim a release it never made, or every daytime tick would
   burn a full refresh. */
void test_unlocked_and_outside_the_window_claims_no_release(void) {
    gate_set_bedtime(GATE_BEDTIME_2000);
    gate_set_now(gate_at(9, 0));
    TEST_ASSERT_FALSE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_FALSE(s_bedtime_released);
    TEST_ASSERT_EQUAL_INT(WAKE_RENDER_PARTIAL, lock_gate_promote_render(WAKE_RENDER_PARTIAL));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_healthy_battery_leaves_the_wake_alone);
    RUN_TEST(test_warn_band_does_not_lock);
    RUN_TEST(test_lock_band_engages_and_ends_the_wake);
    RUN_TEST(test_running_timer_is_paused_before_the_charge_me_paint);
    RUN_TEST(test_engage_does_not_pause_a_timer_that_is_not_running);
    RUN_TEST(test_locked_rewake_only_sleeps_again);
    RUN_TEST(test_locked_at_fifteen_percent_stays_locked);
    RUN_TEST(test_recovery_to_sixteen_percent_releases_the_lock);
    RUN_TEST(test_release_promotes_the_next_partial_render_to_full);
    RUN_TEST(test_no_promotion_without_a_release);
    RUN_TEST(test_a_bedtime_release_promotes_the_render_too);
    RUN_TEST(test_charge_lock_wins_when_both_locks_are_engaged);
    RUN_TEST(test_sleep_mode_tracks_each_lock_on_its_own);
    RUN_TEST(test_outside_the_window_is_a_no_op);
    RUN_TEST(test_disabled_bedtime_never_engages);
    RUN_TEST(test_the_engage_judges_and_stamps_the_callers_instant);
    RUN_TEST(test_engage_pauses_and_persists_before_painting_bed_time);
    RUN_TEST(test_engage_pauses_rather_than_expiring);
    RUN_TEST(test_an_idle_crossing_engages_silently);
    RUN_TEST(test_a_break_running_behind_an_idle_slot_still_alerts);
    RUN_TEST(test_direct_engage_alerts_when_asked_regardless_of_state);
    RUN_TEST(test_direct_engage_stays_silent_when_asked);
    RUN_TEST(test_locked_rewake_runs_one_window_and_sleeps_again);
    RUN_TEST(test_a_bedtime_edit_during_the_window_releases_in_wake);
    RUN_TEST(test_a_bedtime_pushed_later_releases_in_wake);
    RUN_TEST(test_a_clock_step_during_the_window_releases_in_wake);
    RUN_TEST(test_an_edit_that_keeps_the_window_open_stays_locked);
    RUN_TEST(test_morning_release_costs_no_network_window);
    RUN_TEST(test_unlocked_and_outside_the_window_claims_no_release);
    return UNITY_END();
}
