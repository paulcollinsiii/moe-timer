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
#include "../../main/config_validate.c"
#include "../../main/quiet_hours.c"
#include "../../main/sleep_plan.c"
#include "mock_hal_time.c"
// clang-format on

#include "alerts.h"
#include "battery.h"
#include "config_cache.h"
#include "display.h"
#include "net_apply.h"
#include "schedule.h"
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
    EV_CONFIG_ERR_SCREEN,
    EV_ALERT,
    EV_NET_WINDOW,
    /* SOMETHING ELSE REACHED THE PANEL FROM INSIDE THE WINDOW. Not a
       screen this module paints: it stands for whatever
       paint_current_state_full() puts up when net_apply's reconcile fires
       on_active_expired_alert() — the chore checklist, TIME'S UP, the
       timer screen. Which one it is does not matter to any case here;
       that it happened, and that the gate did not leave it on the glass,
       is the whole question. */
    EV_WINDOW_PAINT,
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

static bool gate_is_paint(gate_event_t ev) {
    return ev == EV_CHARGE_ME || ev == EV_BEDTIME_SCREEN || ev == EV_CONFIG_ERR_SCREEN || ev == EV_WINDOW_PAINT;
}

/* WHAT THE PANEL IS LEFT HOLDING: the last thing painted before the wake
   ended, or 0 if nothing painted at all. e-ink retains, so this — and not
   a count — is the question every locked path has to answer. Counting
   cannot ask it: a gate that paints its screen and then lets the window
   paint over the top counts exactly the same as one that gets it right,
   which is how the defect this helper exists for survived a suite that
   already had thirty assertions about these paths. */
static gate_event_t gate_last_paint_before_sleep(void) {
    gate_event_t last = (gate_event_t)0;
    for (int i = 0; i < gate_log_n; i++) {
        if (gate_log[i] == EV_SLEEP)
            break;
        if (gate_is_paint(gate_log[i]))
            last = gate_log[i];
    }
    return last;
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

/* THE SEAM THAT HID THE DEFECT. The real net_apply_try_window() is not a
   passive reader: its reconcile can fire on_active_expired_alert(), and
   that hook repaints (net_apply.c, main.c). Every gate here pauses a
   running timer as it engages, and PAUSED is the state whose reconcile
   can cross zero and report TIMER_RECONCILE_EXPIRED (timer.c), so each
   lock arms the painter that can paint over it. A stub that could only
   change config values and the clock modelled a window that cannot touch
   the panel — which is exactly the assumption that was wrong. */
static bool gate_window_paints;

/* ---- the config-error gate's inputs -------------------------------------

   Four day types, each with its own stored pair, because the whole point
   of the gate is that it judges ONE of them — today's — and stays dormant
   over the other three (design 5.3, rows C11/C12). A single pair here
   would make "only today's" untestable by construction.

   MINUTES, as stored. schedule_get_chore_free_sec() returns SECONDS and
   CLAMPS the free slice to the allocation, so it can never see the fault;
   the gate reads the raw minute pair instead and these stubs are in that
   unit deliberately. */
#define GATE_DAY_TYPES 4

static uint16_t gate_free_min[GATE_DAY_TYPES];
static uint16_t gate_alloc_min[GATE_DAY_TYPES];
static uint16_t gate_free_min_after_window[GATE_DAY_TYPES];
static uint16_t gate_alloc_min_after_window[GATE_DAY_TYPES];

static day_type_t gate_day_type;       /* today, before any clock step */
static day_type_t gate_day_type_after; /* after gate_day_switch_at */
static time_t gate_day_switch_at;      /* 0 = the day type never changes */

/* Non-vacuity witnesses: which day type the gate actually looked up, and
   the instant it resolved the day from. A gate that asked about the wrong
   day, or re-judged a locked re-wake against the caller's stale instant
   instead of the post-window clock, is a silent pass without these. */
static day_type_t gate_pair_asked;
static int gate_pair_calls;
static time_t gate_day_type_arg;

/* What the config-error screen was told to name. */
static day_type_t gate_screen_day;
static uint16_t gate_screen_free;
static uint16_t gate_screen_alloc;

static unsigned gate_day_idx(day_type_t dt) {
    return ((unsigned)dt < GATE_DAY_TYPES) ? (unsigned)dt : 0u;
}

/* A REAL function of `now`, not a value the window happens to overwrite:
   the release-by-day-change case turns on the gate re-reading the clock
   and re-resolving the day from it, and a stub that ignored `now` would
   pass that case without either happening. */
day_type_t schedule_get_day_type(time_t now) {
    gate_day_type_arg = now;
    return (gate_day_switch_at != 0 && now >= gate_day_switch_at) ? gate_day_type_after : gate_day_type;
}

void schedule_get_chore_free_pair_min(day_type_t day_type, uint16_t *free_min, uint16_t *alloc_min) {
    gate_pair_asked = day_type;
    gate_pair_calls++;
    *free_min = gate_free_min[gate_day_idx(day_type)];
    *alloc_min = gate_alloc_min[gate_day_idx(day_type)];
}

void display_config_error(day_type_t day_type, uint16_t chore_free_min, uint16_t alloc_min) {
    gate_screen_day = day_type;
    gate_screen_free = chore_free_min;
    gate_screen_alloc = alloc_min;
    gate_log_push(EV_CONFIG_ERR_SCREEN);
}

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
    /* The real window's finish invalidates the schedule cache too
       (config_cache_invalidate -> schedule_cache_invalidate), so a
       chore_free/allocation edit that landed in it is visible to the
       re-check that follows. Modelling that is what makes the
       fix-arrives-in-the-window release path a real test rather than an
       assertion about a value nothing could have changed. */
    for (unsigned i = 0; i < GATE_DAY_TYPES; i++) {
        gate_free_min[i] = gate_free_min_after_window[i];
        gate_alloc_min[i] = gate_alloc_min_after_window[i];
    }
    if (gate_clock_after_window != 0) {
        mock_time_set(gate_clock_after_window);
    }
    /* LAST, because the real hook runs inside net_apply_finish()'s
       reconcile — after the config it applied is visible and after any
       NTP step it measured. A paint pushed before those would let a gate
       that repaints too early still look correct. */
    if (gate_window_paints) {
        gate_log_push(EV_WINDOW_PAINT);
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

/* Install one day type's stored pair on both sides of the network window,
   so a case that does not care about the window says nothing about it. */
static void gate_set_pair(day_type_t dt, uint16_t free_min, uint16_t alloc_min) {
    unsigned i = gate_day_idx(dt);
    gate_free_min[i] = free_min;
    gate_alloc_min[i] = alloc_min;
    gate_free_min_after_window[i] = free_min;
    gate_alloc_min_after_window[i] = alloc_min;
}

/* What an HA edit landing inside the window installs. */
static void gate_set_pair_after_window(day_type_t dt, uint16_t free_min, uint16_t alloc_min) {
    unsigned i = gate_day_idx(dt);
    gate_free_min_after_window[i] = free_min;
    gate_alloc_min_after_window[i] = alloc_min;
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
    gate_window_paints = false; /* off unless a case asks the window to paint */
    gate_set_bedtime(-1);       /* bed time disabled unless a case configures it */
    /* Every day type valid by default — free 0 against a 60 min day, which
       is the fully-gated state every device in the field ships in. The
       config gate must be invisible to the thirty cases above this line,
       and a default that locked would make all of them assert the wrong
       thing. */
    gate_day_type = DAY_WEEKDAY;
    gate_day_type_after = DAY_WEEKDAY;
    gate_day_switch_at = 0;
    gate_day_type_arg = 0;
    gate_pair_asked = (day_type_t)-1;
    gate_pair_calls = 0;
    gate_screen_day = (day_type_t)-1;
    gate_screen_free = 0;
    gate_screen_alloc = 0;
    for (unsigned i = 0; i < GATE_DAY_TYPES; i++) {
        gate_set_pair((day_type_t)i, 0, 60);
    }
    /* The module's own state is RTC-backed on device and plain statics
       here, so the suite zeroes them directly (as test_timer does) rather
       than making main.c carry a reset entry point it would never call. */
    s_charge_locked = false;
    s_charge_lock_released = false;
    s_bedtime_locked = false;
    s_bedtime_released = false;
    s_config_locked = false;
    s_config_released = false;
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

/* What the gate told its caller (M3-T4): true = a lock let go in this
   call. Poisoned before each run so a gate that never returned cannot
   read as either answer. */
static int gate_body_released;

static void gate_body_check_bedtime(void) {
    gate_body_released = -1;
    gate_body_released = lock_gate_check_bedtime(gate_body_now) ? 1 : 0;
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
    /* TWICE, and both are load-bearing: once before the window so the
       screen is up before 90 s of radio, once after it because the window
       can repaint (see THE LAST WORD ON THE PANEL in lock_gate.c). */
    TEST_ASSERT_EQUAL_INT(2, gate_log_count(EV_CHARGE_ME));
    TEST_ASSERT_EQUAL_INT(1, gate_log_count(EV_NET_WINDOW));
    TEST_ASSERT_EQUAL_INT(EV_CHARGE_ME, gate_last_paint_before_sleep());
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
    TEST_ASSERT_EQUAL_INT(2, gate_log_count(EV_CHARGE_ME)); /* still paints, both times */
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
    TEST_ASSERT_EQUAL_INT(2, gate_log_count(EV_BEDTIME_SCREEN)); /* engage, then the pre-sleep repaint */
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
    TEST_ASSERT_EQUAL_INT(1, gate_log_count(EV_ALERT)); /* the alert does NOT repeat */
    TEST_ASSERT_EQUAL_INT(2, gate_log_count(EV_BEDTIME_SCREEN));
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
    /* ONE paint, and it is the pre-sleep repaint rather than a re-engage:
       no alert and no pause come with it. The re-wake owes the panel this
       one refresh because the window it just ran could have painted. */
    TEST_ASSERT_EQUAL_INT(1, gate_log_count(EV_BEDTIME_SCREEN));
    TEST_ASSERT_TRUE(gate_log_at(EV_NET_WINDOW) < gate_log_at(EV_BEDTIME_SCREEN));
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

/* ---- the config-error lock (design 5.3, rows C11/C12) -------------------

   THE THIRD THING THAT CAN LOCK THIS DEVICE, and the plan's risk register
   names the failure mode outright: "config-error lock with no exit ->
   unrecoverable device without a serial cable". So the cases below are
   weighted towards the RELEASE path rather than the engage, and every way
   the gate could engage and then fail to let go has one of its own:

     R1 the pair is fixed between wakes         -> released before any window
     R2 the fix lands in the ENGAGE wake's window
     R3 the fix lands in a locked RE-WAKE's window
     R4 the day type changes under a clock step -> re-resolved, not cached
     R5 the day rolls over into a good pair     -> same, without a step
     R6 the wake sources survive                -> D stays armed (sleep_plan)
     R7 another lock painted over us            -> repaint, not a blank hold
     R8 the WINDOW painted over us              -> repaint, last, before sleep

   R6 is the one that is not a property of this file alone, which is why
   it asserts through sleep_plan_outcome(): the mode this gate sleeps under
   is the only thing standing between "press D to fix it" and a device that
   can only be rescued with a cable.

   R8 arrived in review and is the reason R7's cases no longer pin a
   condition of their own: R7 was answered by an arm that named the two
   painters it knew about, and R8 is a third that arm could not see. One
   unconditional repaint before each locked sleep answers both, and the R8
   cases are written for all three locks because all three run a window
   with their flag already set. */

static uint16_t gate_free_min_of(day_type_t dt) {
    return gate_free_min[gate_day_idx(dt)];
}

/* ---- config error: the engage edge (row C11) ---------------------------- */

void test_a_valid_pair_leaves_the_wake_alone(void) {
    gate_set_pair(DAY_WEEKDAY, 20, 60);
    gate_set_now(gate_at(9, 0));
    TEST_ASSERT_FALSE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_FALSE(s_config_locked);
    TEST_ASSERT_EQUAL_INT(0, gate_log_n);
}

/* `==` is VALID and it is the per-day-type off switch, not a near miss:
   the whole allocation handed over free. A gate written with `>=` locks
   every operator who typed the same number twice out of their device. */
void test_the_equal_pair_is_the_off_switch_and_never_locks(void) {
    gate_set_pair(DAY_WEEKDAY, 60, 60);
    gate_set_now(gate_at(9, 0));
    TEST_ASSERT_FALSE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_FALSE(s_config_locked);
    TEST_ASSERT_EQUAL_INT(0, gate_log_n);
}

void test_a_broken_pair_for_today_engages_and_ends_the_wake(void) {
    gate_set_pair(DAY_WEEKDAY, 120, 60);
    gate_set_now(gate_at(9, 0));
    TEST_ASSERT_TRUE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_TRUE(s_config_locked);
    TEST_ASSERT_EQUAL_INT(1, gate_sleep_calls);
    TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_CONFIG_ERR, gate_sleep_mode);
    /* Twice: the engage paint, then the pre-sleep repaint the window
       makes necessary (THE LAST WORD ON THE PANEL, lock_gate.c). */
    TEST_ASSERT_EQUAL_INT(2, gate_log_count(EV_CONFIG_ERR_SCREEN));
}

/* Row C12. A broken SUMMER pair in November is dormant: the device does
   not block in December over a setting nobody will look at until June.
   The witness matters as much as the outcome — a gate that judged a
   hard-coded DAY_WEEKDAY would pass the assertion above and fail here. */
void test_a_broken_pair_for_another_day_type_stays_dormant(void) {
    gate_set_pair(DAY_SUMMER, 240, 60); /* broken, and not today */
    gate_set_pair(DAY_WEEKDAY, 20, 60); /* today, and fine */
    gate_day_type = DAY_WEEKDAY;
    gate_set_now(gate_at(9, 0));

    TEST_ASSERT_FALSE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_FALSE(s_config_locked);
    TEST_ASSERT_EQUAL_INT(0, gate_log_n);
    TEST_ASSERT_TRUE_MESSAGE(gate_pair_calls > 0, "the gate never looked a pair up - the case asserted nothing");
    TEST_ASSERT_EQUAL_INT_MESSAGE(DAY_WEEKDAY, gate_pair_asked, "the gate judged a day type that is not today");
}

/* The same broken pair, now that it IS today's. Without this the case
   above passes on a gate that never locks at all. */
void test_the_same_pair_locks_once_that_day_type_is_today(void) {
    gate_set_pair(DAY_SUMMER, 240, 60);
    gate_day_type = DAY_SUMMER;
    gate_set_now(gate_at(9, 0));

    TEST_ASSERT_TRUE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_TRUE(s_config_locked);
    TEST_ASSERT_EQUAL_INT(DAY_SUMMER, gate_pair_asked);
}

/* The screen names the offending pair, which is what makes the fix
   possible without a laptop — the entire justification for layer 3 being
   a screen rather than another ack nobody reads. */
void test_the_screen_is_told_which_pair_is_broken(void) {
    gate_set_pair(DAY_HOLIDAY, 300, 90);
    gate_day_type = DAY_HOLIDAY;
    gate_set_now(gate_at(9, 0));

    TEST_ASSERT_TRUE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_EQUAL_INT(2, gate_log_count(EV_CONFIG_ERR_SCREEN));
    TEST_ASSERT_EQUAL_INT(DAY_HOLIDAY, gate_screen_day);
    TEST_ASSERT_EQUAL_UINT16(300, gate_screen_free);
    TEST_ASSERT_EQUAL_UINT16(90, gate_screen_alloc);
}

void test_a_running_timer_is_paused_and_persisted_before_the_config_paint(void) {
    gate_set_pair(DAY_WEEKDAY, 120, 60);
    gate_timer_state = TIMER_RUNNING;
    gate_set_now(gate_at(9, 0));

    TEST_ASSERT_TRUE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_EQUAL_INT(1, gate_log_count(EV_TIMER_PAUSE));
    TEST_ASSERT_EQUAL_INT64(gate_at(9, 0), gate_pause_arg);
    TEST_ASSERT_TRUE(gate_log_at(EV_TIMER_PAUSE) < gate_log_at(EV_PERSIST_SAVE));
    TEST_ASSERT_TRUE(gate_log_at(EV_PERSIST_SAVE) < gate_log_at(EV_CONFIG_ERR_SCREEN));
    TEST_ASSERT_TRUE(gate_log_at(EV_CONFIG_ERR_SCREEN) < gate_log_at(EV_NET_WINDOW));
    TEST_ASSERT_TRUE(gate_log_at(EV_NET_WINDOW) < gate_log_at(EV_SLEEP));
}

void test_the_config_engage_does_not_pause_a_timer_that_is_not_running(void) {
    gate_set_pair(DAY_WEEKDAY, 120, 60);
    gate_timer_state = TIMER_PAUSED;
    gate_set_now(gate_at(9, 0));

    TEST_ASSERT_TRUE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_EQUAL_INT(0, gate_log_count(EV_TIMER_PAUSE));
    TEST_ASSERT_EQUAL_INT(TIMER_PAUSED, gate_timer_state);
}

/* A locked re-wake spends its whole budget on the one thing that can end
   the lock, and pays for exactly one refresh on the way out: the window
   it just ran is a painter (THE LAST WORD ON THE PANEL, lock_gate.c), so
   the gate cannot leave the glass to chance. NOT a re-engage — no pause,
   no persist, no second screen before the window. */
void test_a_locked_config_rewake_runs_one_window_and_sleeps_again(void) {
    gate_set_pair(DAY_WEEKDAY, 120, 60);
    s_config_locked = true;
    gate_set_now(gate_at(11, 0));

    TEST_ASSERT_TRUE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_EQUAL_INT(1, gate_log_count(EV_CONFIG_ERR_SCREEN));
    TEST_ASSERT_TRUE(gate_log_at(EV_NET_WINDOW) < gate_log_at(EV_CONFIG_ERR_SCREEN));
    TEST_ASSERT_EQUAL_INT(0, gate_log_count(EV_TIMER_PAUSE));
    TEST_ASSERT_EQUAL_INT(0, gate_log_count(EV_PERSIST_SAVE));
    TEST_ASSERT_EQUAL_INT(1, gate_log_count(EV_NET_WINDOW));
    TEST_ASSERT_EQUAL_INT(1, gate_sleep_calls);
    TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_CONFIG_ERR, gate_sleep_mode);
    TEST_ASSERT_TRUE(s_config_locked);
}

/* ---- config error: R1..R5, the release paths ---------------------------- */

/* R1. The pair was fixed while the device slept, so the gate lets go
   before spending a network window on a problem that is already over. */
void test_a_pair_fixed_between_wakes_releases_without_a_window(void) {
    gate_set_pair(DAY_WEEKDAY, 30, 60);
    s_config_locked = true;
    gate_set_now(gate_at(11, 0));

    TEST_ASSERT_FALSE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_FALSE(s_config_locked);
    TEST_ASSERT_TRUE(s_config_released);
    TEST_ASSERT_EQUAL_INT(0, gate_log_count(EV_NET_WINDOW));
}

/* R2. The fix lands in the window the ENGAGE wake opens. Without the
   re-check after that window the device would sleep a whole interval on a
   config that is already correct — the panel saying "broken" while NVS
   says otherwise. */
void test_a_fix_in_the_engage_window_releases_in_the_same_wake(void) {
    gate_set_pair(DAY_WEEKDAY, 120, 60);
    gate_set_pair_after_window(DAY_WEEKDAY, 30, 60);
    gate_set_now(gate_at(9, 0));

    TEST_ASSERT_FALSE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_EQUAL_INT(1, gate_log_count(EV_CONFIG_ERR_SCREEN)); /* it did engage */
    TEST_ASSERT_EQUAL_INT(1, gate_log_count(EV_NET_WINDOW));
    TEST_ASSERT_EQUAL_INT(0, gate_sleep_calls);
    TEST_ASSERT_FALSE(s_config_locked);
    TEST_ASSERT_TRUE(s_config_released);
}

/* R3. The same fix on a LOCKED re-wake, which is the path a parent
   actually uses: press D, the window runs, the corrected config arrives,
   the device is usable before the button is released. */
void test_a_fix_in_a_locked_rewake_window_releases_in_the_same_wake(void) {
    gate_set_pair(DAY_WEEKDAY, 120, 60);
    gate_set_pair_after_window(DAY_WEEKDAY, 0, 60);
    s_config_locked = true;
    gate_set_now(gate_at(11, 0));

    TEST_ASSERT_FALSE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_EQUAL_INT(1, gate_log_count(EV_NET_WINDOW));
    TEST_ASSERT_EQUAL_INT(0, gate_sleep_calls);
    TEST_ASSERT_FALSE(s_config_locked);
    TEST_ASSERT_TRUE(s_config_released);
}

/* R4. An NTP step inside the window carries the clock into a day type
   whose pair is fine. The gate has to re-resolve the DAY as well as the
   pair: re-reading the same day's values would hold the lock forever on a
   device whose only good day is tomorrow. */
void test_a_clock_step_into_a_good_day_type_releases_in_wake(void) {
    gate_set_pair(DAY_WEEKDAY, 120, 60); /* today: broken */
    gate_set_pair(DAY_WEEKEND, 20, 60);  /* tomorrow: fine */
    gate_day_type = DAY_WEEKDAY;
    gate_day_type_after = DAY_WEEKEND;
    gate_day_switch_at = gate_next_day_at(0, 0);
    gate_set_now(gate_at(23, 59));
    gate_clock_after_window = gate_next_day_at(0, 2);
    s_config_locked = true;

    TEST_ASSERT_FALSE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_FALSE(s_config_locked);
    TEST_ASSERT_TRUE(s_config_released);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(gate_next_day_at(0, 2), gate_day_type_arg,
                                    "the re-check resolved the day from the stale caller instant");
}

/* R5. The same rollover WITHOUT a clock step — the ordinary case, one
   whole wake later. The caller's own `now` is already the new day, so
   this is the pre-window check doing the work. */
void test_a_day_rollover_into_a_good_day_type_releases_before_the_window(void) {
    gate_set_pair(DAY_WEEKDAY, 120, 60);
    gate_set_pair(DAY_WEEKEND, 20, 60);
    gate_day_type = DAY_WEEKDAY;
    gate_day_type_after = DAY_WEEKEND;
    gate_day_switch_at = gate_next_day_at(0, 0);
    s_config_locked = true;
    gate_set_now(gate_next_day_at(8, 0));

    TEST_ASSERT_FALSE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_FALSE(s_config_locked);
    TEST_ASSERT_TRUE(s_config_released);
    TEST_ASSERT_EQUAL_INT(0, gate_log_count(EV_NET_WINDOW));
    TEST_ASSERT_EQUAL_INT(DAY_WEEKEND, gate_pair_asked);
}

/* And the converse of R2/R3, or the two above are satisfied by a gate
   that releases unconditionally after a window: a window that changes
   nothing leaves the lock exactly where it was. */
void test_a_window_that_fixes_nothing_keeps_the_lock(void) {
    gate_set_pair(DAY_WEEKDAY, 120, 60);
    s_config_locked = true;
    gate_set_now(gate_at(11, 0));

    TEST_ASSERT_TRUE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_TRUE(s_config_locked);
    TEST_ASSERT_FALSE(s_config_released);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(120, gate_free_min_of(DAY_WEEKDAY), "the fixture fixed the pair by accident");
}

void test_a_config_release_promotes_the_next_partial_render_to_full(void) {
    gate_set_pair(DAY_WEEKDAY, 30, 60);
    s_config_locked = true;
    gate_set_now(gate_at(11, 0));

    TEST_ASSERT_FALSE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_EQUAL_INT(WAKE_RENDER_FULL, lock_gate_promote_render(WAKE_RENDER_PARTIAL));
    /* FULL and an expiry alert own the display already; only PARTIAL is
       promoted. */
    TEST_ASSERT_EQUAL_INT(WAKE_RENDER_FULL, lock_gate_promote_render(WAKE_RENDER_FULL));
}

/* ---- R6: the wake sources survive --------------------------------------- */

/* The one place this lock differs from the bed-time one, and the reason
   the device is recoverable without a cable. Bed time and charge both arm
   NOTHING; this one has to leave D alive so a parent can force the
   corrected config in immediately instead of waiting out an interval. */
void test_the_config_lock_sleeps_with_buttons_still_armed(void) {
    sleep_plan_in_t in = {0};
    sleep_outcome_t out = sleep_plan_outcome(WAKE_SLEEP_CONFIG_ERR, &in);
    TEST_ASSERT_TRUE_MESSAGE(out.enable_buttons, "a config-locked sleep armed no wake source - Button D is dead");
    TEST_ASSERT_EQUAL_UINT32(CONFIG_ERR_SLEEP_SEC, out.seconds);
}

/* The flag the wake mask reads. buttons.c narrows to D alone off this
   answer, so a gate that locked without publishing it would arm the full
   mask and let a stray press cost a refresh on a device that cannot use
   one. */
void test_the_config_lock_publishes_itself_for_the_wake_mask(void) {
    TEST_ASSERT_FALSE(lock_gate_config_locked());
    gate_set_pair(DAY_WEEKDAY, 120, 60);
    gate_set_now(gate_at(9, 0));
    TEST_ASSERT_TRUE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_TRUE(lock_gate_config_locked());
}

void test_the_sleep_mode_tracks_the_config_lock(void) {
    TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_NORMAL, lock_gate_sleep_mode());
    s_config_locked = true;
    TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_CONFIG_ERR, lock_gate_sleep_mode());
    /* Both of the older locks outrank it: a battery that cannot afford a
       refresh cannot afford D either, and at bed time nobody is editing
       config. */
    s_bedtime_locked = true;
    TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_BEDTIME, lock_gate_sleep_mode());
    s_charge_locked = true;
    TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_CHARGE_LOCK, lock_gate_sleep_mode());
}

/* ---- R7: another lock painted over us ----------------------------------- */

/* Bed time engages FIRST and never returns, so a broken pair discovered
   at 21:00 does not get a screen that night. */
void test_bed_time_engages_before_the_config_gate_is_reached(void) {
    gate_set_pair(DAY_WEEKDAY, 120, 60);
    gate_set_bedtime(GATE_BEDTIME_2000);
    gate_set_now(gate_at(21, 0));

    TEST_ASSERT_TRUE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_EQUAL_INT(2, gate_log_count(EV_BEDTIME_SCREEN)); /* engage + pre-sleep repaint */
    TEST_ASSERT_EQUAL_INT(0, gate_log_count(EV_CONFIG_ERR_SCREEN));
    TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_BEDTIME, gate_sleep_mode);
}

/* The morning after: the bed-time lock let go a few lines above this
   gate, so the panel still says "Bed Time" while the config lock is still
   held. Without a repaint the device sits on the wrong screen for as long
   as the config stays broken - exactly the state a parent is being asked
   to recognise and fix.

   THIS CASE USED TO PIN A CONDITION AND NOW PINS A MECHANISM. The gate
   carried an `else if (s_charge_lock_released || s_bedtime_released)`
   arm that repainted before the window; the arm is gone, because it
   enumerated the painters it knew about and missed the window itself
   (M2-T10's HIGH finding). The unconditional pre-sleep repaint subsumes
   it, so the outcome asserted here is unchanged and the count is still
   one - only now the repaint names the pair the POST-window re-check
   read, which the arm could not. */
void test_a_bedtime_release_repaints_the_config_error_screen(void) {
    gate_set_pair(DAY_WEEKDAY, 120, 60);
    gate_set_bedtime(GATE_BEDTIME_2000);
    s_bedtime_locked = true;
    s_config_locked = true; /* engaged yesterday, never let go */
    gate_set_now(gate_at(7, 0));

    TEST_ASSERT_TRUE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_TRUE(s_bedtime_released);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, gate_log_count(EV_CONFIG_ERR_SCREEN),
                                  "the panel was left showing Bed Time under a held config lock");
    TEST_ASSERT_EQUAL_INT(0, gate_log_count(EV_TIMER_PAUSE)); /* a repaint is not a re-engage */
    TEST_ASSERT_TRUE(s_config_locked);
}

/* Same shape through the other lock: Charge Me! is on the panel when the
   battery recovers, and the charge gate releases in main.c's boot phase,
   before this one runs. */
void test_a_charge_release_repaints_the_config_error_screen(void) {
    gate_set_pair(DAY_WEEKDAY, 120, 60);
    s_charge_lock_released = true; /* released earlier in this same wake */
    s_config_locked = true;
    gate_set_now(gate_at(11, 0));

    TEST_ASSERT_TRUE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_EQUAL_INT(1, gate_log_count(EV_CONFIG_ERR_SCREEN));
    TEST_ASSERT_TRUE(s_config_locked);
}

/* The seam BETWEEN the two gates, and the reason the bed-time one takes
   `now` by pointer. Its release-by-edit path returns from behind a
   network window that can carry an NTP step, so the instant this wake
   started is stale from there on — and the config gate's whole question
   is "what day is it TODAY". Judge the caller's instant and a step across
   midnight has the device paint a blocking screen about yesterday's day
   type, then open a SECOND window to discover it was already fine. */
void test_a_clock_step_that_ends_bed_time_also_moves_the_config_gates_day(void) {
    gate_set_bedtime(GATE_BEDTIME_2000);
    s_bedtime_locked = true;
    gate_set_now(gate_at(23, 59));
    gate_clock_after_window = gate_next_day_at(0, 2);
    gate_day_type = DAY_WEEKDAY;       /* the day the wake started in */
    gate_day_type_after = DAY_WEEKEND; /* the day the step lands in */
    gate_day_switch_at = gate_next_day_at(0, 0);
    gate_set_pair(DAY_WEEKDAY, 120, 60); /* broken, and no longer today */
    gate_set_pair(DAY_WEEKEND, 20, 60);  /* today, and fine */

    TEST_ASSERT_FALSE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_TRUE(s_bedtime_released);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, gate_log_count(EV_CONFIG_ERR_SCREEN),
                                  "the config gate judged the instant the wake started, not the stepped clock");
    TEST_ASSERT_FALSE(s_config_locked);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, gate_log_count(EV_NET_WINDOW), "a second network window was opened for nothing");
    TEST_ASSERT_EQUAL_INT(DAY_WEEKEND, gate_pair_asked);
}

/* And the guard against a gate that repaints MORE than it owes. This
   case asserted zero repaints on a locked re-wake until M2-T10's review,
   on the reasoning that e-ink retains so the panel needs nothing — true
   of the panel, false of the wake, because the window in the middle of it
   can paint. The obligation is ONE refresh, taken last; two would mean
   the gate had started repainting on its way in as well. */
void test_a_locked_rewake_repaints_once_and_takes_the_last_word(void) {
    gate_set_pair(DAY_WEEKDAY, 120, 60);
    s_config_locked = true;
    gate_set_now(gate_at(11, 0));

    TEST_ASSERT_TRUE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_EQUAL_INT(1, gate_log_count(EV_CONFIG_ERR_SCREEN));
    TEST_ASSERT_EQUAL_INT(EV_CONFIG_ERR_SCREEN, gate_last_paint_before_sleep());
}

/* ---- R8: the window is a painter ----------------------------------------

   THE CASE NOTHING HELD, and the reason it could not be held from here
   before: net_apply_try_window() was stubbed as a thing that changes
   config values and the clock, so the suite could not express a window
   that reaches the panel. It does reach it — the reconcile fires
   on_active_expired_alert(), whose hook repaints whatever
   display_screen_for() answers — and each gate arms it by pausing a
   running timer on the way in. Under the config lock the screen that
   lands there can be the chore checklist, above an EXT1 mask narrowed to
   Button D: "Chores" offered over a button that is not a wake source. */

void test_a_window_that_paints_does_not_get_the_last_word_under_the_config_lock(void) {
    gate_set_pair(DAY_WEEKDAY, 120, 60);
    s_config_locked = true;
    gate_window_paints = true;
    gate_set_now(gate_at(11, 0));

    TEST_ASSERT_TRUE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, gate_log_count(EV_WINDOW_PAINT), "the fixture's window never painted");
    TEST_ASSERT_EQUAL_INT_MESSAGE(EV_CONFIG_ERR_SCREEN, gate_last_paint_before_sleep(),
                                  "the device slept on a screen the window painted, under a D-only wake mask");
    TEST_ASSERT_TRUE(s_config_locked);
}

/* The same hazard on the ENGAGE wake, where the gate has just paused a
   running timer itself — which is what makes the reconcile able to expire
   it. The engage paint is not the answer: it happens BEFORE the window. */
void test_a_window_that_paints_does_not_get_the_last_word_on_the_config_engage(void) {
    gate_set_pair(DAY_WEEKDAY, 120, 60);
    gate_timer_state = TIMER_RUNNING;
    gate_window_paints = true;
    gate_set_now(gate_at(9, 0));

    TEST_ASSERT_TRUE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_EQUAL_INT(1, gate_log_count(EV_TIMER_PAUSE)); /* the gate armed the painter */
    TEST_ASSERT_EQUAL_INT_MESSAGE(EV_CONFIG_ERR_SCREEN, gate_last_paint_before_sleep(),
                                  "the engage paint was the last one, and the window painted after it");
}

/* Bed time: ~2 h of sleep with NO button armed at all, so nothing but the
   next wake can correct what the window left on the glass. */
void test_a_window_that_paints_does_not_get_the_last_word_under_the_bedtime_lock(void) {
    s_bedtime_locked = true;
    gate_set_bedtime(GATE_BEDTIME_2000);
    gate_window_paints = true;
    gate_set_now(gate_at(23, 0));

    TEST_ASSERT_TRUE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_EQUAL_INT(1, gate_log_count(EV_WINDOW_PAINT));
    TEST_ASSERT_EQUAL_INT(EV_BEDTIME_SCREEN, gate_last_paint_before_sleep());
}

void test_a_window_that_paints_does_not_get_the_last_word_on_the_bedtime_engage(void) {
    gate_set_bedtime(GATE_BEDTIME_2000);
    gate_timer_state = TIMER_RUNNING;
    gate_window_paints = true;
    gate_set_now(gate_at(20, 0));

    TEST_ASSERT_TRUE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_EQUAL_INT(EV_BEDTIME_SCREEN, gate_last_paint_before_sleep());
}

/* The charge lock, and the worst of the three by consequence: this sleep
   arms NOTHING and runs CHARGE_LOCK_SLEEP_SEC, so a screen the window
   leaves behind cannot be cleared by any press at all - only by the
   battery recovering or the interval expiring. */
void test_a_window_that_paints_does_not_get_the_last_word_under_the_charge_lock(void) {
    gate_batt_pct = 5;
    gate_timer_state = TIMER_RUNNING;
    gate_window_paints = true;
    mock_time_set(gate_at(14, 30));

    TEST_ASSERT_TRUE(gate_run(gate_body_check_charge));
    TEST_ASSERT_EQUAL_INT(1, gate_log_count(EV_WINDOW_PAINT));
    TEST_ASSERT_EQUAL_INT_MESSAGE(EV_CHARGE_ME, gate_last_paint_before_sleep(),
                                  "the device slept on a screen the window painted, with no button armed to clear it");
}

/* The charge lock's locked RE-wake is the one path that owes nothing: it
   opens no window (sleep_plan.h), so there is no painter to undo and a
   refresh per 600 s wake on a flat battery is the artifact risk the lock
   exists to avoid. The asymmetry is deliberate and this pins it. */
void test_a_charge_locked_rewake_still_leaves_the_panel_alone(void) {
    s_charge_locked = true;
    gate_batt_pct = 5;
    gate_window_paints = true; /* armed, and never given a window to run in */

    TEST_ASSERT_TRUE(gate_run(gate_body_check_charge));
    TEST_ASSERT_EQUAL_INT(0, gate_log_count(EV_NET_WINDOW));
    TEST_ASSERT_EQUAL_INT(0, gate_log_count(EV_CHARGE_ME));
    TEST_ASSERT_EQUAL_INT(1, gate_log_n);
}

/* THE REPAINT READS THE POST-WINDOW PAIR, which the deleted "another lock
   painted over us" arm could not: it ran before the window. A day
   rollover inside the window into a day type that is ALSO broken has to
   name today's numbers, not the ones the engage screen was built from —
   otherwise the parent fixes the figures the panel shows and nothing
   changes. */
void test_the_repaint_names_the_pair_the_post_window_recheck_read(void) {
    gate_set_pair(DAY_WEEKDAY, 120, 60); /* the day the wake started in */
    gate_set_pair(DAY_SUMMER, 300, 90);  /* the day the step lands in, also broken */
    gate_day_type = DAY_WEEKDAY;
    gate_day_type_after = DAY_SUMMER;
    gate_day_switch_at = gate_next_day_at(0, 0);
    gate_clock_after_window = gate_next_day_at(0, 2);
    s_config_locked = true;
    gate_set_now(gate_at(23, 59));

    TEST_ASSERT_TRUE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_TRUE(s_config_locked); /* still broken, just differently */
    TEST_ASSERT_EQUAL_INT(DAY_SUMMER, gate_screen_day);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(300, gate_screen_free, "the repaint left yesterday's numbers on the glass");
    TEST_ASSERT_EQUAL_UINT16(90, gate_screen_alloc);
}

/* And the converse, or the repaint could be an unconditional paint that
   happens to run before every sleep: a wake that RELEASES the lock falls
   through and must not paint at all, because the wake it returns into
   renders in full (lock_gate_promote_render on the tick path; on the
   button path wake_flow's s_lock_screen_on_glass, set off this gate's
   true return — M3-T4). A repaint here
   would put Config Error on a healthy device and then paint over it. */
void test_a_releasing_wake_does_not_repaint_on_its_way_out(void) {
    gate_set_pair(DAY_WEEKDAY, 120, 60);
    gate_set_pair_after_window(DAY_WEEKDAY, 30, 60);
    gate_window_paints = true;
    s_config_locked = true;
    gate_set_now(gate_at(11, 0));

    TEST_ASSERT_FALSE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_FALSE(s_config_locked);
    TEST_ASSERT_EQUAL_INT(0, gate_log_count(EV_CONFIG_ERR_SCREEN));
    TEST_ASSERT_EQUAL_INT(0, gate_sleep_calls);
}

/* ---- M3-T4: the gate tells its caller that it let go --------------------

   The bool is what makes "fix it in HA, then press D" exactly true: the
   wake handlers consume the press that woke a locked device, drain the
   latch and repaint in full on true, and act normally on false. So every
   RELEASE path has to say true — a path that forgot would hand the D
   press straight to ✓3 or to a second window — and nothing else may, or
   an ordinary press would be thrown away. The wake side is pinned in
   test_wake_flow (test_m3t4_*). */

/* R1, the early path: fixed between wakes, released before any window.
   The one a "post-window only" implementation would miss. */
void test_m3t4_an_early_config_release_says_so(void) {
    gate_set_pair(DAY_WEEKDAY, 30, 60);
    s_config_locked = true;
    gate_set_now(gate_at(11, 0));

    TEST_ASSERT_FALSE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, gate_body_released, "the pre-window release did not report itself");
    TEST_ASSERT_EQUAL_INT(0, gate_log_count(EV_NET_WINDOW));
}

/* R3, the path D exists for: a locked re-wake whose window brings the fix. */
void test_m3t4_a_config_release_in_the_window_says_so(void) {
    gate_set_pair(DAY_WEEKDAY, 120, 60);
    gate_set_pair_after_window(DAY_WEEKDAY, 0, 60);
    s_config_locked = true;
    gate_set_now(gate_at(11, 0));

    TEST_ASSERT_FALSE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, gate_body_released, "the post-window release did not report itself");
    TEST_ASSERT_EQUAL_INT(1, gate_log_count(EV_NET_WINDOW));
}

/* R2: engaged and released inside the same call. Config Error was painted
   this wake, so the press that started it belongs to the lock too. */
void test_m3t4_an_engage_and_release_in_one_call_says_so(void) {
    gate_set_pair(DAY_WEEKDAY, 120, 60);
    gate_set_pair_after_window(DAY_WEEKDAY, 30, 60);
    gate_set_now(gate_at(9, 0));

    TEST_ASSERT_FALSE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_EQUAL_INT(1, gate_log_count(EV_CONFIG_ERR_SCREEN));
    TEST_ASSERT_EQUAL_INT(1, gate_body_released);
}

/* The bed-time lock's two release paths say it as well: its buttons are
   dark, but its window's latch is not. */
void test_m3t4_a_morning_bedtime_release_says_so(void) {
    s_bedtime_locked = true;
    gate_set_bedtime(GATE_BEDTIME_2000);
    gate_set_now(gate_at(7, 0));

    TEST_ASSERT_FALSE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_EQUAL_INT(1, gate_body_released);
}

void test_m3t4_a_bedtime_release_in_the_window_says_so(void) {
    s_bedtime_locked = true;
    gate_set_bedtime(GATE_BEDTIME_2000);
    gate_bedtime_min_after_window = -1; /* parent disables bed time from HA */
    gate_set_now(gate_at(23, 0));

    TEST_ASSERT_FALSE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_EQUAL_INT(1, gate_body_released);
}

/* THE NEGATIVE CONTROLS. An unlocked device — with or without bed time
   configured, and with a pair that is fine — releases nothing, and must
   not say otherwise: every ordinary press would be thrown away. */
void test_m3t4_an_unlocked_wake_reports_no_release(void) {
    gate_set_pair(DAY_WEEKDAY, 20, 60);
    gate_set_bedtime(GATE_BEDTIME_2000);
    gate_set_now(gate_at(9, 0));

    TEST_ASSERT_FALSE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, gate_body_released, "an unlocked wake claimed a release");
}

/* And a lock that holds never returns at all, so it cannot answer. */
void test_m3t4_a_lock_that_holds_never_answers(void) {
    gate_set_pair(DAY_WEEKDAY, 120, 60);
    s_config_locked = true;
    gate_set_now(gate_at(11, 0));

    TEST_ASSERT_TRUE(gate_run(gate_body_check_bedtime));
    TEST_ASSERT_EQUAL_INT(-1, gate_body_released);
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
    RUN_TEST(test_a_valid_pair_leaves_the_wake_alone);
    RUN_TEST(test_the_equal_pair_is_the_off_switch_and_never_locks);
    RUN_TEST(test_a_broken_pair_for_today_engages_and_ends_the_wake);
    RUN_TEST(test_a_broken_pair_for_another_day_type_stays_dormant);
    RUN_TEST(test_the_same_pair_locks_once_that_day_type_is_today);
    RUN_TEST(test_the_screen_is_told_which_pair_is_broken);
    RUN_TEST(test_a_running_timer_is_paused_and_persisted_before_the_config_paint);
    RUN_TEST(test_the_config_engage_does_not_pause_a_timer_that_is_not_running);
    RUN_TEST(test_a_locked_config_rewake_runs_one_window_and_sleeps_again);
    RUN_TEST(test_a_pair_fixed_between_wakes_releases_without_a_window);
    RUN_TEST(test_a_fix_in_the_engage_window_releases_in_the_same_wake);
    RUN_TEST(test_a_fix_in_a_locked_rewake_window_releases_in_the_same_wake);
    RUN_TEST(test_a_clock_step_into_a_good_day_type_releases_in_wake);
    RUN_TEST(test_a_day_rollover_into_a_good_day_type_releases_before_the_window);
    RUN_TEST(test_a_window_that_fixes_nothing_keeps_the_lock);
    RUN_TEST(test_a_config_release_promotes_the_next_partial_render_to_full);
    RUN_TEST(test_the_config_lock_sleeps_with_buttons_still_armed);
    RUN_TEST(test_the_config_lock_publishes_itself_for_the_wake_mask);
    RUN_TEST(test_the_sleep_mode_tracks_the_config_lock);
    RUN_TEST(test_bed_time_engages_before_the_config_gate_is_reached);
    RUN_TEST(test_a_bedtime_release_repaints_the_config_error_screen);
    RUN_TEST(test_a_charge_release_repaints_the_config_error_screen);
    RUN_TEST(test_a_clock_step_that_ends_bed_time_also_moves_the_config_gates_day);
    RUN_TEST(test_a_locked_rewake_repaints_once_and_takes_the_last_word);
    RUN_TEST(test_a_window_that_paints_does_not_get_the_last_word_under_the_config_lock);
    RUN_TEST(test_a_window_that_paints_does_not_get_the_last_word_on_the_config_engage);
    RUN_TEST(test_a_window_that_paints_does_not_get_the_last_word_under_the_bedtime_lock);
    RUN_TEST(test_a_window_that_paints_does_not_get_the_last_word_on_the_bedtime_engage);
    RUN_TEST(test_a_window_that_paints_does_not_get_the_last_word_under_the_charge_lock);
    RUN_TEST(test_a_charge_locked_rewake_still_leaves_the_panel_alone);
    RUN_TEST(test_the_repaint_names_the_pair_the_post_window_recheck_read);
    RUN_TEST(test_a_releasing_wake_does_not_repaint_on_its_way_out);
    RUN_TEST(test_m3t4_an_early_config_release_says_so);
    RUN_TEST(test_m3t4_a_config_release_in_the_window_says_so);
    RUN_TEST(test_m3t4_an_engage_and_release_in_one_call_says_so);
    RUN_TEST(test_m3t4_a_morning_bedtime_release_says_so);
    RUN_TEST(test_m3t4_a_bedtime_release_in_the_window_says_so);
    RUN_TEST(test_m3t4_an_unlocked_wake_reports_no_release);
    RUN_TEST(test_m3t4_a_lock_that_holds_never_answers);
    return UNITY_END();
}
