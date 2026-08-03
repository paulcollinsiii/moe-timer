#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

/* Single-TU: the real policies this module consults — the chime grace
   window, the render choice, the press latch with its A > C > B > D
   priority, and the bed-time crossing rule — are compiled in alongside
   the module under test, so the edges below are pinned against the
   shipping rules rather than a restatement of them. button_latch.c in
   particular is what makes row 5 a statement about the real masked take:
   buttons.c's take wrappers are pure pass-throughs to it (a critical
   section either side), so the stubs below delegate to it exactly as the
   device does. bedtime.c (with the quiet-hours HHMM helpers it leans on)
   is what makes row 21 a statement about the real crossing arithmetic
   rather than about an injected bool. The mock clock comes along because
   every decision here is a wall-time comparison. Everything with a device
   behind it gets a link-time spy stub in the preamble below. */
// clang-format off
#include "../../main/bedtime.c"
#include "../../main/button_latch.c"
#include "../../main/quiet_hours.c"
#include "../../main/wake_policy.c"
#include "mock_hal_time.c"
// clang-format on

#include "alerts.h"
#include "audio.h"
#include "button_actions.h"
#include "buttons.h"
#include "config_cache.h"
#include "display.h"
#include "lock_gate.h"
#include "mqtt_ha.h"
#include "net_apply.h"
#include "net_window.h"
#include "nvs_config.h"
#include "nvs_defaults.h"
#include "sleep_plan.h" /* BREAK_CHIME_GRACE_SEC */
#include "status_led.h"
#include "timer.h"
#include "timer_persist.h"
#include "wake_flow.h"

/* ---- the effect log -----------------------------------------------------

   Row 3 is an ORDERING ("drained before the tick that feeds the render"),
   not an outcome, so the stubs append to a shared log instead of each
   keeping a private counter. Everything is flow_/EV_ prefixed: cppcheck's
   shadowFunction check runs across the whole TU and the module under test
   has locals called now, overdue, extra_running and interrupted. */

typedef enum {
    EV_BREAK_TICK = 1,
    EV_CHIME,
    EV_SNAP_BACK,
    EV_REPAINT,
    /* the guard matrix */
    EV_A_APPLY,
    EV_PAUSE,
    EV_LED,
    EV_NET_OPEN,
    EV_WAIT_NTP,
    EV_TAKE_STEP,
    EV_SHIFT_EXPIRY,
    EV_NOTE_UNSYNCED,
    EV_RELOAD_ALLOWED,
    EV_RELOAD,
    EV_SELECT_NEXT,
    EV_ACTION_RENDER,
    /* the break gate */
    EV_CFG_INTERVAL,
    EV_CFG_DURATION,
    EV_BREAK_DUE,
    EV_BEDTIME_ENGAGE,
    EV_START_BREAK,
    EV_PERSIST_SAVE,
    EV_BREAK_PAINT,
    EV_ALERT_BREAK,
    /* the expiry alert */
    EV_TIMESUP,
    EV_ALERT_EXPIRY,
    /* the day rollover */
    EV_IS_NEW_DAY,
    EV_QUEUE_SUMMARY,
    EV_BONUS_CLEAR,
    EV_TRY_WINDOW,
    EV_PERSIST_RESTORE,
    EV_TIMER_RESET,
    EV_RECORD_DATE,
} flow_event_t;

static flow_event_t flow_log[64];
static int flow_log_n;

static void flow_log_push(flow_event_t ev) {
    if (flow_log_n < (int)(sizeof flow_log / sizeof flow_log[0])) {
        flow_log[flow_log_n++] = ev;
    }
}

/* Position of the first occurrence, or -1. Ordering assertions compare
   two of these; -1 on either side makes the comparison fail loudly rather
   than quietly pass on an effect that never happened. */
static int flow_log_at(flow_event_t ev) {
    for (int i = 0; i < flow_log_n; i++) {
        if (flow_log[i] == ev)
            return i;
    }
    return -1;
}

static int flow_log_count(flow_event_t ev) {
    int n = 0;
    for (int i = 0; i < flow_log_n; i++) {
        if (flow_log[i] == ev)
            n++;
    }
    return n;
}

/* ---- the injected break model -------------------------------------------

   Deliberately not a bare bool. Both rows this suite exists for turn on
   WHEN the latch is set and consumed relative to a tick and a paint, and
   a bool would let either ordering pass. So the stubs model timer.c's
   actual latch: the tick latches the break's WALL end once it has passed
   (and does nothing before that), and take_ended consumes it exactly once
   while reporting how late THAT DRAIN is — which is the number the grace
   window judges. */

#define FLOW_SLOTS 3
#define FLOW_SCREEN 0
#define FLOW_PIANO 2

static bool flow_break_running;    /* slot 0 is TIMER_BREAK */
static time_t flow_break_wall_end; /* when that break is due to end */
static bool flow_latched;          /* the edge timer.c latches */
static time_t flow_latched_wall;   /* the wall end recorded at latch time */
static bool flow_extra_running;
static int flow_active_slot;
static int flow_interrupted_slot;
static int32_t flow_slot_remaining[FLOW_SLOTS];

/* What the paint actually put on the panel. */
static int flow_painted_slot;
static int32_t flow_painted_remaining;

void timer_break_tick(time_t now) {
    flow_log_push(EV_BREAK_TICK);
    if (!flow_break_running || now < flow_break_wall_end)
        return; /* idempotent: nothing to end yet */
    flow_break_running = false;
    flow_latched = true;
    flow_latched_wall = flow_break_wall_end;
}

bool timer_break_take_ended(time_t now, int32_t *overdue_sec) {
    if (overdue_sec)
        *overdue_sec = 0;
    if (!flow_latched)
        return false;
    flow_latched = false;
    if (overdue_sec)
        *overdue_sec = (int32_t)(now - flow_latched_wall);
    return true;
}

bool timer_any_extra_running(void) {
    return flow_extra_running;
}

int timer_active_slot(void) {
    return flow_active_slot;
}

int timer_break_interrupted_slot(void) {
    return flow_interrupted_slot;
}

/* timer.c refuses the snap while the active slot is RUNNING — stealing
   the selection mid-run would be hostile. That is the same condition that
   suppresses the chime, so the refusal is modelled off it rather than off
   a separate switch nothing on device could set independently. */
bool timer_select_interrupted(void) {
    if (flow_extra_running)
        return false;
    flow_active_slot = flow_interrupted_slot;
    flow_log_push(EV_SNAP_BACK);
    return true;
}

void audio_break_over_chime(void) {
    flow_log_push(EV_CHIME);
}

/* The render seam main.c implements: make_state(timer_tick(now), now)
   followed by a full refresh. What reaches the panel is the ACTIVE slot's
   number under the ACTIVE slot's layout — which is exactly what row 3 is
   about, so the stub records both. A paint that runs before the drain
   therefore records the PRE-snap slot, and the row-3 case fails. */
void paint_current_state_full(void) {
    flow_log_push(EV_REPAINT);
    flow_painted_slot = flow_active_slot;
    flow_painted_remaining = flow_slot_remaining[flow_active_slot];
}

/* ---- the injected button + action model ---------------------------------

   Everything the guard matrix can reach. Defaults are POISONED in setUp()
   wherever "never touched" and "touched with the value we expect" would
   otherwise be the same assertion — the captured reload flag starts true
   so that a gate which is never asked cannot read as "asked with false",
   and the captured clock step starts at a value the code can never
   produce. */

static timer_state_t flow_state; /* the ACTIVE slot's state */
static time_t flow_pause_arg;
static btn_a_action_t flow_a_result;
static time_t flow_a_apply_arg;
static bool flow_slot_reloadable;  /* the selected def's reloadable flag */
static int flow_reload_parent_arg; /* tri-state: -1 = never asked */
static bool flow_reload_ok;
static bool flow_select_ok;
static int flow_select_slot;               /* where a successful swap lands */
static timer_state_t flow_state_at_select; /* the state of that slot */
static bool flow_net_open;
static bool flow_ntp_ok;
static int32_t flow_ntp_seconds; /* wall seconds the window burns */
static int64_t flow_clock_step;  /* what the window measured */
static int64_t flow_shift_arg;
static uint32_t flow_delay_at_led; /* mock_delay_total_ms() when the LED first lit */

/* What render_action_result — the seam main.c still implements — was
   handed. All four arguments, because three of them are the row-4/row-20
   contract and the fourth is the clock the dispatch left behind. */
static button_id_t flow_render_btn;
static timer_state_t flow_render_before;
static time_t flow_render_now;
static bool flow_render_selection_changed;

timer_state_t timer_get_state(void) {
    return flow_state;
}

void timer_pause(time_t at) {
    flow_pause_arg = at;
    flow_state = TIMER_PAUSED;
    flow_log_push(EV_PAUSE);
}

/* button_actions.c's map, modelled rather than switched: what it RETURNS
   is injected (that map is test_button_actions' business), but the
   transition it leaves behind is real, so a caller reading timer_get_state()
   after the call sees what the device would show. */
btn_a_action_t button_a_apply(time_t at) {
    flow_a_apply_arg = at;
    flow_log_push(EV_A_APPLY);
    switch (flow_a_result) {
        case BTN_A_PAUSED:
            flow_state = TIMER_PAUSED;
            break;
        case BTN_A_STARTED:
        case BTN_A_RESUMED:
            flow_state = TIMER_RUNNING;
            break;
        default:
            break; /* BTN_A_NONE: BREAK/EXPIRED, no transition */
    }
    return flow_a_result;
}

/* timer.c's rule, modelled off the shipping implementation rather than
   off a free switch: a RUNNING slot is never reloadable (pause first), a
   reloadable def always is, and everything else falls back to the build
   flag. Keeping the RUNNING half real is what makes row 19 a statement
   about the shipping guard instead of about an injected bool. */
bool timer_reload_allowed(bool parent_testing) {
    flow_reload_parent_arg = parent_testing ? 1 : 0;
    flow_log_push(EV_RELOAD_ALLOWED);
    if (flow_state == TIMER_RUNNING)
        return false;
    if (flow_slot_reloadable)
        return true;
    return parent_testing;
}

bool timer_reload(void) {
    flow_log_push(EV_RELOAD);
    if (!flow_reload_ok)
        return false;
    flow_state = TIMER_IDLE; /* the real reload zeroes the slot to IDLE */
    return true;
}

bool timer_select_next(void) {
    flow_log_push(EV_SELECT_NEXT);
    if (!flow_select_ok)
        return false;
    flow_active_slot = flow_select_slot;
    flow_state = flow_state_at_select; /* the slot we landed on */
    return true;
}

void timer_shift_expiry(int64_t delta_sec) {
    flow_shift_arg = delta_sec;
    flow_log_push(EV_SHIFT_EXPIRY);
}

bool net_apply_open(void) {
    flow_log_push(EV_NET_OPEN);
    return flow_net_open;
}

void net_apply_note_start_unsynced(void) {
    flow_log_push(EV_NOTE_UNSYNCED);
}

/* A real window blocks for seconds, which is the whole reason `now` is
   passed by pointer — so the stub burns wall time too. */
bool net_window_wait_ntp(void) {
    flow_log_push(EV_WAIT_NTP);
    if (flow_ntp_seconds != 0) {
        mock_time_set(hal_time_now() + flow_ntp_seconds);
    }
    return flow_ntp_ok;
}

int64_t net_window_take_clock_step(void) {
    flow_log_push(EV_TAKE_STEP);
    return flow_clock_step;
}

/* Records WHEN it first lit, not just that it did: the 250 ms hold before
   it exists so the WHITE/AMBER -> GREEN transition is visible, and a hold
   moved after the LED would be invisible to a plain counter. */
void status_led_show_timer_state(void) {
    if (flow_log_count(EV_LED) == 0) {
        flow_delay_at_led = mock_delay_total_ms();
    }
    flow_log_push(EV_LED);
}

/* The other seam main.c implements. Captures all four arguments: `before`
   and `selection_changed` are the row-4/row-20 contract, and `now` is the
   clock the dispatch left behind. */
void render_action_result(button_id_t btn, timer_state_t before, time_t at, bool selection_changed) {
    flow_render_btn = btn;
    flow_render_before = before;
    flow_render_now = at;
    flow_render_selection_changed = selection_changed;
    flow_log_push(EV_ACTION_RENDER);
}

/* buttons.c's take wrappers are pure pass-throughs to the latch compiled
   in above (it adds only a critical section), so these are the device's
   behaviour, not a model of it. Row 5 is a property of button_latch's
   masked take, and this is what puts the real one under the module. */
uint8_t buttons_take_pressed(void) {
    return button_latch_take();
}

uint8_t buttons_take_pressed_mask(uint8_t mask) {
    return button_latch_take_masked(mask);
}

/* ---- the injected break-gate model --------------------------------------

   The gate reads its two numbers from NVS, and the caller pre-seeds both
   with the compile-time defaults before asking. So the stub models a
   getter that can DECLINE to write (the key was never stored), which is
   the only way the pre-seed is observable at all — a stub that always
   wrote would make deleting those initialisers invisible. */

static bool flow_cfg_writes; /* do the getters fill the out-params? */
static uint16_t flow_interval_min;
static uint16_t flow_duration_min;
static uint16_t flow_seen_interval_default; /* what the caller pre-seeded */
static uint16_t flow_seen_duration_default;

static bool flow_break_due_ret;
static time_t flow_break_due_now;
static int32_t flow_break_due_interval; /* SECONDS, as passed */

static int flow_bed_min; /* what the bed-time cache answers */
static time_t flow_start_break_now;
static int32_t flow_start_break_dur; /* SECONDS, as passed */
static time_t flow_break_paint_now;
static alert_kind_t flow_last_alert;
static bool flow_alert_dismissed;

/* Bed time DOES NOT RETURN on device, so the stub does not either:
   unwinding to the case is what makes "the gate went to bed" and "the
   gate started a break" two distinguishable outcomes. A returning stub
   would let the break start anyway and the row-21 case would still
   pass. */
static jmp_buf flow_bed_jmp;
static bool flow_bed_engaged;
static time_t flow_bed_engage_now;
static bool flow_bed_engage_alert;

esp_err_t nvs_config_get_break_interval_min(uint16_t *out) {
    flow_log_push(EV_CFG_INTERVAL);
    flow_seen_interval_default = *out;
    if (!flow_cfg_writes)
        return ESP_FAIL; /* never stored: the caller's default stands */
    *out = flow_interval_min;
    return ESP_OK;
}

esp_err_t nvs_config_get_break_duration_min(uint16_t *out) {
    flow_log_push(EV_CFG_DURATION);
    flow_seen_duration_default = *out;
    if (!flow_cfg_writes)
        return ESP_FAIL;
    *out = flow_duration_min;
    return ESP_OK;
}

bool timer_break_due(time_t now, int32_t interval_sec) {
    flow_log_push(EV_BREAK_DUE);
    flow_break_due_now = now;
    flow_break_due_interval = interval_sec;
    return flow_break_due_ret;
}

/* Log-only on the host: the sole call sits inside an ESP_LOGI vararg,
   which this module's NATIVE block discards. Defined so the TU links and
   so a mutant that gave it a side effect would still have somewhere to
   land. */
int32_t timer_run_accum(time_t now) {
    (void)now;
    return 0;
}

void timer_start_break(time_t now, int32_t duration_sec) {
    flow_log_push(EV_START_BREAK);
    flow_start_break_now = now;
    flow_start_break_dur = duration_sec;
    flow_state = TIMER_BREAK;
}

int config_cache_bedtime_minutes(void) {
    return flow_bed_min;
}

void lock_gate_bedtime_engage(time_t now, bool alert) {
    flow_log_push(EV_BEDTIME_ENGAGE);
    flow_bed_engaged = true;
    flow_bed_engage_now = now;
    flow_bed_engage_alert = alert;
    longjmp(flow_bed_jmp, 1); /* no return, exactly as on device */
}

void timer_persist_save(void) {
    flow_log_push(EV_PERSIST_SAVE);
}

/* The other paint seam main.c implements. Records the instant it was
   handed: the break must paint against the moment it STARTED, not a
   re-read clock, which is the whole reason this is its own seam. */
void paint_break_started(time_t now) {
    flow_log_push(EV_BREAK_PAINT);
    flow_break_paint_now = now;
    flow_painted_slot = flow_active_slot;
    flow_painted_remaining = flow_slot_remaining[flow_active_slot];
}

bool alert_run(alert_kind_t kind) {
    flow_last_alert = kind;
    flow_log_push(kind == ALERT_EXPIRY ? EV_ALERT_EXPIRY : EV_ALERT_BREAK);
    return flow_alert_dismissed;
}

void display_timesup(void) {
    flow_log_push(EV_TIMESUP);
}

/* ---- the injected day-rollover model ------------------------------------ */

static bool flow_new_day;
static time_t flow_new_day_arg;
static const char *flow_date; /* timer_current_date() */
static int32_t flow_screen_used;
static time_t flow_screen_used_arg;
static uint16_t flow_completions[TIMER_SLOT_COUNT];
static int flow_completion_slots[TIMER_EXTRA_SLOTS]; /* which slots were asked for */
static int flow_completion_asks;
/* How many completions had been read by the time the day's usage was:
   0 means the usage was sampled BEFORE the loop, which is the shipped
   order. Kept out of the shared effect log so the end-to-end trace cases
   stay about the rollover's own steps. */
static int flow_comps_asked_when_used_read;

/* What reached mqtt_ha_queue_summary. */
static const char *flow_summary_date;
static int32_t flow_summary_used;
static uint16_t flow_summary_comp[TIMER_EXTRA_SLOTS];

static bool flow_restore_ok;
static time_t flow_restore_arg;
static time_t flow_record_date_arg;
static bool flow_reset_called;
static time_t flow_clock_after_window; /* 0 = the window does not step the clock */

bool timer_is_new_day(time_t now) {
    flow_log_push(EV_IS_NEW_DAY);
    flow_new_day_arg = now;
    return flow_new_day;
}

const char *timer_current_date(void) {
    return flow_date;
}

int32_t timer_screen_used_sec(time_t now) {
    flow_screen_used_arg = now;
    flow_comps_asked_when_used_read = flow_completion_asks;
    return flow_screen_used;
}

uint16_t timer_slot_completions(int slot) {
    if (flow_completion_asks < TIMER_EXTRA_SLOTS)
        flow_completion_slots[flow_completion_asks++] = slot;
    if (slot < 0 || slot >= TIMER_SLOT_COUNT)
        return 0xFFFFu; /* out of range: a visible wrong answer, not a crash */
    return flow_completions[slot];
}

void mqtt_ha_queue_summary(const char *date, int32_t screen_used_s, const uint16_t completions[TIMER_EXTRA_SLOTS]) {
    flow_log_push(EV_QUEUE_SUMMARY);
    flow_summary_date = date;
    flow_summary_used = screen_used_s;
    memcpy(flow_summary_comp, completions, sizeof flow_summary_comp);
}

void mqtt_ha_queue_bonus_clear(void) {
    flow_log_push(EV_BONUS_CLEAR);
}

/* The rollover's own window. Steps the clock when a case asks, which is
   the whole reason `now` is an out-param. */
esp_err_t net_apply_try_window(void) {
    flow_log_push(EV_TRY_WINDOW);
    if (flow_clock_after_window != 0) {
        mock_time_set(flow_clock_after_window);
    }
    return ESP_OK;
}

bool timer_persist_try_restore(time_t now) {
    flow_log_push(EV_PERSIST_RESTORE);
    flow_restore_arg = now;
    return flow_restore_ok;
}

/* The clock step is load-bearing, not scenery. The mock clock is frozen
   except inside hal_delay_ms(), so with a still clock nothing downstream
   of the rollover's `*now = hal_time_now()` can tell "reuse the corrected
   value" apart from "re-read the clock" — timer_record_date(hal_time_now())
   passes every assertion. On device a real timer_persist_try_restore()
   (NVS blob read + CRC) and this reset (RTC memset) run in that gap, and
   a second boundary landing there at midnight records the wrong date and
   misattributes the day's allocation. Stepping here is what makes
   test_the_recorded_date_is_the_corrected_clock_too able to see it. */
void timer_reset(void) {
    flow_log_push(EV_TIMER_RESET);
    flow_reset_called = true;
    mock_time_set(hal_time_now() + 5);
}

void timer_record_date(time_t now) {
    flow_log_push(EV_RECORD_DATE);
    flow_record_date_arg = now;
}

// clang-format off
#include "../../main/wake_flow.c"
// clang-format on

/* ---- harness ------------------------------------------------------------ */

/* Every case pins the clock to a fixed UTC day so the wall-time
   comparisons read literally. */
#define FLOW_DAY_BASE 1785283200 /* 2026-07-29 00:00:00 UTC */
#define FLOW_SCREEN_REMAINING 1800
#define FLOW_PIANO_REMAINING 400

static time_t flow_at(int hour, int minute) {
    return (time_t)FLOW_DAY_BASE + (time_t)hour * 3600 + (time_t)minute * 60;
}

/* A Screen Break running on slot 0 and due to end at `wall_end`, having
   interrupted `interrupted_slot`, with the selection currently sitting on
   `selected_slot` (a Button C press during the break moves it). */
static void flow_arm_break(time_t wall_end, int interrupted_slot, int selected_slot) {
    flow_break_running = true;
    flow_break_wall_end = wall_end;
    flow_interrupted_slot = interrupted_slot;
    flow_active_slot = selected_slot;
}

/* Presses go in through the real latch, one simulated edge per call.
   Edges are spaced well past BUTTON_LATCH_DEBOUNCE_US so a case that
   presses the same button twice is recording two presses and not
   measuring the debounce, which is test_button_latch's subject. */
static int64_t flow_edge_us;

static void flow_press(button_id_t btn) {
    button_latch_record((int)btn, flow_edge_us);
    flow_edge_us += 10 * BUTTON_LATCH_DEBOUNCE_US;
}

/* What is STILL latched. Consumes, so it is an end-of-case assertion. */
static uint8_t flow_latch_residue(void) {
    return button_latch_take();
}

/* Run the break gate under a landing pad for the bed-time engage, which
   does not return on device. Reports what actually happened, which a bare
   call cannot: FLOW_GATE_BEDTIME and FLOW_GATE_STARTED are the two
   outcomes row 21 has to tell apart. */
typedef enum {
    FLOW_GATE_NO_BREAK = 0, /* returned false */
    FLOW_GATE_STARTED,      /* returned true */
    FLOW_GATE_BEDTIME,      /* never returned: went to bed instead */
} flow_gate_result_t;

static flow_gate_result_t flow_run_break_gate(time_t now) {
    if (setjmp(flow_bed_jmp) != 0)
        return FLOW_GATE_BEDTIME;
    return wake_flow_maybe_start_break(now) ? FLOW_GATE_STARTED : FLOW_GATE_NO_BREAK;
}

/* Minutes-of-day for the bed-time cache, in the same UTC day the clock
   helpers above use. */
static int flow_bed_at(int hour, int minute) {
    return hour * 60 + minute;
}

void setUp(void) {
    /* time_util_minutes_of_day() reads the local calendar, so the day the
       break gate compares against bed time is only arithmetic if the zone
       is pinned. Same fixed UTC day as the clock helpers above. */
    setenv("TZ", "UTC0", 1);
    tzset();
    mock_time_reset();
    flow_log_n = 0;
    flow_break_running = false;
    flow_break_wall_end = 0;
    flow_latched = false;
    flow_latched_wall = 0;
    flow_extra_running = false;
    flow_active_slot = FLOW_SCREEN;
    flow_interrupted_slot = FLOW_SCREEN;
    flow_slot_remaining[0] = FLOW_SCREEN_REMAINING;
    flow_slot_remaining[1] = 900;
    flow_slot_remaining[2] = FLOW_PIANO_REMAINING;
    flow_painted_slot = -1;
    flow_painted_remaining = -1;
    /* Wake-sticky on device (one wake is one boot), so the suite zeroes it
       directly — as test_lock_gate does with the lock flags — rather than
       making wake_flow carry a reset entry point production never calls. */
    s_break_ended = false;

    button_latch_reset();
    flow_edge_us = 1000000;
    flow_state = TIMER_IDLE;
    flow_pause_arg = 0;
    flow_a_result = BTN_A_NONE;
    flow_a_apply_arg = 0;
    flow_slot_reloadable = false;
    flow_reload_ok = true;
    flow_select_ok = true;
    flow_select_slot = FLOW_PIANO;
    flow_state_at_select = TIMER_IDLE;
    flow_net_open = true;
    flow_ntp_ok = true;
    flow_ntp_seconds = 0;
    flow_clock_step = 0;
    flow_render_btn = BTN_NONE;
    flow_render_before = TIMER_IDLE;
    flow_render_selection_changed = false;

    /* The break gate. Stored config by default (the getters answer), a
       balance that has NOT crossed, and bed time disabled — so a case
       opts INTO each of the three things that make the gate act. */
    flow_cfg_writes = true;
    flow_interval_min = 30;
    flow_duration_min = 15;
    flow_break_due_ret = false;
    flow_bed_min = -1; /* disabled: bedtime_* treat negative as off */
    flow_alert_dismissed = false;
    flow_bed_engaged = false;
    flow_bed_engage_alert = false;

    /* The day rollover. Not a new day by default, and a restore that
       fails — so the reset path is the one a case has to ask for. */
    flow_new_day = false;
    flow_date = "2026-07-28";
    flow_screen_used = 1234;
    flow_restore_ok = false;
    flow_reset_called = false;
    flow_clock_after_window = 0;
    flow_completion_asks = 0;
    flow_comps_asked_when_used_read = -1; /* poisoned: never read */
    for (int i = 0; i < TIMER_SLOT_COUNT; i++) {
        /* Distinct per slot so the 1+i offset is observable: a mutant
           reading slot i instead would report Screen's number. */
        flow_completions[i] = (uint16_t)(100 + i);
    }
    for (int i = 0; i < TIMER_EXTRA_SLOTS; i++) {
        flow_completion_slots[i] = -1;
        flow_summary_comp[i] = 0xEEEEu;
    }

    /* Poisoned: values the module cannot produce, so "never written" and
       "written with what we expected" can never be the same assertion. */
    flow_reload_parent_arg = -1; /* never asked */
    flow_shift_arg = -424242;
    flow_delay_at_led = 0xFFFFFFFFu;
    flow_render_now = -1;
    flow_seen_interval_default = 0xFFFFu;
    flow_seen_duration_default = 0xFFFFu;
    flow_break_due_now = -1;
    flow_break_due_interval = -1;
    flow_start_break_now = -1;
    flow_start_break_dur = -1;
    flow_break_paint_now = -1;
    flow_last_alert = (alert_kind_t)-1;
    flow_bed_engage_now = -1;
    flow_new_day_arg = -1;
    flow_screen_used_arg = -1;
    flow_summary_date = NULL;
    flow_summary_used = -424242;
    flow_restore_arg = -1;
    flow_record_date_arg = -1;
}

void tearDown(void) {}

/* ---- reset-reason map --------------------------------------------------- */

/* These strings ship: they land in the HA "Last reset" sensor and in the
   late-wake log line, which together are the only forensics that survive
   the USB CDC console dropping output around a sleep/reset transition.
   Renaming one silently breaks a dashboard, so the table is pinned
   exhaustively rather than spot-checked. */

void test_deepsleep_is_the_healthy_reason(void) {
    TEST_ASSERT_EQUAL_STRING("DEEPSLEEP", wake_flow_reset_reason_str(ESP_RST_DEEPSLEEP));
}

void test_poweron_and_external_reset(void) {
    TEST_ASSERT_EQUAL_STRING("POWERON", wake_flow_reset_reason_str(ESP_RST_POWERON));
    TEST_ASSERT_EQUAL_STRING("EXT", wake_flow_reset_reason_str(ESP_RST_EXT));
}

void test_software_reset(void) {
    TEST_ASSERT_EQUAL_STRING("SW", wake_flow_reset_reason_str(ESP_RST_SW));
}

void test_crash_reasons(void) {
    TEST_ASSERT_EQUAL_STRING("PANIC", wake_flow_reset_reason_str(ESP_RST_PANIC));
    TEST_ASSERT_EQUAL_STRING("BROWNOUT", wake_flow_reset_reason_str(ESP_RST_BROWNOUT));
}

void test_watchdogs_are_distinguishable(void) {
    /* Three separate watchdogs with three separate causes — collapsing
       them into one string would lose which subsystem wedged. */
    TEST_ASSERT_EQUAL_STRING("INT_WDT", wake_flow_reset_reason_str(ESP_RST_INT_WDT));
    TEST_ASSERT_EQUAL_STRING("TASK_WDT", wake_flow_reset_reason_str(ESP_RST_TASK_WDT));
    TEST_ASSERT_EQUAL_STRING("WDT", wake_flow_reset_reason_str(ESP_RST_WDT));
}

void test_unknown_reason(void) {
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", wake_flow_reset_reason_str(ESP_RST_UNKNOWN));
}

void test_unmapped_reason_falls_back_to_unknown(void) {
    /* The production enum carries reasons this map does not name (SDIO,
       USB, JTAG, ...). They must degrade to a string, never to NULL — the
       result is passed straight into a log format and a JSON payload. */
    const char *s = wake_flow_reset_reason_str((esp_reset_reason_t)(ESP_RST_BROWNOUT + 1));
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", s);
}

void test_every_reason_maps_to_a_distinct_non_empty_string(void) {
    const esp_reset_reason_t all[] = {ESP_RST_UNKNOWN,   ESP_RST_POWERON, ESP_RST_EXT,      ESP_RST_SW,
                                      ESP_RST_PANIC,     ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT,
                                      ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT};
    const size_t n = sizeof(all) / sizeof(all[0]);
    for (size_t i = 0; i < n; i++) {
        const char *a = wake_flow_reset_reason_str(all[i]);
        TEST_ASSERT_NOT_NULL(a);
        TEST_ASSERT_TRUE(a[0] != '\0');
        /* UNKNOWN doubles as the fallback, so it is the only reason
           allowed to share its string with the unmapped case. */
        for (size_t j = i + 1; j < n; j++) {
            TEST_ASSERT_FALSE_MESSAGE(strcmp(a, wake_flow_reset_reason_str(all[j])) == 0,
                                      "two reset reasons share a string");
        }
    }
}

/* ---- the break-end edge: when there is one ------------------------------ */

/* The overwhelmingly common wake. The drain runs on every path that does
   timer work, so its no-edge behaviour has to be completely inert — a
   stray chime or a stray full refresh here would land on every tick. */
void test_a_wake_with_no_break_is_inert(void) {
    mock_time_set(flow_at(14, 0));
    TEST_ASSERT_FALSE(wake_flow_break_end());
    TEST_ASSERT_FALSE(wake_flow_break_ended_this_wake());
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_CHIME));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_SNAP_BACK));
}

void test_a_break_still_running_is_not_an_edge(void) {
    flow_arm_break(flow_at(16, 0), FLOW_SCREEN, FLOW_SCREEN);
    mock_time_set(flow_at(15, 55)); /* five minutes of break left */

    TEST_ASSERT_FALSE(wake_flow_break_end());

    TEST_ASSERT_FALSE(wake_flow_break_ended_this_wake());
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_CHIME));
    TEST_ASSERT_TRUE(flow_break_running); /* still on the break screen */
}

/* Regression 4eb1549, first half. The drain-only version of this function
   missed a break that elapsed while the device was busy, because the
   latch only exists once something has TICKED. The sharpest caller is the
   break-tail watch, whose wait loop exits on wall time without ticking
   anything: the drain right after it found nothing, skipped the chime and
   the repaint, and the break-over chime slipped to the next wake. That is
   the primary break-end path, so it is modelled here exactly — the break
   is elapsed and nothing has latched it. */
void test_an_elapsed_break_that_nothing_ticked_still_chimes(void) {
    flow_arm_break(flow_at(16, 0), FLOW_SCREEN, FLOW_SCREEN);
    mock_time_set(flow_at(16, 0) + 2);
    TEST_ASSERT_FALSE(flow_latched); /* nothing has ticked this wake */

    TEST_ASSERT_TRUE(wake_flow_break_end());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_CHIME));
    TEST_ASSERT_TRUE(wake_flow_break_ended_this_wake());
}

/* The exact instant the primary break-end path lands on, and the reason
   this pair exists rather than a single "elapsed" case. timer.c latches
   at `now >= break_expiry_wall`, and watch_break_end()'s wait loop exits
   the moment timer_break_remaining() reaches 0 — which is `now ==
   break_expiry_wall` precisely, not a second later — and only then calls
   the drain. A clock slipped one second late here latches nothing at that
   instant: no chime, no snap back, no repaint, and regression 4eb1549 is
   back, one second wide and on the path it is most likely to be hit.
   Every other case in this suite sits at wall_end + 1 or later, so none
   of them would notice. */
void test_the_end_latches_at_the_instant_the_wall_end_is_reached(void) {
    flow_arm_break(flow_at(16, 0), FLOW_SCREEN, FLOW_PIANO);
    mock_time_set(flow_at(16, 0)); /* on the wall end, not one second past it */

    TEST_ASSERT_TRUE(wake_flow_break_end());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_CHIME));
    TEST_ASSERT_EQUAL_INT(FLOW_SCREEN, flow_active_slot); /* snapped back */
    TEST_ASSERT_TRUE(wake_flow_break_ended_this_wake());
}

/* The other side of the same boundary: one second short is still a
   running break. Ending it early would chime and snap the selection away
   while the panel's own countdown still reads a second left. Paired with
   the case above the two bracket the comparison — either one alone passes
   against a slip in the opposite direction. */
void test_a_break_one_second_short_of_its_wall_end_is_not_an_edge(void) {
    flow_arm_break(flow_at(16, 0), FLOW_SCREEN, FLOW_PIANO);
    mock_time_set(flow_at(16, 0) - 1);

    TEST_ASSERT_FALSE(wake_flow_break_end());

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_CHIME));
    TEST_ASSERT_FALSE(wake_flow_break_ended_this_wake());
    TEST_ASSERT_TRUE(flow_break_running);                /* still on the break screen */
    TEST_ASSERT_EQUAL_INT(FLOW_PIANO, flow_active_slot); /* selection untouched */
}

/* Regression 1520486, defects 2+3, and the reason the edge is a LATCH
   rather than a return value: timer_tick() ends an elapsed break
   internally, so the expiry alert's own tick could consume the edge and
   lose the chime with no recovery path. Modelled here as an edge that is
   already latched with no break left running — whoever ticked is gone. */
void test_a_latch_left_by_a_foreign_tick_is_still_drained(void) {
    flow_latched = true;
    flow_latched_wall = flow_at(16, 0);
    flow_interrupted_slot = FLOW_SCREEN;
    flow_active_slot = FLOW_PIANO;
    mock_time_set(flow_at(16, 0) + 4);

    TEST_ASSERT_TRUE(wake_flow_break_end_repaint());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_CHIME));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_REPAINT));
    TEST_ASSERT_TRUE(wake_flow_break_ended_this_wake());
}

/* Seven call sites share this function precisely because it is safe to
   call anywhere; that is only true if the edge is consumed exactly once. */
void test_the_edge_is_taken_exactly_once_per_wake(void) {
    flow_arm_break(flow_at(16, 0), FLOW_SCREEN, FLOW_PIANO);
    mock_time_set(flow_at(16, 0) + 1);

    TEST_ASSERT_TRUE(wake_flow_break_end());
    TEST_ASSERT_FALSE(wake_flow_break_end());
    TEST_ASSERT_FALSE(wake_flow_break_end());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_CHIME));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_SNAP_BACK));
}

/* ---- the chime and the snap are one event ------------------------------- */

/* Rule 4: the kid is mid-activity on another timer, so that timer's own
   alert is the one that matters. The end is still real — the BREAK chip
   leaves the panel — so the edge is still reported and the wake-sticky
   flag still set; only the sound and the selection change are suppressed. */
void test_a_running_extra_suppresses_the_chime_and_the_snap(void) {
    flow_extra_running = true;
    flow_arm_break(flow_at(16, 0), FLOW_SCREEN, FLOW_PIANO);
    mock_time_set(flow_at(16, 0) + 1);

    TEST_ASSERT_TRUE(wake_flow_break_end());

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_CHIME));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_SNAP_BACK));
    TEST_ASSERT_EQUAL_INT(FLOW_PIANO, flow_active_slot); /* selection untouched */
    TEST_ASSERT_TRUE(wake_flow_break_ended_this_wake());
}

/* Rule 6, and the number that decides it: what the grace window judges is
   how late THE DRAIN is, not how late the tick was — the chime is an "it
   just happened" signal, not a replay, and a charge lock or a power cycle
   can span the end. The pair brackets the boundary: either side alone
   passes against an off-by-one or against a hard-coded lateness. */
void test_an_end_observed_inside_the_grace_still_chimes(void) {
    flow_arm_break(flow_at(16, 0), FLOW_SCREEN, FLOW_SCREEN);
    mock_time_set(flow_at(16, 0) + BREAK_CHIME_GRACE_SEC);

    TEST_ASSERT_TRUE(wake_flow_break_end());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_CHIME));
}

void test_an_end_observed_past_the_grace_is_silent(void) {
    flow_arm_break(flow_at(16, 0), FLOW_SCREEN, FLOW_PIANO);
    mock_time_set(flow_at(16, 0) + BREAK_CHIME_GRACE_SEC + 1);

    TEST_ASSERT_TRUE(wake_flow_break_end()); /* still an edge */

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_CHIME));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_SNAP_BACK)); /* the snap rides the chime */
    TEST_ASSERT_TRUE(wake_flow_break_ended_this_wake());
}

/* "The chime and the return are the same event: the break is over, so you
   go back to whatever it interrupted." Both halves, and their order. */
void test_the_chime_and_the_snap_back_are_one_event(void) {
    flow_arm_break(flow_at(16, 0), FLOW_SCREEN, FLOW_PIANO);
    mock_time_set(flow_at(16, 0) + 1);

    TEST_ASSERT_TRUE(wake_flow_break_end());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_CHIME));
    TEST_ASSERT_EQUAL_INT(FLOW_SCREEN, flow_active_slot);
    TEST_ASSERT_TRUE(flow_log_at(EV_CHIME) < flow_log_at(EV_SNAP_BACK));
}

/* Rule 8: a break can be earned entirely by a non-eligible extra, so the
   return is to the slot the break INTERRUPTED — not unconditionally to
   Screen. Here Piano was interrupted and Screen is selected. */
void test_the_snap_returns_to_the_interrupted_slot_not_always_screen(void) {
    flow_arm_break(flow_at(16, 0), FLOW_PIANO, FLOW_SCREEN);
    mock_time_set(flow_at(16, 0) + 1);

    TEST_ASSERT_TRUE(wake_flow_break_end());

    TEST_ASSERT_EQUAL_INT(FLOW_PIANO, flow_active_slot);
}

/* Already sitting on the interrupted slot: chime, but no selection call —
   the guard is what keeps a no-op out of the selection machinery. */
void test_no_snap_when_the_selection_is_already_where_it_belongs(void) {
    flow_arm_break(flow_at(16, 0), FLOW_SCREEN, FLOW_SCREEN);
    mock_time_set(flow_at(16, 0) + 1);

    TEST_ASSERT_TRUE(wake_flow_break_end());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_CHIME));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_SNAP_BACK));
    TEST_ASSERT_EQUAL_INT(FLOW_SCREEN, flow_active_slot);
}

/* ---- ROW 3: drained before the tick that feeds the render --------------- */

/* Regression 4eb1549, second half — the defect the report actually named.
   A Screen Break earned by Screen; during it the kid pressed Button C and
   moved the selection to Piano. The break ends, the chime snaps the
   selection back to Screen — and the number painted must be SCREEN's
   30:00. Ticking first and draining afterwards painted Piano's 6:40 under
   Screen's name and allocation, which is what shipped.

   The seam stub records the slot AND its remaining at paint time, so the
   wrong order fails on the value, not merely on the call sequence. */
void test_row3_the_repaint_shows_the_snapped_back_slot(void) {
    flow_arm_break(flow_at(16, 0), FLOW_SCREEN, FLOW_PIANO);
    mock_time_set(flow_at(16, 0) + 3);

    TEST_ASSERT_TRUE(wake_flow_break_end_repaint());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_REPAINT));
    TEST_ASSERT_EQUAL_INT(FLOW_SCREEN, flow_painted_slot);
    TEST_ASSERT_EQUAL_INT32(FLOW_SCREEN_REMAINING, flow_painted_remaining);
    /* and the ordering that produces it */
    TEST_ASSERT_TRUE(flow_log_at(EV_SNAP_BACK) < flow_log_at(EV_REPAINT));
}

/* The guaranteed drain runs on every wake that does timer work. A repaint
   without an edge would mean a full e-ink refresh every minute, all day —
   the battery cost the partial cadence exists to avoid. */
void test_a_wake_without_a_break_end_never_repaints(void) {
    mock_time_set(flow_at(14, 0));
    TEST_ASSERT_FALSE(wake_flow_break_end_repaint());
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_REPAINT));

    /* and mid-break, which is the other way to reach the drain with no edge */
    flow_arm_break(flow_at(16, 0), FLOW_SCREEN, FLOW_SCREEN);
    mock_time_set(flow_at(15, 30));
    TEST_ASSERT_FALSE(wake_flow_break_end_repaint());
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_REPAINT));
}

/* Both sites that repaint reach it after something else may already have
   drained (the break watch drains, then the guaranteed drain runs). The
   second one must not repaint again. */
void test_the_repaint_happens_once_per_edge(void) {
    flow_arm_break(flow_at(16, 0), FLOW_SCREEN, FLOW_PIANO);
    mock_time_set(flow_at(16, 0) + 1);

    TEST_ASSERT_TRUE(wake_flow_break_end_repaint());
    TEST_ASSERT_FALSE(wake_flow_break_end_repaint());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_REPAINT));
}

/* A silent end still changes the panel: the inverted BREAK chip is gone.
   Tying the repaint to the chime instead of to the edge would leave the
   stale chip on the e-ink until the next state change. */
void test_a_silent_break_end_still_repaints(void) {
    flow_extra_running = true;
    flow_arm_break(flow_at(16, 0), FLOW_SCREEN, FLOW_PIANO);
    mock_time_set(flow_at(16, 0) + 1);

    TEST_ASSERT_TRUE(wake_flow_break_end_repaint());

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_CHIME));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_REPAINT));
}

/* ---- ROW 4: the wake-sticky flag and the ghost -------------------------- */

/* The flag is a level for the rest of the wake but an EDGE to set: a
   device that merely happens not to be on a break must not claim one
   ended, or every render would be promoted to full. */
void test_the_flag_is_not_set_without_an_edge(void) {
    mock_time_set(flow_at(14, 0));
    TEST_ASSERT_FALSE(wake_flow_break_ended_this_wake());
    wake_flow_break_end();
    TEST_ASSERT_FALSE(wake_flow_break_ended_this_wake());
}

/* Set by ANY drained edge, including the silent one. A suppressed chime
   still means the chip vanished from the panel, so the promotion it owes
   the render is identical — putting the assignment inside the chiming arm
   would ghost every silent end. */
void test_any_edge_sets_the_flag_including_a_silent_one(void) {
    flow_extra_running = true;
    flow_arm_break(flow_at(16, 0), FLOW_SCREEN, FLOW_PIANO);
    mock_time_set(flow_at(16, 0) + 1);

    TEST_ASSERT_TRUE(wake_flow_break_end());

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_CHIME));  /* silent... */
    TEST_ASSERT_TRUE(wake_flow_break_ended_this_wake()); /* ...but still promoted */
}

/* Sticky for the whole wake. The three renders that read it run at
   different points — the post-action tail, the post-network re-render,
   and the tick handler's own paint — and only the first of them can be
   the call that drained the edge. A flag that tracked the last drain's
   return value would ghost the other two. */
void test_the_flag_survives_a_later_drain_that_found_nothing(void) {
    flow_arm_break(flow_at(16, 0), FLOW_SCREEN, FLOW_PIANO);
    mock_time_set(flow_at(16, 0) + 1);

    TEST_ASSERT_TRUE(wake_flow_break_end());
    TEST_ASSERT_FALSE(wake_flow_break_end()); /* nothing left to drain */

    TEST_ASSERT_TRUE(wake_flow_break_ended_this_wake());
}

/* Regression 1520486, defect 1. The break screen is a full-screen
   INVERSION of the main layout, so any render that crosses it with a
   partial diff ghosts the whole panel — and display.c promotes only every
   fifth partial, so four presses in five ghosted. After a break end every
   render this wake crosses that boundary: the chip has gone, and a
   chiming end has also changed which timer's layout is showing.

   The flag is what carries that across the wake, so the property is
   asserted over the entire state matrix, composed with the shipping
   wake_policy_render rather than a restatement of it. The single
   exception is the expiry alert, which outranks a repaint by design. */
void test_row4_a_break_end_forces_every_later_render_full(void) {
    static const timer_state_t states[] = {TIMER_IDLE, TIMER_RUNNING, TIMER_PAUSED, TIMER_EXPIRED, TIMER_BREAK};
    const int n_states = (int)(sizeof(states) / sizeof(states[0]));

    flow_arm_break(flow_at(16, 0), FLOW_SCREEN, FLOW_PIANO);
    mock_time_set(flow_at(16, 0) + 1);
    TEST_ASSERT_TRUE(wake_flow_break_end());
    TEST_ASSERT_TRUE(wake_flow_break_ended_this_wake());

    int partials_without_the_flag = 0;
    for (int b = 0; b < n_states; b++) {
        for (int a = 0; a < n_states; a++) {
            for (int btn = 0; btn < 2; btn++) {
                for (int sel = 0; sel < 2; sel++) {
                    bool alerting = (states[a] == TIMER_EXPIRED && states[b] != TIMER_EXPIRED && sel == 0);
                    wake_render_t wr =
                        wake_policy_render(states[b], states[a], btn != 0, wake_flow_break_ended_this_wake(), sel != 0);
                    if (alerting) {
                        TEST_ASSERT_EQUAL_INT(WAKE_RENDER_EXPIRY_ALERT, wr);
                        continue;
                    }
                    TEST_ASSERT_EQUAL_INT_MESSAGE(WAKE_RENDER_FULL, wr, "a render after a break end went partial");
                    /* Non-vacuity: the same matrix without the flag has
                       real PARTIAL entries, so the assertion above is
                       carried by the flag and not by the state pair. */
                    if (wake_policy_render(states[b], states[a], btn != 0, false, sel != 0) == WAKE_RENDER_PARTIAL) {
                        partials_without_the_flag++;
                    }
                }
            }
        }
    }
    TEST_ASSERT_TRUE(partials_without_the_flag > 0);
}

/* NOT a wake_flow test, and deliberately labelled as such: every input
   below is a literal, so nothing here exercises the code that DECIDES
   `before`. It is kept as the truth table the boundary rule states, in
   one place, for the cases the guard matrix feeds it.

   The row-4 defect proper — 1520486 defect 1: dispatch_button_action
   (as it was then named, before this cycle moved it here)
   rewriting *before on a successful swap, so four presses in five
   ghosted — is pinned END TO END further down, by driving the real
   wake_flow_dispatch_button_action and handing ITS outputs to the real
   wake_policy_render (test_row4_a_swap_during_a_break_renders_full_end_to_end
   and its two siblings). Those are the tests that would fail if the
   signature were "cleaned up"; this one would not. */
void test_a_swap_across_the_break_screen_is_full_by_the_boundary_rule(void) {
    /* off the break screen onto an idle extra, and back again */
    TEST_ASSERT_EQUAL_INT(WAKE_RENDER_FULL, wake_policy_render(TIMER_BREAK, TIMER_IDLE, true, false, true));
    TEST_ASSERT_EQUAL_INT(WAKE_RENDER_FULL, wake_policy_render(TIMER_PAUSED, TIMER_BREAK, true, false, true));
    /* and onto an already-EXPIRED slot, where the swap suppresses the
       alert: suppressed must still mean a FULL repaint, never a partial
       one across the inversion. */
    TEST_ASSERT_EQUAL_INT(WAKE_RENDER_FULL, wake_policy_render(TIMER_BREAK, TIMER_EXPIRED, true, false, true));

    /* The shipped defect, as the negative control: with `before`
       clobbered to the post-swap state the boundary is lost and the panel
       renders partial — which is what ghosted. */
    TEST_ASSERT_EQUAL_INT(WAKE_RENDER_PARTIAL, wake_policy_render(TIMER_IDLE, TIMER_IDLE, true, false, true));
}

/* ======================================================================
   THE GUARD MATRIX
   ====================================================================== */

/* Shared shorthand: run a dispatch with the out-params poisoned, so every
   case can assert on both of them without restating the setup. `swapped`
   starts TRUE on purpose — the out-param contract is that the function
   CLEARS it, and a default of false would let a missing write pass. */
static time_t flow_now_io;
static bool flow_swapped_io;

static bool flow_dispatch(button_id_t btn, time_t at, timer_state_t before, bool allow_net_window) {
    flow_now_io = at;
    flow_swapped_io = true;
    return wake_flow_dispatch_button_action(btn, &flow_now_io, before, allow_net_window, &flow_swapped_io);
}

/* ---- ROW 5: the masked take -------------------------------------------

   Regression 8fc8de8. The awake polls run inside loops that own the CPU
   for tens of seconds (the render grid wait, the final-minute watch), and
   they care about exactly one button. An unmasked take there swallows the
   B and C presses that the tick-wake drain at the end of the wake is
   supposed to act on, and those presses are then discarded at deep sleep
   — the press simply does nothing. Both polls are pinned, because both
   take, and a fix applied to only one of them shipped once already. */

void test_row5_the_pause_poll_consumes_only_the_a_bit(void) {
    flow_state = TIMER_RUNNING;
    flow_press(BTN_A);
    flow_press(BTN_B);
    flow_press(BTN_C);
    mock_time_set(flow_at(14, 0));

    TEST_ASSERT_TRUE(wake_flow_poll_pause_button());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_PAUSE));
    TEST_ASSERT_EQUAL_HEX8((1u << BTN_B) | (1u << BTN_C), flow_latch_residue());
}

/* The refusal path takes too — the take happens before the guard — so the
   mask has to be right on the branch that does nothing, which is the
   overwhelmingly common one (every poll of every grid wait). */
void test_row5_a_refused_pause_poll_still_leaves_b_and_c_latched(void) {
    flow_state = TIMER_IDLE;
    flow_press(BTN_A);
    flow_press(BTN_B);
    flow_press(BTN_C);
    mock_time_set(flow_at(14, 0));

    TEST_ASSERT_FALSE(wake_flow_poll_pause_button());

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PAUSE));
    TEST_ASSERT_EQUAL_HEX8((1u << BTN_B) | (1u << BTN_C), flow_latch_residue());
}

void test_row5_the_join_poll_consumes_only_the_a_bit(void) {
    flow_a_result = BTN_A_STARTED;
    flow_press(BTN_A);
    flow_press(BTN_B);
    flow_press(BTN_C);
    mock_time_set(flow_at(14, 0));

    TEST_ASSERT_TRUE(wake_flow_poll_button_a_action());

    TEST_ASSERT_EQUAL_HEX8((1u << BTN_B) | (1u << BTN_C), flow_latch_residue());
}

/* The join poll's take runs first and its guard second, so an unmasked
   take here would eat B and C on the far more common no-A poll — the
   join-wait poll fires every 100 ms for as long as the MQTT tail runs. */
void test_row5_a_join_poll_with_no_a_press_leaves_b_and_c_latched(void) {
    flow_press(BTN_B);
    flow_press(BTN_C);
    mock_time_set(flow_at(14, 0));

    TEST_ASSERT_FALSE(wake_flow_poll_button_a_action());

    TEST_ASSERT_EQUAL_HEX8((1u << BTN_B) | (1u << BTN_C), flow_latch_residue());
}

/* The A press itself IS consumed on the refusal path — the take is
   unconditional and precedes the state guard. Pinned rather than
   corrected: it is the shipping behaviour, and it is what stops a single
   press from pausing twice when two polls straddle it. */
void test_the_pause_poll_consumes_the_a_press_even_when_it_refuses(void) {
    flow_state = TIMER_IDLE;
    flow_press(BTN_A);
    mock_time_set(flow_at(14, 0));

    TEST_ASSERT_FALSE(wake_flow_poll_pause_button());

    TEST_ASSERT_EQUAL_HEX8(0, flow_latch_residue()); /* gone, not left for later */
}

/* ---- the awake pause poll ---------------------------------------------- */

/* Pause is the one action that must not be lost, and the only one this
   poll performs: every other state either has nothing to pause or is
   wake-press-only. A poll that acted on IDLE would start a timer from
   inside a render wait, with no repaint behind it. */
void test_the_pause_poll_pauses_only_a_running_timer(void) {
    static const timer_state_t idle_states[] = {TIMER_IDLE, TIMER_PAUSED, TIMER_EXPIRED, TIMER_BREAK};
    for (int i = 0; i < (int)(sizeof(idle_states) / sizeof(idle_states[0])); i++) {
        setUp();
        flow_state = idle_states[i];
        flow_press(BTN_A);
        mock_time_set(flow_at(14, 0));

        TEST_ASSERT_FALSE(wake_flow_poll_pause_button());
        TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PAUSE));
        TEST_ASSERT_EQUAL_INT(idle_states[i], flow_state); /* untouched */
    }

    setUp();
    flow_state = TIMER_RUNNING;
    flow_press(BTN_A);
    mock_time_set(flow_at(14, 0));
    TEST_ASSERT_TRUE(wake_flow_poll_pause_button());
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_PAUSE));
}

void test_the_pause_poll_does_nothing_without_an_a_press(void) {
    flow_state = TIMER_RUNNING;
    mock_time_set(flow_at(14, 0));

    TEST_ASSERT_FALSE(wake_flow_poll_pause_button());

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PAUSE));
    TEST_ASSERT_EQUAL_INT(TIMER_RUNNING, flow_state);
}

/* The pause is stamped with the clock read at the press, not with a stale
   `now` the caller carried in: the remaining time frozen into the slot is
   computed from it, so a stale stamp refunds or steals real seconds. */
void test_the_pause_poll_stamps_the_clock_it_reads_now(void) {
    flow_state = TIMER_RUNNING;
    flow_press(BTN_A);
    mock_time_set(flow_at(15, 47) + 23);

    TEST_ASSERT_TRUE(wake_flow_poll_pause_button());

    TEST_ASSERT_EQUAL_INT64(flow_at(15, 47) + 23, flow_pause_arg);
}

/* One press, one pause: the watches call this every 250 ms, so a poll
   that re-paused on a consumed latch would re-stamp the slot on every
   iteration of the final minute. */
void test_the_pause_poll_pauses_at_most_once_per_press(void) {
    flow_state = TIMER_RUNNING;
    flow_press(BTN_A);
    mock_time_set(flow_at(14, 0));

    TEST_ASSERT_TRUE(wake_flow_poll_pause_button());
    flow_state = TIMER_RUNNING; /* pretend it kept running */
    TEST_ASSERT_FALSE(wake_flow_poll_pause_button());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_PAUSE));
}

/* ---- the join poll ----------------------------------------------------- */

/* The guard is the take, and it comes first: without a latched A the
   state map must never be consulted. It has side effects — it starts,
   pauses and resumes the active slot — so calling it speculatively on
   every 100 ms join poll would start a timer nobody pressed for. */
void test_the_join_poll_is_inert_without_a_latched_a(void) {
    flow_a_result = BTN_A_STARTED;
    mock_time_set(flow_at(14, 0));

    TEST_ASSERT_FALSE(wake_flow_poll_button_a_action());

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_A_APPLY));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_LED));
    TEST_ASSERT_EQUAL_INT(TIMER_IDLE, flow_state);
}

/* BREAK and EXPIRED stay wake-press-only: the map returns NONE, and the
   poll must then report nothing AND leave the LEDs alone — the pixel
   showing "heard you" when nothing happened is worse than silence. */
void test_the_join_poll_reports_nothing_when_the_state_map_refuses(void) {
    flow_state = TIMER_BREAK;
    flow_a_result = BTN_A_NONE;
    flow_press(BTN_A);
    mock_time_set(flow_at(14, 0));

    TEST_ASSERT_FALSE(wake_flow_poll_button_a_action());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_A_APPLY)); /* it was asked... */
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_LED));     /* ...and said no */
}

void test_the_join_poll_acks_on_the_leds_for_every_accepted_action(void) {
    static const btn_a_action_t accepted[] = {BTN_A_STARTED, BTN_A_RESUMED, BTN_A_PAUSED};
    for (int i = 0; i < (int)(sizeof(accepted) / sizeof(accepted[0])); i++) {
        setUp();
        flow_a_result = accepted[i];
        flow_press(BTN_A);
        mock_time_set(flow_at(14, 0));

        TEST_ASSERT_TRUE(wake_flow_poll_button_a_action());
        TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_LED));
    }
}

/* The panel must stay quiet while the MQTT tail is transmitting — an
   e-ink refresh current alongside a radio TX burst is the brownout this
   whole rendezvous exists to avoid — and the wake has already synced, so
   there is no window to open either. The repaint rides the post-join
   changed-state re-render instead. */
void test_the_join_poll_neither_paints_nor_syncs(void) {
    flow_a_result = BTN_A_STARTED;
    flow_press(BTN_A);
    mock_time_set(flow_at(14, 0));

    TEST_ASSERT_TRUE(wake_flow_poll_button_a_action());

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_REPAINT));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_ACTION_RENDER));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_NET_OPEN));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_WAIT_NTP));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_SHIFT_EXPIRY));
}

void test_the_join_poll_applies_the_map_at_the_current_clock(void) {
    flow_a_result = BTN_A_STARTED;
    flow_press(BTN_A);
    mock_time_set(flow_at(9, 5) + 41);

    TEST_ASSERT_TRUE(wake_flow_poll_button_a_action());

    TEST_ASSERT_EQUAL_INT64(flow_at(9, 5) + 41, flow_a_apply_arg);
}

/* ---- ROW 18: Button A during a break ----------------------------------- */

/* A is the start/pause button and slot 0 is the break: starting the
   screen timer from inside the break it was sent on defeats the break
   entirely. Refused, logged, and — the part that matters — the state map
   is never even asked, so nothing transitions. */
void test_row18_button_a_during_a_break_is_refused_with_no_side_effects(void) {
    flow_state = TIMER_BREAK;
    flow_a_result = BTN_A_STARTED; /* the map WOULD start it if asked */
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_FALSE(flow_dispatch(BTN_A, flow_at(16, 0), TIMER_BREAK, true));

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_A_APPLY));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_LED));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_NET_OPEN));
    TEST_ASSERT_EQUAL_INT(TIMER_BREAK, flow_state);
    TEST_ASSERT_FALSE(flow_swapped_io);
    TEST_ASSERT_EQUAL_INT64(flow_at(16, 0), flow_now_io); /* clock untouched */
    TEST_ASSERT_EQUAL_UINT32(0, mock_delay_total_ms());   /* no ack hold either */
}

/* The `before`-by-value pin, in both directions. The guard reads the
   PAINTED state, never the live one, and the two genuinely differ: a
   break can end mid-wake (the drain runs before `before` is captured in
   one handler and after it in the other), and a Button C swap moves the
   active slot out from under it. Keying this guard on timer_get_state()
   would refuse a legitimate press and admit the one the break forbids. */
void test_row18_the_refusal_keys_on_the_painted_state_not_the_live_one(void) {
    /* painted BREAK, live RUNNING: still refused */
    flow_state = TIMER_RUNNING;
    flow_a_result = BTN_A_PAUSED;
    mock_time_set(flow_at(16, 0));
    TEST_ASSERT_FALSE(flow_dispatch(BTN_A, flow_at(16, 0), TIMER_BREAK, true));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_A_APPLY));

    /* painted RUNNING, live BREAK: NOT refused — the guard is not a
       disguised read of timer_get_state() */
    setUp();
    flow_state = TIMER_BREAK;
    flow_a_result = BTN_A_PAUSED;
    mock_time_set(flow_at(16, 0));
    TEST_ASSERT_TRUE(flow_dispatch(BTN_A, flow_at(16, 0), TIMER_RUNNING, true));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_A_APPLY));
}

/* The refusal is keyed on BREAK alone. EXPIRED reaches the map and is
   turned down there instead (BTN_A_NONE), which is a different arm with a
   different outcome — no ack hold, no LED, but the map WAS consulted. */
void test_button_a_on_an_expired_slot_is_refused_by_the_map_not_the_guard(void) {
    flow_state = TIMER_EXPIRED;
    flow_a_result = BTN_A_NONE;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_FALSE(flow_dispatch(BTN_A, flow_at(16, 0) - 30, TIMER_EXPIRED, true));

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_A_APPLY));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_LED));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_NET_OPEN));
    TEST_ASSERT_EQUAL_UINT32(0, mock_delay_total_ms());
    TEST_ASSERT_EQUAL_INT64(flow_at(16, 0) - 30, flow_now_io); /* clock untouched */
    TEST_ASSERT_FALSE(flow_swapped_io);
}

/* ---- the start/resume arm ---------------------------------------------- */

/* The map is applied at the CALLER's clock, not at the wall clock: the
   handlers captured `now` before the rollover and the bedtime gate, and
   an allocation computed off a different second than the one the caller
   renders with drifts the expiry by that difference. */
void test_a_start_applies_the_map_at_the_callers_clock(void) {
    flow_a_result = BTN_A_STARTED;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_A, flow_at(16, 0) - 30, TIMER_IDLE, false));

    TEST_ASSERT_EQUAL_INT64(flow_at(16, 0) - 30, flow_a_apply_arg);
}

/* "Hold the pre-press colour briefly so the WHITE/AMBER -> GREEN
   transition is visible as an acknowledgement." The hold is worthless
   after the colour has already changed, so its position is the point,
   not its existence. */
void test_a_start_holds_the_pre_press_colour_before_the_led(void) {
    flow_a_result = BTN_A_STARTED;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_A, flow_at(16, 0), TIMER_IDLE, false));

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_LED));
    TEST_ASSERT_EQUAL_UINT32(250, flow_delay_at_led); /* the hold had already run */
    TEST_ASSERT_EQUAL_UINT32(250, mock_delay_total_ms());
}

/* The ack must be on the pixels before the window blocks for seconds —
   an LED that lights only after the sync settles is not an acknowledgement
   of anything. */
void test_a_start_acks_before_it_opens_the_window(void) {
    flow_a_result = BTN_A_STARTED;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_A, flow_at(16, 0), TIMER_IDLE, true));

    /* Presence first: flow_log_at returns -1 for an effect that never
       happened, and -1 sorts before everything — so an ordering assertion
       on its own passes when the earlier effect is simply missing. */
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_LED));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_NET_OPEN));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_WAIT_NTP));
    TEST_ASSERT_TRUE(flow_log_at(EV_LED) < flow_log_at(EV_NET_OPEN));
    TEST_ASSERT_TRUE(flow_log_at(EV_NET_OPEN) < flow_log_at(EV_WAIT_NTP));
}

/* The reason `now` is a pointer. The window blocks for seconds; the
   caller renders with what comes back, and rendering with the pre-window
   clock puts a stale time on the panel and mis-computes the remaining. */
void test_a_start_hands_the_caller_the_post_window_clock(void) {
    flow_a_result = BTN_A_RESUMED;
    flow_ntp_seconds = 7; /* the sync took seven seconds */
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_A, flow_at(16, 0), TIMER_PAUSED, true));

    TEST_ASSERT_EQUAL_INT64(flow_at(16, 0) + 7, flow_now_io);
}

/* Even with no window at all the clock is re-read, because the ack hold
   above can itself cross a second boundary — and one second is the whole
   resolution the panel renders in. */
void test_a_start_without_a_window_still_re_reads_the_clock(void) {
    flow_a_result = BTN_A_STARTED;
    mock_time_set(flow_at(16, 0));
    hal_delay_ms(750); /* the poll loop that got here has already burned 750 ms */

    TEST_ASSERT_TRUE(flow_dispatch(BTN_A, flow_at(16, 0), TIMER_IDLE, false));

    /* 750 + the 250 ms ack hold = exactly one second */
    TEST_ASSERT_EQUAL_INT64(flow_at(16, 0) + 1, flow_now_io);
}

void test_a_synced_start_shifts_the_expiry_by_the_measured_step(void) {
    flow_a_result = BTN_A_STARTED;
    flow_ntp_ok = true;
    flow_clock_step = 4;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_A, flow_at(16, 0), TIMER_IDLE, true));

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_SHIFT_EXPIRY));
    TEST_ASSERT_EQUAL_INT64(4, flow_shift_arg);
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_NOTE_UNSYNCED));
}

/* A negative step is the interesting direction: the device clock ran
   fast, so the expiry must move EARLIER. A shift that dropped the sign
   (or took an absolute value) would extend the allocation instead. */
void test_a_backwards_clock_step_shifts_the_expiry_backwards(void) {
    flow_a_result = BTN_A_STARTED;
    flow_ntp_ok = true;
    flow_clock_step = -9;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_A, flow_at(16, 0), TIMER_IDLE, true));

    TEST_ASSERT_EQUAL_INT64(-9, flow_shift_arg);
}

/* The step is a TAKE: reading it twice would hand the second reader a
   zero, and the shift is the only consumer on this path. */
void test_the_measured_step_is_taken_exactly_once(void) {
    flow_a_result = BTN_A_STARTED;
    flow_clock_step = 3;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_A, flow_at(16, 0), TIMER_IDLE, true));

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_TAKE_STEP));
}

/* Fail-open: the timer keeps running on the uncorrected clock. But the
   sync may still settle during the MQTT tail, so the wake has to REMEMBER
   that it painted unsynced or the late step is never applied. */
void test_a_start_that_misses_the_sync_notes_it_and_shifts_nothing(void) {
    flow_a_result = BTN_A_STARTED;
    flow_ntp_ok = false;
    flow_clock_step = 11; /* would be applied if the branch were inverted */
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_A, flow_at(16, 0), TIMER_IDLE, true));

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_NOTE_UNSYNCED));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_SHIFT_EXPIRY));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_TAKE_STEP));
    TEST_ASSERT_EQUAL_INT64(-424242, flow_shift_arg); /* never written */
}

/* net_apply_open() failing is fail-open too, but it is a DIFFERENT
   failure: there is no window, so there is nothing to wait for and
   nothing to note. Waiting on a window that never opened would block for
   the full NTP settle timeout on every press made with no WiFi. */
void test_a_start_whose_window_will_not_open_skips_the_sync_entirely(void) {
    flow_a_result = BTN_A_STARTED;
    flow_net_open = false;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_A, flow_at(16, 0), TIMER_IDLE, true));

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_NET_OPEN));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_WAIT_NTP));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_NOTE_UNSYNCED));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_SHIFT_EXPIRY));
}

/* A wake that already ran a window skips the redundant second one: the
   clock is corrected and the buffered HA effects are applied already, and
   a second window would cost another radio session and paint over the
   tail. The gate is checked BEFORE net_apply_open, so nothing is opened. */
void test_a_start_on_an_already_synced_wake_opens_no_second_window(void) {
    flow_a_result = BTN_A_STARTED;
    flow_net_open = true; /* it WOULD open if asked */
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_A, flow_at(16, 0), TIMER_IDLE, false));

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_NET_OPEN));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_WAIT_NTP));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_LED)); /* still acked */
}

/* STARTED and RESUMED share one arm, so both must get the whole
   sequence — an arm that fell through for only one of them would leave a
   resume unsynced and unacknowledged. */
void test_a_resume_takes_exactly_the_same_path_as_a_start(void) {
    static const btn_a_action_t both[] = {BTN_A_STARTED, BTN_A_RESUMED};
    for (int i = 0; i < 2; i++) {
        setUp();
        flow_a_result = both[i];
        flow_ntp_seconds = 2;
        mock_time_set(flow_at(16, 0));

        TEST_ASSERT_TRUE(flow_dispatch(BTN_A, flow_at(16, 0), TIMER_IDLE, true));
        TEST_ASSERT_EQUAL_UINT32(250, flow_delay_at_led);
        TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_LED));
        TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_WAIT_NTP));
        TEST_ASSERT_EQUAL_INT64(flow_at(16, 0) + 2, flow_now_io);
    }
}

/* The pause arm is deliberately bare: nothing to acknowledge visually
   (the caller renders PAUSED immediately), nothing to sync for, and no
   reason to burn 250 ms of awake time on a battery-powered device. */
void test_a_pause_neither_holds_nor_syncs_nor_moves_the_clock(void) {
    flow_state = TIMER_RUNNING;
    flow_a_result = BTN_A_PAUSED;
    mock_time_set(flow_at(16, 0) + 55);

    TEST_ASSERT_TRUE(flow_dispatch(BTN_A, flow_at(16, 0), TIMER_RUNNING, true));

    TEST_ASSERT_EQUAL_UINT32(0, mock_delay_total_ms());
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_LED));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_NET_OPEN));
    TEST_ASSERT_EQUAL_INT64(flow_at(16, 0), flow_now_io); /* caller's clock kept */
    TEST_ASSERT_FALSE(flow_swapped_io);
}

/* ---- ROW 19: Button B ---------------------------------------------------

   B resets the selected timer to full. Reloadable extras allow it
   outright; anything else needs the ParentTesting build flag — and
   nothing allows it while the slot is RUNNING, because a reset that
   refunds a running allocation is the trivial way around the whole
   screen-time budget. B is dropped from the wake mask while RUNNING, so
   this guard exists for presses that ride in on ANOTHER wake's latch. */

void test_row19_button_b_while_running_without_parent_testing_is_refused(void) {
    flow_state = TIMER_RUNNING;
    flow_slot_reloadable = false;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_FALSE(flow_dispatch(BTN_B, flow_at(16, 0), TIMER_RUNNING, true));

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_RELOAD_ALLOWED));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_RELOAD)); /* never attempted */
    TEST_ASSERT_EQUAL_INT(TIMER_RUNNING, flow_state);
    TEST_ASSERT_FALSE(flow_swapped_io);
    TEST_ASSERT_EQUAL_INT64(flow_at(16, 0), flow_now_io);
}

/* Even a reloadable extra is refused while it runs — the gate's RUNNING
   arm outranks its reloadable arm, and this dispatch honours whatever the
   gate says rather than second-guessing it. */
void test_button_b_is_never_allowed_while_the_slot_is_running(void) {
    flow_state = TIMER_RUNNING;
    flow_slot_reloadable = true;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_FALSE(flow_dispatch(BTN_B, flow_at(16, 0), TIMER_RUNNING, true));

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_RELOAD));
}

/* The gate is asked with the BUILD FLAG, not with a literal: hard-coding
   true here would hand every device the parent escape, and hard-coding
   false would break the parent build's reset of a non-reloadable slot.
   PARENT_TESTING is visible in this TU (wake_flow.c defines it), so the
   assertion tracks whichever way the build is configured. */
void test_row19_the_reload_gate_is_asked_with_the_build_flag(void) {
    flow_state = TIMER_IDLE;
    flow_slot_reloadable = false;
    mock_time_set(flow_at(16, 0));

    (void)flow_dispatch(BTN_B, flow_at(16, 0), TIMER_IDLE, true);

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_RELOAD_ALLOWED));
    TEST_ASSERT_EQUAL_INT(PARENT_TESTING ? 1 : 0, flow_reload_parent_arg);
}

void test_button_b_resets_a_reloadable_slot_when_the_rule_allows_it(void) {
    flow_state = TIMER_PAUSED;
    flow_slot_reloadable = true;
    flow_reload_ok = true;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_B, flow_at(16, 0), TIMER_PAUSED, true));

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_RELOAD));
    TEST_ASSERT_EQUAL_INT(TIMER_IDLE, flow_state);
    TEST_ASSERT_FALSE(flow_swapped_io); /* a reset is not a selection change */
}

/* Two independent refusals, and the second one is not redundant: the gate
   answers for the SELECTED def while the reload answers for the slot, and
   they are re-read at different moments. A dispatch that reported success
   off the gate alone would render a "reset" that never happened. */
void test_button_b_is_refused_when_the_reload_itself_declines(void) {
    flow_state = TIMER_PAUSED;
    flow_slot_reloadable = true;
    flow_reload_ok = false;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_FALSE(flow_dispatch(BTN_B, flow_at(16, 0), TIMER_PAUSED, true));

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_RELOAD));
}

/* No LED and no window on this arm: the caller lights the resulting state
   once, and a reset needs no clock correction. */
void test_button_b_lights_nothing_and_opens_nothing_of_its_own(void) {
    flow_state = TIMER_IDLE;
    flow_slot_reloadable = true;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_B, flow_at(16, 0), TIMER_IDLE, true));

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_LED));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_NET_OPEN));
    TEST_ASSERT_EQUAL_UINT32(0, mock_delay_total_ms());
}

/* ---- ROW 20 and ROW 4: Button C ---------------------------------------- */

/* A swap is reported through the out-param, never by rewriting `before`.
   Landing on a slot that is ALREADY expired must not re-fire its alert —
   the kid did not just run out of time, they walked over to a timer that
   ran out earlier — and that suppression is exactly what the flag buys. */
void test_row20_a_swap_onto_an_expired_slot_reports_the_change(void) {
    flow_state = TIMER_PAUSED;
    flow_select_ok = true;
    flow_state_at_select = TIMER_EXPIRED;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_C, flow_at(16, 0), TIMER_PAUSED, true));

    TEST_ASSERT_TRUE(flow_swapped_io);
    TEST_ASSERT_EQUAL_INT(TIMER_EXPIRED, flow_state);
    TEST_ASSERT_EQUAL_INT(FLOW_PIANO, flow_active_slot);
}

/* Row 20 end to end: the dispatch's own outputs, fed to the shipping
   render policy. The negative control is the same call with the flag
   dropped, which alerts — so the suppression is carried by what the
   dispatch reported and not by the state pair. */
void test_row20_the_reported_swap_suppresses_the_expiry_alert(void) {
    flow_state = TIMER_PAUSED;
    flow_state_at_select = TIMER_EXPIRED;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_C, flow_at(16, 0), TIMER_PAUSED, true));
    TEST_ASSERT_TRUE(flow_swapped_io);

    wake_render_t wr =
        wake_policy_render(TIMER_PAUSED, timer_get_state(), true, wake_flow_break_ended_this_wake(), flow_swapped_io);
    TEST_ASSERT_NOT_EQUAL_INT(WAKE_RENDER_EXPIRY_ALERT, wr);

    /* negative control: without the report the same landing alerts */
    TEST_ASSERT_EQUAL_INT(WAKE_RENDER_EXPIRY_ALERT,
                          wake_policy_render(TIMER_PAUSED, timer_get_state(), true, false, false));
}

/* ROW 4, end to end, and the gap cycle 7 could not close: the break
   screen is a full-screen INVERSION of the main layout, and display.c
   promotes only every fifth partial, so four presses in five ghosted the
   whole panel. `before` is by value precisely so it still names the
   inverted layout after the swap has moved the active slot; the dispatch
   is driven for real here and ITS outputs go to the real render policy. */
void test_row4_a_swap_during_a_break_renders_full_end_to_end(void) {
    flow_state = TIMER_BREAK; /* the break screen is what is on the glass */
    flow_state_at_select = TIMER_IDLE;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_C, flow_at(16, 0), TIMER_BREAK, true));

    TEST_ASSERT_TRUE(flow_swapped_io);
    TEST_ASSERT_EQUAL_INT(TIMER_IDLE, timer_get_state()); /* the slot moved... */
    TEST_ASSERT_EQUAL_INT(WAKE_RENDER_FULL,
                          wake_policy_render(TIMER_BREAK, timer_get_state(), true, false, flow_swapped_io));

    /* The shipped defect as the negative control: had the dispatch
       rewritten `before` to the post-swap state, the boundary would be
       invisible and this same render goes partial — the ghost. */
    TEST_ASSERT_EQUAL_INT(WAKE_RENDER_PARTIAL,
                          wake_policy_render(timer_get_state(), timer_get_state(), true, false, flow_swapped_io));
}

/* The other direction across the same inversion: off an extra and back
   onto the break screen. Both crossings ghost, so both are pinned. */
void test_row4_a_swap_back_onto_the_break_screen_renders_full(void) {
    flow_state = TIMER_PAUSED;
    flow_select_slot = FLOW_SCREEN;
    flow_state_at_select = TIMER_BREAK;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_C, flow_at(16, 0), TIMER_PAUSED, true));

    TEST_ASSERT_EQUAL_INT(TIMER_BREAK, timer_get_state());
    TEST_ASSERT_EQUAL_INT(WAKE_RENDER_FULL,
                          wake_policy_render(TIMER_PAUSED, timer_get_state(), true, false, flow_swapped_io));
}

/* Rows 4 and 20 at once, which is where they actually collide: a swap
   from the break screen onto an already-EXPIRED slot must suppress the
   alert AND still be a full refresh. Suppressed must never mean partial. */
void test_row4_a_swap_from_the_break_onto_an_expired_slot_is_full_not_an_alert(void) {
    flow_state = TIMER_BREAK;
    flow_state_at_select = TIMER_EXPIRED;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_C, flow_at(16, 0), TIMER_BREAK, true));

    wake_render_t wr = wake_policy_render(TIMER_BREAK, timer_get_state(), true, false, flow_swapped_io);
    TEST_ASSERT_EQUAL_INT(WAKE_RENDER_FULL, wr);
}

/* Refused only while RUNNING (pause first). A refusal must report NO
   selection change, or the caller would suppress an expiry alert that
   nothing swapped away from. */
void test_a_refused_swap_reports_no_selection_change(void) {
    flow_state = TIMER_RUNNING;
    flow_select_ok = false;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_FALSE(flow_dispatch(BTN_C, flow_at(16, 0), TIMER_RUNNING, true));

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_SELECT_NEXT));
    TEST_ASSERT_FALSE(flow_swapped_io);
    TEST_ASSERT_EQUAL_INT(FLOW_SCREEN, flow_active_slot); /* selection untouched */
}

/* A Screen Break deliberately does NOT refuse a swap — going and running
   Piano is what the break time is for (and is the whole reason the break
   tail polls buttons at all). */
void test_a_swap_during_a_break_is_allowed(void) {
    flow_state = TIMER_BREAK;
    flow_select_ok = true;
    flow_state_at_select = TIMER_IDLE;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_C, flow_at(16, 0), TIMER_BREAK, true));

    TEST_ASSERT_EQUAL_INT(FLOW_PIANO, flow_active_slot);
}

void test_a_swap_lights_nothing_opens_nothing_and_leaves_the_clock_alone(void) {
    flow_state = TIMER_IDLE;
    mock_time_set(flow_at(16, 0) + 40);

    TEST_ASSERT_TRUE(flow_dispatch(BTN_C, flow_at(16, 0), TIMER_IDLE, true));

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_LED));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_NET_OPEN));
    TEST_ASSERT_EQUAL_UINT32(0, mock_delay_total_ms());
    TEST_ASSERT_EQUAL_INT64(flow_at(16, 0), flow_now_io);
}

/* ---- the out-param contract -------------------------------------------- */

/* Written on EVERY arm, before anything is decided. The callers pass the
   address of a local that the render then reads, and a path that left it
   untouched would carry the previous press's answer into this one —
   suppressing an expiry alert nobody swapped for. */
void test_every_arm_clears_the_selection_report_before_deciding(void) {
    static const button_id_t all[] = {BTN_A, BTN_B, BTN_C, BTN_D, BTN_NONE};
    for (int i = 0; i < (int)(sizeof(all) / sizeof(all[0])); i++) {
        setUp();
        /* every arm arranged to REFUSE, so nothing legitimately sets it */
        flow_state = TIMER_RUNNING;
        flow_a_result = BTN_A_NONE;
        flow_slot_reloadable = false;
        flow_select_ok = false;
        mock_time_set(flow_at(16, 0));

        TEST_ASSERT_FALSE(flow_dispatch(all[i], flow_at(16, 0), TIMER_BREAK, true));
        TEST_ASSERT_FALSE_MESSAGE(flow_swapped_io, "an arm left the selection report unwritten");
    }
}

void test_only_a_successful_swap_reports_a_selection_change(void) {
    /* the accepting non-C arms */
    flow_a_result = BTN_A_STARTED;
    mock_time_set(flow_at(16, 0));
    TEST_ASSERT_TRUE(flow_dispatch(BTN_A, flow_at(16, 0), TIMER_IDLE, false));
    TEST_ASSERT_FALSE(flow_swapped_io);

    setUp();
    flow_slot_reloadable = true;
    mock_time_set(flow_at(16, 0));
    TEST_ASSERT_TRUE(flow_dispatch(BTN_B, flow_at(16, 0), TIMER_IDLE, false));
    TEST_ASSERT_FALSE(flow_swapped_io);

    setUp();
    mock_time_set(flow_at(16, 0));
    TEST_ASSERT_TRUE(flow_dispatch(BTN_C, flow_at(16, 0), TIMER_IDLE, false));
    TEST_ASSERT_TRUE(flow_swapped_io);
}

/* D is the user-facing "refresh everything" button and is handled by the
   wake decode, never here; BTN_NONE reaches this on a spurious EXT1 wake.
   Both must be completely inert — a fall-through into the A arm would
   start a timer on a sync press. */
void test_button_d_and_button_none_are_inert_in_the_dispatch(void) {
    static const button_id_t inert[] = {BTN_D, BTN_NONE};
    for (int i = 0; i < 2; i++) {
        setUp();
        flow_a_result = BTN_A_STARTED;
        flow_slot_reloadable = true;
        flow_select_ok = true;
        mock_time_set(flow_at(16, 0));

        TEST_ASSERT_FALSE(flow_dispatch(inert[i], flow_at(16, 0) - 30, TIMER_IDLE, true));

        TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_A_APPLY));
        TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_RELOAD_ALLOWED));
        TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_SELECT_NEXT));
        TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_LED));
        TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_NET_OPEN));
        TEST_ASSERT_EQUAL_INT(TIMER_IDLE, flow_state);
        TEST_ASSERT_EQUAL_INT64(flow_at(16, 0) - 30, flow_now_io);
        TEST_ASSERT_FALSE(flow_swapped_io);
    }
}

/* ---- the break tail poll ------------------------------------------------ */

void test_the_break_tail_poll_is_inert_when_nothing_is_latched(void) {
    flow_state = TIMER_BREAK;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_FALSE(wake_flow_poll_break_buttons());

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_A_APPLY));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_LED));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_ACTION_RENDER));
}

/* Unlike the two single-button polls this one takes the WHOLE latch: it
   is the only consumer left before sleep, so anything it leaves behind is
   discarded anyway. Pinned because the mask is the visible difference
   between this poll and the row-5 pair, and copying their masked take
   here would strand a B or C press for a drain that never comes. */
void test_the_break_tail_poll_drains_the_whole_latch_including_d(void) {
    flow_state = TIMER_BREAK; /* A is refused here, so the poll reports false */
    flow_press(BTN_A);
    flow_press(BTN_B);
    flow_press(BTN_C);
    flow_press(BTN_D);
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_FALSE(wake_flow_poll_break_buttons());

    TEST_ASSERT_EQUAL_HEX8(0, flow_latch_residue());
}

/* D's ABSENCE from the allowed mask is the one membership fact in this
   poll that cannot be pinned, and deliberately so: adding (1u << BTN_D)
   back changes nothing observable. D only ever wins the pick when it is
   latched alone, and it then reaches the dispatch's outer default arm and
   is refused there instead — same false return, same empty effect log,
   same drained latch. The assertions below hold either way. What makes
   that safe is the dispatch having no BTN_D arm, which IS pinned
   (test_button_d_and_button_none_are_inert_in_the_dispatch); if D ever
   gains one, this mask becomes load bearing and needs a case of its own. */
void test_a_lone_d_press_in_the_break_tail_is_consumed_and_ignored(void) {
    flow_state = TIMER_BREAK;
    flow_press(BTN_D);
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_FALSE(wake_flow_poll_break_buttons());

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_A_APPLY));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_ACTION_RENDER));
    TEST_ASSERT_EQUAL_HEX8(0, flow_latch_residue());
}

/* MEMBERSHIP, which is a different property from priority. The priority
   case below presses B alongside C and pins that C wins — which says
   nothing about whether B is in the allowed mask at all. Dropping
   (1u << BTN_B) from it leaves every other case in this suite passing
   while silently disabling Button B for the entire break tail: the kid
   walks to another timer during the break and cannot reset it.
   A and C are each already the winning pick in a case of their own; this
   is B's, and it is the only reason the mask's B bit is pinned. */
void test_the_break_tail_acts_on_a_lone_button_b_press(void) {
    flow_state = TIMER_PAUSED;
    flow_slot_reloadable = true;
    flow_reload_ok = true;
    flow_press(BTN_B);
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(wake_flow_poll_break_buttons());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_RELOAD));
    TEST_ASSERT_EQUAL_INT(TIMER_IDLE, flow_state); /* the slot really was reset */
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_LED));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_ACTION_RENDER));
    TEST_ASSERT_EQUAL_INT(BTN_B, flow_render_btn);
    TEST_ASSERT_EQUAL_INT(TIMER_PAUSED, flow_render_before); /* the painted layout */
    TEST_ASSERT_FALSE(flow_render_selection_changed);        /* a reset is not a swap */
    TEST_ASSERT_EQUAL_HEX8(0, flow_latch_residue());
}

/* One press per poll, by the latch's own A > C > B > D priority — the
   real button_latch_pick is compiled in, so this is the shipping order.
   C over B matters here: during a break, C is how you walk to another
   timer and B would reset the one you are leaving. */
void test_the_break_tail_acts_on_the_highest_priority_latched_press(void) {
    flow_state = TIMER_PAUSED;
    flow_slot_reloadable = true; /* B would succeed if it were picked */
    flow_state_at_select = TIMER_IDLE;
    flow_press(BTN_B);
    flow_press(BTN_C);
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(wake_flow_poll_break_buttons());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_SELECT_NEXT));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_RELOAD));
    TEST_ASSERT_EQUAL_INT(BTN_C, flow_render_btn);
}

/* The symptom this poll was added for: "I couldn't move to another timer
   in the final minute of the screen break." C selects, and the panel is
   repainted on the spot rather than at the next wake. */
void test_the_break_tail_lets_button_c_move_to_another_timer(void) {
    flow_state = TIMER_BREAK;
    flow_state_at_select = TIMER_IDLE;
    flow_press(BTN_C);
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(wake_flow_poll_break_buttons());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_ACTION_RENDER));
    TEST_ASSERT_EQUAL_INT(BTN_C, flow_render_btn);
    TEST_ASSERT_EQUAL_INT(TIMER_BREAK, flow_render_before); /* the painted layout */
    TEST_ASSERT_TRUE(flow_render_selection_changed);
    TEST_ASSERT_EQUAL_INT(FLOW_PIANO, flow_active_slot);
}

/* The window for this wake has already been joined by the time the watch
   runs; a second one here would block the tail for seconds and paint over
   it. This is the allow_net_window=false argument, pinned at the caller. */
void test_the_break_tail_never_opens_a_second_network_window(void) {
    /* the break runs on slot 0 behind a SELECTED extra, so the active
       slot can legitimately be IDLE and A can legitimately start it */
    flow_state = TIMER_IDLE;
    flow_a_result = BTN_A_STARTED;
    flow_net_open = true;
    flow_press(BTN_A);
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(wake_flow_poll_break_buttons());

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_NET_OPEN));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_WAIT_NTP));
}

/* The render is handed the clock the DISPATCH left behind, not the one
   the poll walked in with — the 250 ms acknowledgement hold alone can
   cross a second boundary, and one second is the panel's whole
   resolution. Set up so it crosses exactly. */
void test_the_break_tail_hands_the_render_the_clock_the_action_left(void) {
    flow_state = TIMER_IDLE;
    flow_a_result = BTN_A_STARTED;
    flow_press(BTN_A);
    mock_time_set(flow_at(16, 0));
    hal_delay_ms(750); /* the tail's 250 ms poll loop has already run three times */

    TEST_ASSERT_TRUE(wake_flow_poll_break_buttons());

    TEST_ASSERT_EQUAL_INT64(flow_at(16, 0) + 1, flow_render_now);
}

/* A press the guards refuse changed nothing, so it must not light the
   pixels (which would read as "heard, and done") and must not repaint —
   a full refresh mid-break is seconds of e-ink and a wasted wake. The
   caller also keeps watching, which is what the false return buys. */
void test_a_refused_press_in_the_break_tail_neither_lights_nor_renders(void) {
    flow_state = TIMER_BREAK;
    flow_a_result = BTN_A_STARTED;
    flow_press(BTN_A); /* A during a break: refused by row 18 */
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_FALSE(wake_flow_poll_break_buttons());

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_LED));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_ACTION_RENDER));
    TEST_ASSERT_EQUAL_INT(-1, flow_render_now); /* the seam was never reached */
}

/* The pixel is the instant feedback; the e-ink refresh behind it takes
   seconds. Lighting it after the render would make every break-tail press
   feel dead for the length of a full refresh. */
void test_the_break_tail_lights_the_pixels_before_it_renders(void) {
    flow_state = TIMER_PAUSED;
    flow_state_at_select = TIMER_IDLE;
    flow_press(BTN_C);
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(wake_flow_poll_break_buttons());

    /* Presence first, for the same reason as above — and here it is load
       bearing: a Button C dispatch lights no pixel of its own, so if this
       poll's own call were deleted the log would hold a render and no
       LED, and a bare ordering comparison would pass on the -1. */
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_LED));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_ACTION_RENDER));
    TEST_ASSERT_TRUE(flow_log_at(EV_LED) < flow_log_at(EV_ACTION_RENDER));
}

/* `before` is captured BEFORE the dispatch, so the render still learns
   which layout was on the glass. Capturing it afterwards collapses
   before/after to the same state and every break-tail press goes partial
   across the inversion — regression 1520486 by another route. */
void test_the_break_tail_reports_the_state_that_was_painted_not_the_new_one(void) {
    flow_state = TIMER_IDLE;
    flow_a_result = BTN_A_STARTED;
    flow_press(BTN_A);
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(wake_flow_poll_break_buttons());

    TEST_ASSERT_EQUAL_INT(TIMER_IDLE, flow_render_before);   /* what was painted */
    TEST_ASSERT_EQUAL_INT(TIMER_RUNNING, timer_get_state()); /* what is live now */
    TEST_ASSERT_FALSE(flow_render_selection_changed);
}

/* ---- ROW 22: the break gate's config -----------------------------------
   "Break interval configured 0" => no break ever starts. The value is a
   real HA knob, so this is the documented way to switch eye rest off. */

void test_row22_a_zero_interval_never_starts_a_break(void) {
    flow_interval_min = 0;
    flow_break_due_ret = true; /* the balance is over: irrelevant */
    TEST_ASSERT_EQUAL_INT(FLOW_GATE_NO_BREAK, flow_run_break_gate(flow_at(14, 0)));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_START_BREAK));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_BREAK_PAINT));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_ALERT_BREAK));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PERSIST_SAVE));
}

/* The zero check has to come FIRST: with breaks switched off the balance
   must not even be asked, or a stale accrual could still trip a mutant
   that reordered the two. */
void test_row22_a_zero_interval_is_decided_before_the_balance_is_asked(void) {
    flow_interval_min = 0;
    flow_break_due_ret = true;
    (void)flow_run_break_gate(flow_at(14, 0));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_BREAK_DUE));
}

/* Both numbers are read before anything is decided — the duration is
   needed by the bed-time check further down, which runs before the
   start. */
void test_the_gate_reads_both_config_values_up_front(void) {
    flow_interval_min = 0;
    (void)flow_run_break_gate(flow_at(14, 0));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_CFG_INTERVAL));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_CFG_DURATION));
}

/* A getter that declines to write leaves the caller's pre-seed standing,
   and the pre-seed is the compile-time default. Without this the two
   initialisers could be deleted unnoticed and an unconfigured device
   would run with an interval of zero — i.e. no eye rest at all. */
void test_the_compile_time_defaults_stand_when_nvs_never_stored_them(void) {
    flow_cfg_writes = false;
    flow_break_due_ret = true;
    TEST_ASSERT_EQUAL_INT(FLOW_GATE_STARTED, flow_run_break_gate(flow_at(14, 0)));
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_BREAK_INTERVAL_MIN, flow_seen_interval_default);
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_BREAK_DURATION_MIN, flow_seen_duration_default);
    TEST_ASSERT_EQUAL_INT32((int32_t)NVS_DEFAULT_BREAK_INTERVAL_MIN * 60, flow_break_due_interval);
    TEST_ASSERT_EQUAL_INT32((int32_t)NVS_DEFAULT_BREAK_DURATION_MIN * 60, flow_start_break_dur);
}

/* ZERO is the disable, and nothing else is. A guard widened by one (`<= 1`)
   keeps the row-22 case passing while silently switching eye rest off for
   every device configured with a one-minute interval — which is what
   ParentTesting demos actually use. Mutation testing is what surfaced it:
   `== 0` -> `<= 1` escaped a suite that only ever tried 0 and 30. */
void test_row22_a_one_minute_interval_is_an_interval_not_a_disable(void) {
    flow_interval_min = 1;
    flow_break_due_ret = true;
    TEST_ASSERT_EQUAL_INT(FLOW_GATE_STARTED, flow_run_break_gate(flow_at(14, 0)));
    TEST_ASSERT_EQUAL_INT32(60, flow_break_due_interval);
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_START_BREAK));
}

/* The other side of the same guard: a one-minute break DURATION is also a
   real duration. */
void test_a_one_minute_duration_is_a_real_duration(void) {
    flow_duration_min = 1;
    flow_break_due_ret = true;
    TEST_ASSERT_EQUAL_INT(FLOW_GATE_STARTED, flow_run_break_gate(flow_at(14, 0)));
    TEST_ASSERT_EQUAL_INT32(60, flow_start_break_dur);
}

/* ---- the break gate: the balance ---------------------------------------- */

void test_a_balance_short_of_the_interval_starts_nothing(void) {
    flow_break_due_ret = false;
    TEST_ASSERT_EQUAL_INT(FLOW_GATE_NO_BREAK, flow_run_break_gate(flow_at(14, 0)));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_BREAK_DUE));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_START_BREAK));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PERSIST_SAVE));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_BREAK_PAINT));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_ALERT_BREAK));
}

/* Both config values are MINUTES and both consumers want SECONDS. A
   missing *60 on either side is a 60x error in the wrong direction. */
void test_the_interval_is_asked_of_the_balance_in_seconds(void) {
    flow_interval_min = 45;
    flow_break_due_ret = false;
    (void)flow_run_break_gate(flow_at(14, 0));
    TEST_ASSERT_EQUAL_INT32(45 * 60, flow_break_due_interval);
}

void test_the_break_is_started_for_the_configured_duration_in_seconds(void) {
    flow_duration_min = 12;
    flow_break_due_ret = true;
    TEST_ASSERT_EQUAL_INT(FLOW_GATE_STARTED, flow_run_break_gate(flow_at(14, 0)));
    TEST_ASSERT_EQUAL_INT32(12 * 60, flow_start_break_dur);
}

/* The gate takes `now` rather than reading the clock, because its callers
   do not agree on which instant they mean — two of them re-read the wall
   clock after an alert has held the CPU for ~15 s. The live clock is
   parked somewhere else here so a re-read would be visible. */
void test_the_gate_uses_the_clock_it_was_handed_not_the_live_one(void) {
    mock_time_set(flow_at(9, 0));
    flow_break_due_ret = true;
    TEST_ASSERT_EQUAL_INT(FLOW_GATE_STARTED, flow_run_break_gate(flow_at(14, 30)));
    TEST_ASSERT_EQUAL_INT64(flow_at(14, 30), flow_break_due_now);
    TEST_ASSERT_EQUAL_INT64(flow_at(14, 30), flow_start_break_now);
    TEST_ASSERT_EQUAL_INT64(flow_at(14, 30), flow_break_paint_now);
}

/* ---- the break gate: the start sequence --------------------------------- */

/* Persist BEFORE the ~15 s alarm, same rationale as the EXPIRED
   at-transition save: a power cut during the alarm must not restore a
   snapshot taken before the break existed. And the paint must precede the
   alarm, or the alarm pulses over the previous layout. */
void test_a_started_break_persists_then_paints_then_alarms(void) {
    flow_break_due_ret = true;
    TEST_ASSERT_EQUAL_INT(FLOW_GATE_STARTED, flow_run_break_gate(flow_at(14, 0)));
    int started = flow_log_at(EV_START_BREAK);
    int saved = flow_log_at(EV_PERSIST_SAVE);
    int painted = flow_log_at(EV_BREAK_PAINT);
    int alarmed = flow_log_at(EV_ALERT_BREAK);
    TEST_ASSERT_TRUE(started >= 0);
    TEST_ASSERT_TRUE(started < saved);
    TEST_ASSERT_TRUE(saved < painted);
    TEST_ASSERT_TRUE(painted < alarmed);
}

void test_a_started_break_runs_the_break_alarm_not_the_expiry_one(void) {
    flow_break_due_ret = true;
    (void)flow_run_break_gate(flow_at(14, 0));
    TEST_ASSERT_EQUAL_INT(ALERT_BREAK, flow_last_alert);
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_ALERT_EXPIRY));
}

/* The full ordered trace, so an effect that moved rather than vanished is
   caught too. */
void test_the_started_break_effect_order_is_pinned_end_to_end(void) {
    flow_break_due_ret = true;
    (void)flow_run_break_gate(flow_at(14, 0));
    static const flow_event_t expect[] = {EV_CFG_INTERVAL, EV_CFG_DURATION, EV_BREAK_DUE,  EV_START_BREAK,
                                          EV_PERSIST_SAVE, EV_BREAK_PAINT,  EV_ALERT_BREAK};
    TEST_ASSERT_EQUAL_INT((int)(sizeof expect / sizeof expect[0]), flow_log_n);
    for (int i = 0; i < flow_log_n; i++) {
        TEST_ASSERT_EQUAL_INT((int)expect[i], (int)flow_log[i]);
    }
}

/* ---- ROW 21: a break that would cross bed time -------------------------
   "Break due but it would cross bedtime" => break skipped; bedtime
   engages audibly. The engage does not return, so the break start below
   it is unreachable — which is exactly what a returning stub would
   hide. */

void test_row21_a_break_that_would_cross_bed_time_goes_to_bed_instead(void) {
    flow_break_due_ret = true;
    flow_duration_min = 15;
    flow_bed_min = flow_bed_at(20, 0);
    TEST_ASSERT_EQUAL_INT(FLOW_GATE_BEDTIME, flow_run_break_gate(flow_at(19, 50)));
    TEST_ASSERT_TRUE(flow_bed_engaged);
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_START_BREAK));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_BREAK_PAINT));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_ALERT_BREAK));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PERSIST_SAVE));
}

/* Audibly, unconditionally: this is the one alerting path that begins
   before its own threshold is reached, so the child gets a warning
   instead of the screen simply going dark. A `false` here is silent
   bedtime — the exact bug the row exists to prevent. */
void test_row21_the_bed_time_engage_is_always_audible(void) {
    flow_break_due_ret = true;
    flow_bed_min = flow_bed_at(20, 0);
    (void)flow_run_break_gate(flow_at(19, 50));
    TEST_ASSERT_TRUE(flow_bed_engage_alert);
}

void test_row21_the_bed_time_engage_gets_the_gates_own_clock(void) {
    mock_time_set(flow_at(9, 0));
    flow_break_due_ret = true;
    flow_bed_min = flow_bed_at(20, 0);
    (void)flow_run_break_gate(flow_at(19, 50));
    TEST_ASSERT_EQUAL_INT64(flow_at(19, 50), flow_bed_engage_now);
}

/* The crossing is decided on the DURATION, not on a fixed margin: the
   same instant with a shorter break is fine. */
void test_a_break_that_finishes_before_bed_time_still_starts(void) {
    flow_break_due_ret = true;
    flow_duration_min = 9;
    flow_bed_min = flow_bed_at(20, 0);
    TEST_ASSERT_EQUAL_INT(FLOW_GATE_STARTED, flow_run_break_gate(flow_at(19, 50)));
    TEST_ASSERT_FALSE(flow_bed_engaged);
}

/* The equality boundary, one minute the other side of the case above:
   landing EXACTLY on bed time counts as crossing. */
void test_a_break_landing_exactly_on_bed_time_is_skipped(void) {
    flow_break_due_ret = true;
    flow_duration_min = 10;
    flow_bed_min = flow_bed_at(20, 0);
    TEST_ASSERT_EQUAL_INT(FLOW_GATE_BEDTIME, flow_run_break_gate(flow_at(19, 50)));
}

void test_bed_time_disabled_never_skips_a_break(void) {
    flow_break_due_ret = true;
    flow_duration_min = 600; /* absurdly long: only the disable can save it */
    flow_bed_min = -1;
    TEST_ASSERT_EQUAL_INT(FLOW_GATE_STARTED, flow_run_break_gate(flow_at(19, 50)));
    TEST_ASSERT_FALSE(flow_bed_engaged);
}

/* Already past bed time: the crossing test is false (you cannot cross a
   line you are behind), so this gate lets the break through. Reaching
   here at all means lock_gate_check_bedtime did not run first, which no
   wake path does — pinned so the gate's own answer is unambiguous. */
void test_a_break_starting_after_bed_time_is_not_this_gates_problem(void) {
    flow_break_due_ret = true;
    flow_bed_min = flow_bed_at(20, 0);
    TEST_ASSERT_EQUAL_INT(FLOW_GATE_STARTED, flow_run_break_gate(flow_at(21, 0)));
    TEST_ASSERT_FALSE(flow_bed_engaged);
}

/* The bed-time check runs only once the balance has actually crossed:
   a break that is not due cannot send the device to bed early. */
void test_a_break_that_is_not_due_never_reaches_the_bed_time_check(void) {
    flow_break_due_ret = false;
    flow_bed_min = flow_bed_at(20, 0);
    TEST_ASSERT_EQUAL_INT(FLOW_GATE_NO_BREAK, flow_run_break_gate(flow_at(19, 50)));
    TEST_ASSERT_FALSE(flow_bed_engaged);
}

/* ---- ROW 12: power cut mid-expiry-alert --------------------------------
   "snapshot already persisted as EXPIRED". The alert holds the CPU for
   ~15 s; an EN reset or a pulled cable during it must not restore the
   stale RUNNING snapshot and replay the final minute. */

void test_row12_the_expired_snapshot_is_persisted_before_anything_else(void) {
    wake_flow_fire_expiry_alert();
    TEST_ASSERT_EQUAL_INT(0, flow_log_at(EV_PERSIST_SAVE));
}

void test_row12_the_snapshot_is_persisted_before_the_alarm_runs(void) {
    wake_flow_fire_expiry_alert();
    TEST_ASSERT_TRUE(flow_log_at(EV_PERSIST_SAVE) < flow_log_at(EV_ALERT_EXPIRY));
    TEST_ASSERT_TRUE(flow_log_at(EV_PERSIST_SAVE) < flow_log_at(EV_TIMESUP));
}

/* The big TIME'S UP screen goes up BEFORE the alarm, or the beeps and the
   red pulse arrive against the old layout. */
void test_the_timesup_screen_precedes_the_alarm(void) {
    wake_flow_fire_expiry_alert();
    TEST_ASSERT_TRUE(flow_log_at(EV_TIMESUP) < flow_log_at(EV_ALERT_EXPIRY));
}

/* And the main layout comes back after it — the big screen would only
   last until the next tick redraw anyway. */
void test_the_main_layout_is_repainted_after_the_alarm(void) {
    wake_flow_fire_expiry_alert();
    TEST_ASSERT_TRUE(flow_log_at(EV_ALERT_EXPIRY) < flow_log_at(EV_REPAINT));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_REPAINT));
}

void test_the_expiry_alert_runs_the_expiry_alarm_not_the_break_one(void) {
    wake_flow_fire_expiry_alert();
    TEST_ASSERT_EQUAL_INT(ALERT_EXPIRY, flow_last_alert);
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_ALERT_BREAK));
}

void test_the_expiry_alert_effect_order_is_pinned_end_to_end(void) {
    wake_flow_fire_expiry_alert();
    static const flow_event_t expect[] = {EV_PERSIST_SAVE, EV_TIMESUP, EV_ALERT_EXPIRY, EV_REPAINT};
    TEST_ASSERT_EQUAL_INT((int)(sizeof expect / sizeof expect[0]), flow_log_n);
    for (int i = 0; i < flow_log_n; i++) {
        TEST_ASSERT_EQUAL_INT((int)expect[i], (int)flow_log[i]);
    }
}

/* The alert RETURNS, unlike the bed-time engage: the tick handler
   re-checks the break gate straight after it, which is the only place
   expiry-then-break is ordered correctly within one wake. */
void test_the_expiry_alert_returns_to_its_caller(void) {
    wake_flow_fire_expiry_alert();
    flow_log_push(EV_LED); /* only reachable if the call above came back */
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_LED));
}

/* ---- the day rollover: the no-op path ----------------------------------- */

void test_a_wake_on_the_same_day_rolls_nothing_over(void) {
    time_t now = flow_at(14, 0);
    flow_new_day = false;
    wake_flow_handle_day_rollover(&now);
    TEST_ASSERT_EQUAL_INT(1, flow_log_n);
    TEST_ASSERT_EQUAL_INT((int)EV_IS_NEW_DAY, (int)flow_log[0]);
    TEST_ASSERT_EQUAL_INT64(flow_at(14, 0), now); /* the caller's clock is left alone */
}

void test_the_new_day_check_reads_the_clock_it_was_handed(void) {
    mock_time_set(flow_at(9, 0));
    time_t now = flow_at(14, 0);
    wake_flow_handle_day_rollover(&now);
    TEST_ASSERT_EQUAL_INT64(flow_at(14, 0), flow_new_day_arg);
}

/* ---- ROW 13: rollover with a valid same-day NVS snapshot ---------------
   "snapshot wins; allocation not refunded". Power cycling a device at
   09:00 must not hand back the morning's spent screen time. */

void test_row13_a_valid_same_day_snapshot_beats_the_reset(void) {
    time_t now = flow_at(0, 5);
    flow_new_day = true;
    flow_restore_ok = true;
    wake_flow_handle_day_rollover(&now);
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_PERSIST_RESTORE));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_TIMER_RESET));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_RECORD_DATE));
    TEST_ASSERT_FALSE(flow_reset_called);
}

void test_row13_a_genuine_date_change_resets_and_records_the_new_day(void) {
    time_t now = flow_at(0, 5);
    flow_new_day = true;
    flow_restore_ok = false;
    wake_flow_handle_day_rollover(&now);
    TEST_ASSERT_TRUE(flow_reset_called);
    TEST_ASSERT_TRUE(flow_log_at(EV_TIMER_RESET) < flow_log_at(EV_RECORD_DATE));
}

/* The restore is asked with the CORRECTED clock: the window may have
   stepped it across the date line, and asking with the pre-window value
   is how a same-day snapshot gets misjudged. */
void test_the_snapshot_is_judged_against_the_corrected_clock(void) {
    time_t now = flow_at(0, 5);
    flow_new_day = true;
    flow_clock_after_window = flow_at(6, 30);
    wake_flow_handle_day_rollover(&now);
    TEST_ASSERT_EQUAL_INT64(flow_at(6, 30), flow_restore_arg);
}

/* `now` is an out-param for exactly this reason: everything later in the
   wake (the bed-time gate, the grid wait, this wake's tick) keys off the
   value the rollover leaves behind. */
void test_the_rollover_hands_the_corrected_clock_back_to_the_caller(void) {
    time_t now = flow_at(0, 5);
    flow_new_day = true;
    flow_clock_after_window = flow_at(6, 30);
    wake_flow_handle_day_rollover(&now);
    TEST_ASSERT_EQUAL_INT64(flow_at(6, 30), now);
}

void test_the_recorded_date_is_the_corrected_clock_too(void) {
    time_t now = flow_at(0, 5);
    flow_new_day = true;
    flow_restore_ok = false;
    flow_clock_after_window = flow_at(6, 30);
    wake_flow_handle_day_rollover(&now);
    TEST_ASSERT_EQUAL_INT64(flow_at(6, 30), flow_record_date_arg);
}

/* Fail-open: a window that never syncs still leaves a usable day. */
void test_a_rollover_whose_window_never_syncs_still_resets_the_day(void) {
    time_t now = flow_at(0, 5);
    flow_new_day = true;
    flow_restore_ok = false;
    flow_clock_after_window = 0; /* no step: the sync failed */
    mock_time_set(flow_at(0, 5));
    wake_flow_handle_day_rollover(&now);
    TEST_ASSERT_TRUE(flow_reset_called);
    TEST_ASSERT_EQUAL_INT64(flow_at(0, 5), now);
}

/* ---- the day rollover: yesterday's summary ------------------------------ */

/* Captured BEFORE the reset wipes the counters — and before the window
   that will publish it. */
void test_yesterdays_summary_is_queued_before_anything_is_reset(void) {
    time_t now = flow_at(0, 5);
    flow_new_day = true;
    flow_restore_ok = false;
    wake_flow_handle_day_rollover(&now);
    int queued = flow_log_at(EV_QUEUE_SUMMARY);
    TEST_ASSERT_TRUE(queued >= 0);
    TEST_ASSERT_TRUE(queued < flow_log_at(EV_TRY_WINDOW));
    TEST_ASSERT_TRUE(queued < flow_log_at(EV_TIMER_RESET));
}

void test_the_summary_carries_the_stored_date_and_the_days_screen_usage(void) {
    time_t now = flow_at(0, 5);
    flow_new_day = true;
    flow_date = "2026-07-28";
    flow_screen_used = 4321;
    wake_flow_handle_day_rollover(&now);
    TEST_ASSERT_EQUAL_STRING("2026-07-28", flow_summary_date);
    TEST_ASSERT_EQUAL_INT32(4321, flow_summary_used);
}

/* The completions come from the EXTRA slots (1..N), not from Screen: the
   1+i offset is the whole point of the loop, and dropping it would report
   Screen's number four times. */
void test_the_summary_reads_the_extra_slots_not_the_screen_slot(void) {
    time_t now = flow_at(0, 5);
    flow_new_day = true;
    wake_flow_handle_day_rollover(&now);
    TEST_ASSERT_EQUAL_INT(TIMER_EXTRA_SLOTS, flow_completion_asks);
    for (int i = 0; i < TIMER_EXTRA_SLOTS; i++) {
        TEST_ASSERT_EQUAL_INT(1 + i, flow_completion_slots[i]);
        TEST_ASSERT_EQUAL_UINT16((uint16_t)(100 + 1 + i), flow_summary_comp[i]);
    }
}

/* Nothing to report before the first day was ever recorded. */
void test_a_cold_boot_with_no_stored_date_queues_no_summary(void) {
    time_t now = flow_at(0, 5);
    flow_new_day = true;
    flow_date = "";
    wake_flow_handle_day_rollover(&now);
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_QUEUE_SUMMARY));
}

/* ...but the rest of the rollover still happens: a cold boot must still
   land on a fresh day. */
void test_a_cold_boot_rollover_still_clears_the_bonus_and_resets(void) {
    time_t now = flow_at(0, 5);
    flow_new_day = true;
    flow_date = "";
    flow_restore_ok = false;
    wake_flow_handle_day_rollover(&now);
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_BONUS_CLEAR));
    TEST_ASSERT_TRUE(flow_reset_called);
}

void test_the_retained_bonus_target_is_cleared_on_every_rollover(void) {
    time_t now = flow_at(0, 5);
    flow_new_day = true;
    wake_flow_handle_day_rollover(&now);
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_BONUS_CLEAR));
    TEST_ASSERT_TRUE(flow_log_at(EV_BONUS_CLEAR) < flow_log_at(EV_TRY_WINDOW));
}

/* The usage figure is read at the live clock, which at this point is
   still yesterday's — reading it after the window would attribute the
   step to yesterday's total. */
void test_the_days_usage_is_read_before_the_window_steps_the_clock(void) {
    time_t now = flow_at(0, 5);
    mock_time_set(flow_at(0, 5));
    flow_new_day = true;
    flow_clock_after_window = flow_at(6, 30);
    wake_flow_handle_day_rollover(&now);
    TEST_ASSERT_EQUAL_INT64(flow_at(0, 5), flow_screen_used_arg);
}

/* The usage figure is sampled BEFORE the completions loop, not after it.
   Screen's used seconds are allocation-minus-remaining at `now`, so on a
   RUNNING slot the answer moves while the loop runs — and a summary whose
   two halves were read at different instants is a subtly wrong day.
   Mutation testing surfaced this: hoisting the read below the loop
   escaped a suite that only checked WHICH clock it was given. */
void test_the_days_usage_is_sampled_before_the_completions_loop(void) {
    time_t now = flow_at(0, 5);
    flow_new_day = true;
    wake_flow_handle_day_rollover(&now);
    TEST_ASSERT_EQUAL_INT(0, flow_comps_asked_when_used_read);
    TEST_ASSERT_EQUAL_INT(TIMER_EXTRA_SLOTS, flow_completion_asks);
}

/* The full ordered trace of a rollover that resets. */
void test_the_rollover_effect_order_is_pinned_end_to_end(void) {
    time_t now = flow_at(0, 5);
    flow_new_day = true;
    flow_restore_ok = false;
    wake_flow_handle_day_rollover(&now);
    static const flow_event_t expect[] = {EV_IS_NEW_DAY,      EV_QUEUE_SUMMARY, EV_BONUS_CLEAR, EV_TRY_WINDOW,
                                          EV_PERSIST_RESTORE, EV_TIMER_RESET,   EV_RECORD_DATE};
    TEST_ASSERT_EQUAL_INT((int)(sizeof expect / sizeof expect[0]), flow_log_n);
    for (int i = 0; i < flow_log_n; i++) {
        TEST_ASSERT_EQUAL_INT((int)expect[i], (int)flow_log[i]);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_deepsleep_is_the_healthy_reason);
    RUN_TEST(test_poweron_and_external_reset);
    RUN_TEST(test_software_reset);
    RUN_TEST(test_crash_reasons);
    RUN_TEST(test_watchdogs_are_distinguishable);
    RUN_TEST(test_unknown_reason);
    RUN_TEST(test_unmapped_reason_falls_back_to_unknown);
    RUN_TEST(test_every_reason_maps_to_a_distinct_non_empty_string);
    RUN_TEST(test_a_wake_with_no_break_is_inert);
    RUN_TEST(test_a_break_still_running_is_not_an_edge);
    RUN_TEST(test_an_elapsed_break_that_nothing_ticked_still_chimes);
    RUN_TEST(test_the_end_latches_at_the_instant_the_wall_end_is_reached);
    RUN_TEST(test_a_break_one_second_short_of_its_wall_end_is_not_an_edge);
    RUN_TEST(test_a_latch_left_by_a_foreign_tick_is_still_drained);
    RUN_TEST(test_the_edge_is_taken_exactly_once_per_wake);
    RUN_TEST(test_a_running_extra_suppresses_the_chime_and_the_snap);
    RUN_TEST(test_an_end_observed_inside_the_grace_still_chimes);
    RUN_TEST(test_an_end_observed_past_the_grace_is_silent);
    RUN_TEST(test_the_chime_and_the_snap_back_are_one_event);
    RUN_TEST(test_the_snap_returns_to_the_interrupted_slot_not_always_screen);
    RUN_TEST(test_no_snap_when_the_selection_is_already_where_it_belongs);
    RUN_TEST(test_row3_the_repaint_shows_the_snapped_back_slot);
    RUN_TEST(test_a_wake_without_a_break_end_never_repaints);
    RUN_TEST(test_the_repaint_happens_once_per_edge);
    RUN_TEST(test_a_silent_break_end_still_repaints);
    RUN_TEST(test_the_flag_is_not_set_without_an_edge);
    RUN_TEST(test_any_edge_sets_the_flag_including_a_silent_one);
    RUN_TEST(test_the_flag_survives_a_later_drain_that_found_nothing);
    RUN_TEST(test_row4_a_break_end_forces_every_later_render_full);
    RUN_TEST(test_a_swap_across_the_break_screen_is_full_by_the_boundary_rule);
    /* ---- the guard matrix ---- */
    RUN_TEST(test_row5_the_pause_poll_consumes_only_the_a_bit);
    RUN_TEST(test_row5_a_refused_pause_poll_still_leaves_b_and_c_latched);
    RUN_TEST(test_row5_the_join_poll_consumes_only_the_a_bit);
    RUN_TEST(test_row5_a_join_poll_with_no_a_press_leaves_b_and_c_latched);
    RUN_TEST(test_the_pause_poll_consumes_the_a_press_even_when_it_refuses);
    RUN_TEST(test_the_pause_poll_pauses_only_a_running_timer);
    RUN_TEST(test_the_pause_poll_does_nothing_without_an_a_press);
    RUN_TEST(test_the_pause_poll_stamps_the_clock_it_reads_now);
    RUN_TEST(test_the_pause_poll_pauses_at_most_once_per_press);
    RUN_TEST(test_the_join_poll_is_inert_without_a_latched_a);
    RUN_TEST(test_the_join_poll_reports_nothing_when_the_state_map_refuses);
    RUN_TEST(test_the_join_poll_acks_on_the_leds_for_every_accepted_action);
    RUN_TEST(test_the_join_poll_neither_paints_nor_syncs);
    RUN_TEST(test_the_join_poll_applies_the_map_at_the_current_clock);
    RUN_TEST(test_row18_button_a_during_a_break_is_refused_with_no_side_effects);
    RUN_TEST(test_row18_the_refusal_keys_on_the_painted_state_not_the_live_one);
    RUN_TEST(test_button_a_on_an_expired_slot_is_refused_by_the_map_not_the_guard);
    RUN_TEST(test_a_start_applies_the_map_at_the_callers_clock);
    RUN_TEST(test_a_start_holds_the_pre_press_colour_before_the_led);
    RUN_TEST(test_a_start_acks_before_it_opens_the_window);
    RUN_TEST(test_a_start_hands_the_caller_the_post_window_clock);
    RUN_TEST(test_a_start_without_a_window_still_re_reads_the_clock);
    RUN_TEST(test_a_synced_start_shifts_the_expiry_by_the_measured_step);
    RUN_TEST(test_a_backwards_clock_step_shifts_the_expiry_backwards);
    RUN_TEST(test_the_measured_step_is_taken_exactly_once);
    RUN_TEST(test_a_start_that_misses_the_sync_notes_it_and_shifts_nothing);
    RUN_TEST(test_a_start_whose_window_will_not_open_skips_the_sync_entirely);
    RUN_TEST(test_a_start_on_an_already_synced_wake_opens_no_second_window);
    RUN_TEST(test_a_resume_takes_exactly_the_same_path_as_a_start);
    RUN_TEST(test_a_pause_neither_holds_nor_syncs_nor_moves_the_clock);
    RUN_TEST(test_row19_button_b_while_running_without_parent_testing_is_refused);
    RUN_TEST(test_button_b_is_never_allowed_while_the_slot_is_running);
    RUN_TEST(test_row19_the_reload_gate_is_asked_with_the_build_flag);
    RUN_TEST(test_button_b_resets_a_reloadable_slot_when_the_rule_allows_it);
    RUN_TEST(test_button_b_is_refused_when_the_reload_itself_declines);
    RUN_TEST(test_button_b_lights_nothing_and_opens_nothing_of_its_own);
    RUN_TEST(test_row20_a_swap_onto_an_expired_slot_reports_the_change);
    RUN_TEST(test_row20_the_reported_swap_suppresses_the_expiry_alert);
    RUN_TEST(test_row4_a_swap_during_a_break_renders_full_end_to_end);
    RUN_TEST(test_row4_a_swap_back_onto_the_break_screen_renders_full);
    RUN_TEST(test_row4_a_swap_from_the_break_onto_an_expired_slot_is_full_not_an_alert);
    RUN_TEST(test_a_refused_swap_reports_no_selection_change);
    RUN_TEST(test_a_swap_during_a_break_is_allowed);
    RUN_TEST(test_a_swap_lights_nothing_opens_nothing_and_leaves_the_clock_alone);
    RUN_TEST(test_every_arm_clears_the_selection_report_before_deciding);
    RUN_TEST(test_only_a_successful_swap_reports_a_selection_change);
    RUN_TEST(test_button_d_and_button_none_are_inert_in_the_dispatch);
    RUN_TEST(test_the_break_tail_poll_is_inert_when_nothing_is_latched);
    RUN_TEST(test_the_break_tail_poll_drains_the_whole_latch_including_d);
    RUN_TEST(test_a_lone_d_press_in_the_break_tail_is_consumed_and_ignored);
    RUN_TEST(test_the_break_tail_acts_on_a_lone_button_b_press);
    RUN_TEST(test_the_break_tail_acts_on_the_highest_priority_latched_press);
    RUN_TEST(test_the_break_tail_lets_button_c_move_to_another_timer);
    RUN_TEST(test_the_break_tail_never_opens_a_second_network_window);
    RUN_TEST(test_the_break_tail_hands_the_render_the_clock_the_action_left);
    RUN_TEST(test_a_refused_press_in_the_break_tail_neither_lights_nor_renders);
    RUN_TEST(test_the_break_tail_lights_the_pixels_before_it_renders);
    RUN_TEST(test_the_break_tail_reports_the_state_that_was_painted_not_the_new_one);
    RUN_TEST(test_row22_a_zero_interval_never_starts_a_break);
    RUN_TEST(test_row22_a_zero_interval_is_decided_before_the_balance_is_asked);
    RUN_TEST(test_the_gate_reads_both_config_values_up_front);
    RUN_TEST(test_the_compile_time_defaults_stand_when_nvs_never_stored_them);
    RUN_TEST(test_row22_a_one_minute_interval_is_an_interval_not_a_disable);
    RUN_TEST(test_a_one_minute_duration_is_a_real_duration);
    RUN_TEST(test_a_balance_short_of_the_interval_starts_nothing);
    RUN_TEST(test_the_interval_is_asked_of_the_balance_in_seconds);
    RUN_TEST(test_the_break_is_started_for_the_configured_duration_in_seconds);
    RUN_TEST(test_the_gate_uses_the_clock_it_was_handed_not_the_live_one);
    RUN_TEST(test_a_started_break_persists_then_paints_then_alarms);
    RUN_TEST(test_a_started_break_runs_the_break_alarm_not_the_expiry_one);
    RUN_TEST(test_the_started_break_effect_order_is_pinned_end_to_end);
    RUN_TEST(test_row21_a_break_that_would_cross_bed_time_goes_to_bed_instead);
    RUN_TEST(test_row21_the_bed_time_engage_is_always_audible);
    RUN_TEST(test_row21_the_bed_time_engage_gets_the_gates_own_clock);
    RUN_TEST(test_a_break_that_finishes_before_bed_time_still_starts);
    RUN_TEST(test_a_break_landing_exactly_on_bed_time_is_skipped);
    RUN_TEST(test_bed_time_disabled_never_skips_a_break);
    RUN_TEST(test_a_break_starting_after_bed_time_is_not_this_gates_problem);
    RUN_TEST(test_a_break_that_is_not_due_never_reaches_the_bed_time_check);
    RUN_TEST(test_row12_the_expired_snapshot_is_persisted_before_anything_else);
    RUN_TEST(test_row12_the_snapshot_is_persisted_before_the_alarm_runs);
    RUN_TEST(test_the_timesup_screen_precedes_the_alarm);
    RUN_TEST(test_the_main_layout_is_repainted_after_the_alarm);
    RUN_TEST(test_the_expiry_alert_runs_the_expiry_alarm_not_the_break_one);
    RUN_TEST(test_the_expiry_alert_effect_order_is_pinned_end_to_end);
    RUN_TEST(test_the_expiry_alert_returns_to_its_caller);
    RUN_TEST(test_a_wake_on_the_same_day_rolls_nothing_over);
    RUN_TEST(test_the_new_day_check_reads_the_clock_it_was_handed);
    RUN_TEST(test_row13_a_valid_same_day_snapshot_beats_the_reset);
    RUN_TEST(test_row13_a_genuine_date_change_resets_and_records_the_new_day);
    RUN_TEST(test_the_snapshot_is_judged_against_the_corrected_clock);
    RUN_TEST(test_the_rollover_hands_the_corrected_clock_back_to_the_caller);
    RUN_TEST(test_the_recorded_date_is_the_corrected_clock_too);
    RUN_TEST(test_a_rollover_whose_window_never_syncs_still_resets_the_day);
    RUN_TEST(test_yesterdays_summary_is_queued_before_anything_is_reset);
    RUN_TEST(test_the_summary_carries_the_stored_date_and_the_days_screen_usage);
    RUN_TEST(test_the_summary_reads_the_extra_slots_not_the_screen_slot);
    RUN_TEST(test_a_cold_boot_with_no_stored_date_queues_no_summary);
    RUN_TEST(test_a_cold_boot_rollover_still_clears_the_bonus_and_resets);
    RUN_TEST(test_the_retained_bonus_target_is_cleared_on_every_rollover);
    RUN_TEST(test_the_days_usage_is_read_before_the_window_steps_the_clock);
    RUN_TEST(test_the_days_usage_is_sampled_before_the_completions_loop);
    RUN_TEST(test_the_rollover_effect_order_is_pinned_end_to_end);
    return UNITY_END();
}
