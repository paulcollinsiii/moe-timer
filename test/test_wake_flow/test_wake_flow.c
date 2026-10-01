#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

/* Single-TU: the real policies this module consults — the chime grace
   window, the render choice, the press latch with its B > C > D > A
   priority, and the bed-time crossing rule — are compiled in alongside
   the module under test, so the edges below are pinned against the
   shipping rules rather than a restatement of them. button_latch.c in
   particular is what makes row 5 a statement about the real masked take:
   buttons.c's take wrappers are pure pass-throughs to it (a critical
   section either side), so the stubs below delegate to it exactly as the
   device does. bedtime.c (with the quiet-hours HHMM helpers it leans on)
   is what makes row 21 a statement about the real crossing arithmetic
   rather than about an injected bool. The mock clock comes along because
   every decision here is a wall-time comparison. display_layout.c joins
   them for display_screen_for() alone: the render path now promotes a
   change of SCREEN KIND to a full refresh, and a stub of that function
   would make every such row an assertion about the stub's precedence
   rather than about the painter's. It is pure (no LVGL, no device) and
   collides with none of the display stubs below, which cover the paint
   entry points and not the layout rules. chores.c (pure, layer 1) joins
   for chores_is_acked() alone: the rollover summary counts the day's
   acks through it, so the "bits above the configured count are not a
   chore done" rule is the shipping one, not a restatement. Everything
   with a device behind it gets a link-time spy stub in the preamble
   below. */
// clang-format off
#include "../../main/bedtime.c"
#include "../../main/button_latch.c"
#include "../../main/chores.c"
#include "../../main/display_layout.c"
#include "../../main/quiet_hours.c"
#include "../../main/wake_policy.c"
#include "mock_hal_time.c"
// clang-format on

#include "alerts.h"
#include "app_state.h"
#include "audio.h"
#include "battery.h"
#include "button_actions.h"
#include "buttons.h"
#include "chore_store.h" /* chore_store_load_names(), stubbed for the strip paint */
#include "config_cache.h"
#include "display.h"
#include "lock_gate.h"
#include "mqtt_ha.h"
#include "net_apply.h"
#include "net_window.h"
#include "nvs_config.h"
#include "nvs_defaults.h"
#include "ota_flow.h" /* ota_trigger_t, for the arm stub below */
#include "ota_task.h"
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
    /* EV_REPAINT was here: paint_current_state_full()'s stub pushed it.
       That seam is wake_flow.c's own static now, so the repaint appears in
       the log as its body instead — see flow_repaint_at() below. */
    /* the guard matrix */
    EV_A_APPLY,
    EV_B_APPLY,
    /* button_chore_ack_apply(): B, C or D ticking a checkbox instead of
       doing its timer job (design 2.4). Logged rather than only counted
       because several cases below are about the ack NOT happening — a
       press that fell through to the timer action instead — and a final
       mask check cannot tell "never acked" apart from "acked and toggled
       back". */
    EV_CHORE_ACK,
    EV_PAUSE,
    EV_LED,
    /* The checklist's four pixels (M2-T8). A SEPARATE code from EV_LED
       and not a flavour of it: the two painters collide on pixel 0 —
       NP_STATE_PIXEL is chore slot 2's pixel, button D's row, under
       status_led.c's mapping (it was the GATE's until M2-HW2 inverted the
       strip; the mapping is a permutation, so pixel 0 belongs to some
       slot either way) — so "which of the two ran" is the whole question
       on a chore-mode wake and one shared code could not ask it. */
    EV_CHORE_LEDS,
    EV_LED_CLAIM, /* net_window_claim_leds(): the sync pixel stands down */
    EV_NET_OPEN,
    EV_WAIT_NTP,
    EV_TAKE_STEP,
    EV_SHIFT_EXPIRY,
    EV_NOTE_UNSYNCED,
    EV_RELOAD_ALLOWED,
    EV_RELOAD,
    EV_SELECT_NEXT,
    /* the break gate */
    EV_CFG_INTERVAL,
    EV_CFG_DURATION,
    EV_BREAK_DUE,
    EV_BEDTIME_ENGAGE,
    EV_START_BREAK,
    EV_PERSIST_SAVE,
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
    /* the awake watches */
    EV_TIMER_TICK,
    EV_MAKE_STATE,
    EV_FULL_REFRESH,
    EV_PARTIAL,
    EV_PIXELS_OFF,
    EV_PIXELS_BINARY,
    /* the post-action tail and the two wake handlers */
    EV_STATS_COLLECT,
    EV_STATS_POST,
    EV_NET_FINISH,
    EV_CHECK_BEDTIME,
    EV_PROMOTE_RENDER,
    EV_WAKEUP_BUTTON,
    /* the painted mode (C16). Logged rather than only sampled because two
       of the cases below are about a store that must NOT happen — the
       final ack, and the rollover, which gets its clear from
       timer_reset()'s memset and must not acquire a second one here. A
       final-value check cannot tell "never written" apart from "written
       with the value it already had". */
    EV_SET_MODE,
    /* the OTA call sites */
    EV_OTA_ARM,
    EV_OTA_APPLY,
    EV_SLEEP,
} flow_event_t;

/* Room for a whole watch: the final-minute loop spins at 250 ms for up to
   SLEEP_PLAN_WATCH_SEC and asks the break balance on every pass, so a
   single case can log several hundred events. A log that silently
   truncated would turn an ordering assertion into a coin toss. */
static flow_event_t flow_log[1024];
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

/* Position of the Nth (1-based) occurrence, or -1. The post-join
   re-render performs a SECOND tick and a SECOND paint, so an ordering
   assertion about the re-render cannot be written against first
   occurrences alone. */
static int flow_log_at_nth(flow_event_t ev, int n) {
    int seen = 0;
    for (int i = 0; i < flow_log_n; i++) {
        if (flow_log[i] == ev && ++seen == n)
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

/* Where the break screen's own paint landed in the log, or -1.

   paint_break_started() used to be a main.c seam this file stubbed, so
   "the break painted" was a single logged event. The residency audit
   moved it into wake_flow.c — the ORDER inside it is behaviour and main.c
   may not hold a decision — so what the log carries now is its body: a
   tick, a state assembly, the LED, the full refresh. The break gate is
   the only caller of timer_start_break() and the only path that follows a
   paint with the break alarm, so the full refresh between those two
   anchors is unambiguously that paint, and nothing else in the suite can
   forge the pair.

   Deliberately lenient about what sits BETWEEN the anchors: the order
   inside the body is pinned by the two cases that exist for it
   (test_the_started_break_effect_order_is_pinned_end_to_end and
   test_the_break_screen_lights_the_led_before_the_flush_not_after), so a
   reordering fails the cases that are about ordering rather than the
   dozen that only need to know the break screen reached the panel. */
static int flow_break_paint_at(void) {
    int started = flow_log_at(EV_START_BREAK);
    if (started < 0)
        return -1;
    for (int i = started; i < flow_log_n; i++) {
        if (flow_log[i] == EV_ALERT_BREAK)
            return -1; /* the alarm pulsed over an unpainted panel */
        if (flow_log[i] == EV_FULL_REFRESH)
            return i;
    }
    return -1;
}

/* The gate returns as soon as it has started one break, so this is 0 or
   1 by construction; it reads as a count because that is the question
   every caller is asking. */
static int flow_break_paint_count(void) {
    return flow_break_paint_at() >= 0 ? 1 : 0;
}

/* "Nothing was painted", asked WITHOUT presupposing the break start.

   The helper above cannot answer that question: it anchors on
   EV_START_BREAK and returns -1 by construction whenever that event is
   absent, so `0 == flow_log_count(EV_START_BREAK)` followed by
   `0 == flow_break_paint_count()` is a tautology — the second line cannot
   fail while the first passes, and the three gate cases that were written
   that way had lost the independent observation the older EV_BREAK_PAINT
   stub gave them. These two events are what a paint leaves in the log
   whether or not a break was ever started, so a gate that painted on a
   path it should have returned from fails here. */
static void flow_assert_nothing_painted(void) {
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_MAKE_STATE));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_FULL_REFRESH));
}

/* Where the full-state repaint reached the panel — the index of its FULL
   REFRESH — or -1. Same shape, and the same reason, as the helper above.

   paint_current_state_full() was a main.c seam this file stubbed, so "the
   panel was repainted" used to be one logged event. The audit's review
   moved it into wake_flow.c: "the battery ADC read has no host answer" is
   not one of the four residency reasons, and lock_gate.c refutes it by
   reading the same ADC under a host test. So what the log carries now is
   the body — a tick, a state assembly, a full refresh, with NOTHING
   between them.

   That adjacency is the discriminator, and it is exact rather than
   lenient because this is the only full refresh in the module with
   nothing between the assembly and the flush. Every other one puts
   something there:
     - paint_break_started()       EV_LED, held blue through the refresh
     - render_action_result()      EV_LED (and its re-render half likewise)
     - the final-minute pause      EV_LED
     - the tick handler's render   EV_PROMOTE_RENDER
   Those four orderings are each pinned by a case of their own, so a
   reordering that would confuse this helper fails an ordering test first
   rather than silently inflating a repaint count. */
static int flow_repaint_body_at(int from) {
    for (int i = from; i + 2 < flow_log_n; i++) {
        if (flow_log[i] == EV_TIMER_TICK && flow_log[i + 1] == EV_MAKE_STATE && flow_log[i + 2] == EV_FULL_REFRESH)
            return i;
    }
    return -1;
}

static int flow_repaint_at(void) {
    int at = flow_repaint_body_at(0);
    return at < 0 ? -1 : at + 2; /* the flush: where the panel got it */
}

static int flow_repaint_count(void) {
    int n = 0;
    for (int at = flow_repaint_body_at(0); at >= 0; at = flow_repaint_body_at(at + 3))
        n++;
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
/* The snap moves the SELECTION, so timer_get_state() afterwards reports
   the interrupted slot's state and not the one that was showing. -1 (the
   default) leaves it alone; a case that cares sets the slot's state,
   which is what makes "before is captured after the drain" observable.
   flow_state itself belongs to the button model further down — declared
   here as a tentative definition so the break model can reach it. */
static int flow_state_at_interrupted;
static timer_state_t flow_state;

bool timer_select_interrupted(void) {
    if (flow_extra_running)
        return false;
    flow_active_slot = flow_interrupted_slot;
    if (flow_state_at_interrupted >= 0) {
        flow_state = (timer_state_t)flow_state_at_interrupted;
    }
    flow_log_push(EV_SNAP_BACK);
    return true;
}

void audio_break_over_chime(void) {
    flow_log_push(EV_CHIME);
}

/* paint_current_state_full() was stubbed here until the audit's review
   moved it out of main.c and into wake_flow.c. It is code under test now,
   so there is deliberately no stub: it reaches the log through
   timer_tick(), app_state_display() and display_full_refresh(), all
   stubbed below, and flow_repaint_at() is how a case asks where it
   landed. What reaches the panel is the ACTIVE slot's number under the
   ACTIVE slot's layout — row 3's subject — and the app_state_display()
   stub is what records that pair now. */

/* ---- the injected button + action model ---------------------------------

   Everything the guard matrix can reach. Defaults are POISONED in setUp()
   wherever "never touched" and "touched with the value we expect" would
   otherwise be the same assertion — the captured reload flag starts true
   so that a gate which is never asked cannot read as "asked with false",
   and the captured clock step starts at a value the code can never
   produce. */

static timer_state_t flow_state; /* the ACTIVE slot's state */
static time_t flow_pause_arg;
/* button_a_toggle_allowed()'s answer, injected. The predicate itself —
   refused while the active slot is RUNNING, refused with no chores
   configured — is test_button_actions' subject; what this suite owns is
   what the wake flow DOES with the answer, and in particular the order it
   does it in relative to the break-end drain. */
static bool flow_a_allowed;
static btn_b_action_t flow_b_result;
static time_t flow_b_apply_arg;
static bool flow_slot_reloadable; /* the selected def's reloadable flag */
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

/* render_action_result is no longer a seam this file can stub: cycle 11
   moved it into wake_flow.c, where it is a static the module reaches
   directly. So what used to be an argument capture is now an assertion
   about what the REAL render did — which is strictly more than the stub
   could say, because the arguments now travel through the shipping
   wake_policy_render() before anything is observable:

     `now`               -> the clock timer_tick() was ticked at
                            (flow_tick_arg) and the wall_time the state
                            was assembled against
     `btn`               -> BTN_D forces a full refresh whatever the
                            policy chose
     `before`            -> full vs partial across the break-screen
                            inversion
     `selection_changed` -> whether an EXPIRED landing alerts

   FLOW_RENDERS() stands in for the old EV_ACTION_RENDER count: the
   render's state assembly is the one thing it always does and nothing
   else on these paths does — except the two paints that own their own
   flush, the break screen's and the full repaint's, both of which
   assemble a state of their own now that wake_flow.c owns them rather
   than main.c. Both are subtracted back out, because every use of this
   macro means "the ACTION/TICK render drew a main layout" and several of
   them assert 0 on exactly the paths where the break screen or the
   post-alarm repaint took the wake. Each has its own helper for cases
   that want to assert on it directly. */
#define FLOW_RENDERS() (flow_log_count(EV_MAKE_STATE) - flow_break_paint_count() - flow_repaint_count())

/* The flush half of the same question: full refreshes the ACTION/TICK
   render itself put on the panel. Cases that ask it mean "the render
   painted nothing of its own", and the two seams that own their own flush
   would otherwise answer for it — the post-alarm repaint in particular
   runs on exactly the paths where the render is expected to be silent. */
#define FLOW_RENDER_FLUSHES() (flow_log_count(EV_FULL_REFRESH) - flow_break_paint_count() - flow_repaint_count())

timer_state_t timer_get_state(void) {
    return flow_state;
}

void timer_pause(time_t at) {
    flow_pause_arg = at;
    flow_state = TIMER_PAUSED;
    flow_log_push(EV_PAUSE);
}

/* The mode toggle, modelled the same way as B's map below: the GATE is
   injected, the transition it leaves behind is real. Reading flow_mode and
   writing the other value through timer_set_mode() is what makes the
   break-end collision observable here — a drain that reverts to
   APP_MODE_TIMERS after this ran is a store this stub cannot see, so only
   the ORDER of the two writes can tell the two implementations apart. */
btn_a_action_t button_a_apply(void) {
    flow_log_push(EV_A_APPLY);
    if (!flow_a_allowed) {
        return BTN_A_NONE;
    }
    const app_mode_t next = (timer_mode() == APP_MODE_CHORES) ? APP_MODE_TIMERS : APP_MODE_CHORES;
    timer_set_mode(next);
    return (next == APP_MODE_CHORES) ? BTN_A_CHORES : BTN_A_TIMERS;
}

/* The ack, modelled the same way as the two maps around it: WHAT it does
   to the mask, the flash record and the Screen timer is
   test_button_actions' subject, because that is where the real chores.c,
   chore_store.c and timer.c are linked. What this suite owns is the
   ROUTING — which button reaches it, with which row index, in which mode,
   and what the wake does around it (the break-end drain, the render
   policy, the latch). So the stub records the call and returns an
   injected verdict.

   flow_ack_result defaults to BTN_ACK_NONE, matching the shipped fleet
   (no chore list, never in chore mode): a case that wants the ack to LAND
   has to say so, the same way it has to opt into a chore list at all. */
static btn_ack_action_t flow_ack_result;
static int flow_ack_idx;    /* the row the flow asked for; -1 = never asked */
static time_t flow_ack_now; /* the clock it was handed */

/* Today's ack mask, as timer_chore_acked() would report it out of RTC.
   Declared up here rather than with the rest of the chore state below
   because the stub on the next line is what MOVES it. */
static uint8_t flow_chore_acked;

/* The delay total at each apply. The hold §2.5 asks for sits BETWEEN the
   apply and the flip, so "did this press pay it" is the gap between these
   two instants and nothing else — measuring frame-to-frame instead would
   also count whatever poll loop the press arrived through, which is how
   the first version of the second-ack case read 250 ms of break tail as a
   hold that was never paid. */
static uint32_t flow_ack_at[4];
static int flow_ack_n;

btn_ack_action_t button_chore_ack_apply(uint8_t idx, time_t now) {
    flow_log_push(EV_CHORE_ACK);
    flow_ack_idx = (int)idx;
    flow_ack_now = now;
    if (flow_ack_n < (int)(sizeof flow_ack_at / sizeof flow_ack_at[0])) {
        flow_ack_at[flow_ack_n++] = mock_delay_total_ms();
    }
    /* Same arrangement as button_b_apply()'s stub below: what it RETURNS
       is injected, but the transition it leaves behind is REAL, so a
       caller reading timer_chore_acked() after the call sees what the
       device would show. M2-T8 is the reason this matters here — the
       pre-press pixel frame and the flip are the SAME painter called
       twice, and with an inert stub the two frames would be identical
       and every ordering assertion about them would pass on a painter
       that never read the mask at all.

       A bare XOR, not chores_toggle_ack(): that function's bounds check
       is test_chores' subject, and the real apply refuses an
       out-of-range row with BTN_ACK_NONE — which a case injects — so the
       applied path is the only one that reaches here. */
    if (flow_ack_result != BTN_ACK_NONE) {
        flow_chore_acked ^= (uint8_t)(1u << idx);
    }
    return flow_ack_result;
}

/* button_actions.c's map, modelled rather than switched: what it RETURNS
   is injected (that map is test_button_actions' business), but the
   transition it leaves behind is real, so a caller reading timer_get_state()
   after the call sees what the device would show. */
btn_b_action_t button_b_apply(time_t at) {
    flow_b_apply_arg = at;
    flow_log_push(EV_B_APPLY);
    switch (flow_b_result) {
        case BTN_B_PAUSED:
            flow_state = TIMER_PAUSED;
            break;
        case BTN_B_STARTED:
        case BTN_B_RESUMED:
            flow_state = TIMER_RUNNING;
            /* timer_any_extra_running() is a fact about the slots, not a
               free switch: starting a slot that is not Screen IS what
               makes it true. Row 2 turns on that link — the started extra
               is what suppresses the break end — so the model has to
               carry it rather than let a case assert it into place. */
            if (flow_active_slot != FLOW_SCREEN) {
                flow_extra_running = true;
            }
            break;
        case BTN_B_RELOADED:
            /* The real map reaches this through timer_reload(), which
               returns the slot to IDLE at full duration rather than
               running anything. */
            flow_state = TIMER_IDLE;
            break;
        default:
            break; /* BTN_B_NONE: no transition */
    }
    return flow_b_result;
}

/* timer.c's rule, modelled off the shipping implementation rather than
   off a free switch: a RUNNING slot is never reloadable (pause first),
   and only a reloadable def ever is. Keeping the RUNNING half real is
   what makes row 19 a statement about the shipping guard instead of
   about a free-floating flag. */
bool timer_reload_allowed(void) {
    flow_log_push(EV_RELOAD_ALLOWED);
    if (flow_state == TIMER_RUNNING)
        return false;
    return flow_slot_reloadable;
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

/* Records WHEN it first lit, not just that it did: the STATUS_LED_ACK_HOLD_MS
   hold before it exists so the WHITE/AMBER -> GREEN transition is visible, and
   a hold moved after the LED would be invisible to a plain counter. The hold is
   a menuconfig figure, and this suite is deliberately built at a value that is
   NOT its default (test/CMakeLists.txt), so never write it as a number. */
void status_led_show_timer_state(void) {
    if (flow_log_count(EV_LED) == 0) {
        flow_delay_at_led = mock_delay_total_ms();
    }
    flow_log_push(EV_LED);
}

/* A press that lands PART WAY THROUGH an awake wait, which is what rows 1,
   2 and 6 are all about. The suite's clock only moves inside
   hal_delay_ms(), so "the user pressed A twelve seconds into the wait" is
   expressed as a delay total: the edge is recorded through the REAL latch
   once the wait has burned that long, exactly as the GPIO ISR would have
   latched it mid-loop on device. -1 (the default) disables it, so every
   case that does not arm one is untouched.

   Armed from mock_hal_time's delay hook and NOT from the take stubs
   below. That was the original shape and it made the known-bug case
   vacuous: the press only entered the latch as a side effect of the code
   under test performing a take, so a change that stops taking (which is
   exactly the eventual fix for BUG-2) made the press never arrive at all,
   and "the press was eaten" became indistinguishable from "there was no
   press". An ISR does not wait to be asked. flow_deferred_delivered is
   the other half of that lesson: a case that turns on a press being eaten
   must assert the press was actually delivered, or deleting the arming
   lines leaves every one of its assertions still true. */
static int32_t flow_deferred_press_ms;
static button_id_t flow_deferred_press_btn;
static int flow_deferred_delivered;        /* how many armed presses actually landed */
static void flow_deferred_press_due(void); /* defined with the harness below */

/* How many times each take ran — the polling cadence of a loop is part of
   its contract (100 ms in the grid wait, 250 ms in both watches) and a
   total delay alone cannot tell 30 polls of 100 ms from 3 of 1000 ms. */
static int flow_mask_takes; /* the pause poll's masked take */
static int flow_full_takes; /* the break tail's (and the watch entry's) take */
/* What that take actually returned. flow_full_takes counts CALLS, and the
   break tail takes unconditionally, so the count alone cannot tell a
   delivered press from an empty latch — asserting it as proof of delivery
   is vacuous. This records the content, which is the discriminating fact. */
static uint8_t flow_last_full_take;

/* buttons.c's take wrappers are pass-throughs to the latch compiled in
   above — a critical section, and a level sample fed to the release gate —
   so these are the device's behaviour, not a model of it. Row 5 is a
   property of button_latch's masked take, and this is what puts the real
   one under the module.

   THE SAMPLE IS THE M2-T12 HALF and it is not decoration: button_latch's
   release gate refuses a falling edge on a button nobody has seen come back
   up, so a take stub that skipped the sample would make every second press
   of one button disappear in here and nowhere on the device. */
static void flow_note_levels(void); /* defined with the press harness below */

uint8_t buttons_take_pressed(void) {
    flow_note_levels();
    flow_full_takes++;
    flow_last_full_take = button_latch_take();
    return flow_last_full_take;
}

uint8_t buttons_take_pressed_mask(uint8_t mask) {
    flow_note_levels();
    flow_mask_takes++;
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
static time_t flow_break_due_from; /* 0 = use flow_break_due_ret, see below */
static time_t flow_break_due_now;
static int32_t flow_break_due_interval; /* SECONDS, as passed */

static int flow_bed_min; /* what the bed-time cache answers */
static time_t flow_start_break_now;
static int32_t flow_start_break_dur; /* SECONDS, as passed */
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

/* The balance crosses the interval at a WALL INSTANT, and row 9 is about
   a crossing that happens INSIDE the final-minute watch — after the
   per-wake check has already passed. flow_break_due_from is that instant;
   0 (the default) leaves the plain injected answer every other case
   uses. */
bool timer_break_due(time_t now, int32_t interval_sec) {
    flow_log_push(EV_BREAK_DUE);
    flow_break_due_now = now;
    flow_break_due_interval = interval_sec;
    if (flow_break_due_from != 0)
        return now >= flow_break_due_from;
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

/* paint_break_started() was stubbed here until the residency audit moved
   it out of main.c and into wake_flow.c. It is code under test now, so
   there is deliberately no stub: what it does reaches the log through
   timer_tick(), make_display_state(), status_led_show_timer_state() and
   display_full_refresh(), all of which are stubbed below, and
   flow_break_paint_at() is how a case asks where it landed. */

/* An alarm holds the CPU for up to ~15 s on device, and the tick handler
   re-reads the wall clock afterwards precisely because of that (aa8be4c).
   The mock clock is frozen except inside hal_delay_ms(), so with a still
   clock "re-read" and "reuse the value from before the render" are the
   same observation. flow_alert_seconds is what makes them different; 0
   (the default) leaves every existing case's timing untouched. */
static int32_t flow_alert_seconds;

bool alert_run(alert_kind_t kind) {
    flow_last_alert = kind;
    flow_log_push(kind == ALERT_EXPIRY ? EV_ALERT_EXPIRY : EV_ALERT_BREAK);
    if (flow_alert_seconds != 0) {
        mock_time_set(hal_time_now() + flow_alert_seconds);
    }
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
static int flow_summary_chores_done; /* poisoned per case: -1 = never queued */
static int flow_summary_chores;
/* The window rewriting the acks, as a config document's chore-list edit
   does on device (config_apply.c reconciles them mid-window). -1 (the
   default) leaves them alone. */
static int flow_acked_after_window;

static bool flow_restore_ok;
static time_t flow_restore_arg;
static time_t flow_record_date_arg;
static bool flow_reset_called;
static time_t flow_clock_after_window; /* 0 = the window does not step the clock */
static int flow_state_after_window;    /* -1 = the window leaves the state alone */
/* M3-T4 fix pass: a B pressed DURING the window, handed to the real join
   poll the way net_window_join() would hand it. BTN_NONE (the default)
   leaves the window exactly as it was; the poll's answer is kept. */
static button_id_t flow_window_join_press;
static bool flow_window_join_polled;
static void flow_press(button_id_t btn);

/* ---- the chore checklist's live state (design rows C16, C17) ------------

   Two RTC-backed reads the paint depends on, declared here because
   timer_reset()'s stub below has to clear the first of them the way the
   real memset does.

   flow_mode is the stored byte behind timer_mode()/timer_set_mode().
   flow_chore_count is what chore_store_load_names() would have reported
   into display_state_t.chore_count — 0 is the shipped default (row C1: no
   chores configured), so a case has to opt INTO having a list at all, the
   same way it opts into a new day or an open window. */
static app_mode_t flow_mode;
static uint8_t flow_chore_count;
static int flow_set_mode_calls;
static app_mode_t flow_painted_mode; /* what actually reached the panel */
/* The two fields a WRONG emptied-list guard would key on instead of the
   count — "everything is ticked" and "the day has released". Carried on
   the assembled state purely so the final-ack case can set the count to a
   configured list and these to the all-done values at the same time; with
   them absent, a guard written as `chore_outstanding == 0` would be
   indistinguishable from the shipped one and the case that exists to
   reject it could not fail. */
static uint8_t flow_chore_outstanding;
static bool flow_chore_released;

/* The two RTC reads the strip paint makes for itself. flow_chore_released
   is shared with the assembled state above on purpose: app_state.c fills
   st.chore_released from this same getter, so one variable keeps the
   panel and the pixels from ever disagreeing inside a case. */
uint8_t timer_chore_acked(void) {
    return flow_chore_acked;
}

bool timer_chore_released(void) {
    return flow_chore_released;
}

/* The NVS read behind the row count. flow_chore_count is already defined
   as "what chore_store_load_names() would have reported", so this is that
   sentence made executable rather than a second knob that can drift from
   the one the panel assertions use. The names themselves are never read
   by anything this suite compiles — the strip is a function of the COUNT
   — so the rows are left empty, exactly as the real loader leaves them
   above *n_out.

   flow_chore_load_ret (ESP_OK by default) is the read's verdict. A
   failure is reported the way the real loader reports one: n = 0 and an
   error chore_store_names_known() calls unknown (ESP_FAIL here, a flash
   read error on device). test_chore_store owns which errors are which;
   this suite's stub cannot be reached by mock_nvs_fail_reads(), so the
   knob stands in for it. */
static esp_err_t flow_chore_load_ret;

esp_err_t chore_store_load_names(char names[][CHORE_NAME_BUF], uint8_t *n_out) {
    for (int i = 0; i < CHORE_MAX; i++) {
        names[i][0] = '\0';
    }
    if (flow_chore_load_ret != ESP_OK) {
        *n_out = 0;
        return flow_chore_load_ret;
    }
    *n_out = flow_chore_count;
    return ESP_OK;
}

/* ---- the checklist's four pixels (design §2.5, M2-T8) -------------------

   chores_led_show() is status_led.c's painter and test_status_led owns
   what it PAINTS — the mask-to-colour table, the pixel mapping, the dark
   rows. What this suite owns is WHEN it is called, HOW MANY TIMES, WITH
   WHAT, and WHAT SEPARATES the calls, so the spy records the arguments
   and the delay total of every call rather than only counting them.

   The instant matters as much as the value here, for the same reason it
   does on the binary countdown above: §2.5's ack is a TRANSITION, so a
   flip that arrived at the same instant as the frame it is supposed to
   replace is not an acknowledgement at all, and a plain counter cannot
   see the difference. */
#define FLOW_STRIP_MAX 8
static uint8_t flow_strip_mask[FLOW_STRIP_MAX];
static uint8_t flow_strip_rows[FLOW_STRIP_MAX];
static bool flow_strip_released[FLOW_STRIP_MAX];
static uint32_t flow_strip_at[FLOW_STRIP_MAX];
static int flow_strip_n;

void chores_led_show(uint8_t mask, uint8_t n, bool released) {
    flow_log_push(EV_CHORE_LEDS);
    if (flow_strip_n < FLOW_STRIP_MAX) {
        flow_strip_mask[flow_strip_n] = mask;
        flow_strip_rows[flow_strip_n] = n;
        flow_strip_released[flow_strip_n] = released;
        flow_strip_at[flow_strip_n] = mock_delay_total_ms();
        flow_strip_n++;
    }
}

/* net_window.c's "the checklist owns the strip this wake, drop the sync
   pixel" latch. Counted rather than modelled: what net_window does with
   it is four early returns around neopixel_status_pixel() calls this
   suite does not compile, so the observable part here is that the claim
   is made, and made BEFORE anything can open a window. */
static int flow_led_claims;

void net_window_claim_leds(void) {
    flow_log_push(EV_LED_CLAIM);
    flow_led_claims++;
}

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

void mqtt_ha_queue_summary(const char *date, int32_t screen_used_s, const uint16_t completions[TIMER_EXTRA_SLOTS],
                           uint8_t chores_done, int chores) {
    flow_log_push(EV_QUEUE_SUMMARY);
    flow_summary_date = date;
    flow_summary_used = screen_used_s;
    memcpy(flow_summary_comp, completions, sizeof flow_summary_comp);
    flow_summary_chores_done = chores_done;
    flow_summary_chores = chores;
}

void mqtt_ha_queue_bonus_clear(void) {
    flow_log_push(EV_BONUS_CLEAR);
}

/* The rollover's own window, and the final-minute watch's sharpening
   sync. Steps the clock when a case asks, which is the whole reason `now`
   is an out-param; and can leave the timer somewhere other than RUNNING,
   which is what the reconcile does on device when a config edit lands
   mid-window — the watch must then abandon a countdown that no longer
   exists. -1 (the default) leaves the state alone. */
esp_err_t net_apply_try_window(void) {
    flow_log_push(EV_TRY_WINDOW);
    if (flow_clock_after_window != 0) {
        mock_time_set(flow_clock_after_window);
    }
    if (flow_state_after_window >= 0) {
        flow_state = (timer_state_t)flow_state_after_window;
    }
    if (flow_acked_after_window >= 0) {
        flow_chore_acked = (uint8_t)flow_acked_after_window;
    }
    if (flow_window_join_press != BTN_NONE) {
        flow_press(flow_window_join_press);
        flow_window_join_polled = wake_flow_poll_button_b_action();
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
    /* MODELS THE MEMSET, and that is the whole reason this line is here.
       The real timer_reset() is one `memset(&g_rtc_state, 0, ...)`
       (main/timer.c:343) and `mode` is a field of that struct, so the day
       rollover takes the painted mode back to APP_MODE_TIMERS without
       wake_flow.c writing anything — which is exactly what C13/C16's
       rollover edge rides on, and why APP_MODE_TIMERS has to stay 0.
       A stub that left flow_mode alone would make the rollover case below
       pass only if wake_flow added a redundant explicit clear, i.e. it
       would test for the opposite of what M1-T6 decided. The enumerator's
       value and the memset itself are pinned in test_timer
       (test_a_day_rollover_clears_the_acks_the_release_and_the_mode,
       test_timers_is_the_zero_mode_so_a_zeroed_struct_paints_timers); this
       models the consequence so the wake-flow side can be asked about it.
       Deliberately NOT logged as EV_SET_MODE: nothing called the setter.
       The acks ride the same memset (row C13), so they are cleared here
       too: the summary's chores_done case below relies on a reset that
       really does wipe them. */
    flow_mode = APP_MODE_TIMERS;
    flow_chore_acked = 0;
    mock_time_set(hal_time_now() + 5);
}

void timer_record_date(time_t now) {
    flow_log_push(EV_RECORD_DATE);
    flow_record_date_arg = now;
}

/* ---- the painted mode (C16) --------------------------------------------

   One RTC byte behind an accessor pair (include/timer.h), so the stub is
   the byte. Unclamped on device — timer_set_mode() stores what it is
   given — and unclamped here for the same reason: a stub that bounded the
   value would hide a caller passing something outside the enum. */
app_mode_t timer_mode(void) {
    return flow_mode;
}

void timer_set_mode(app_mode_t mode) {
    flow_log_push(EV_SET_MODE);
    flow_mode = mode;
    flow_set_mode_calls++;
}

/* ---- the injected watch model -------------------------------------------

   The three awake watches are polling loops, so most of what they promise
   is a question of HOW MANY TIMES and IN WHAT ORDER, not of a return
   value. The mock clock is what makes that observable: it only moves
   inside hal_delay_ms(), so a loop's total delay IS the wall time it
   believed it waited, and every "n seconds into the wait" below is
   written as a delay total rather than as a clock the case could set by
   hand. */

static int64_t flow_expiry_wall; /* the active slot's expiry, as timer.c reports it */
static bool flow_needs_sync;
static time_t flow_needs_sync_arg;
static int32_t flow_tick_ret;
static time_t flow_tick_arg;
/* The FIRST tick's clock as well as the last. A path that ends in the
   expiry alert ticks twice — its own, then the post-alarm repaint's,
   which used to be hidden inside a main.c seam this file stubbed — and
   the case that pins "ticked against the clock at the end of the wait"
   is asking about the first one. */
static time_t flow_tick_arg_first;
static time_t flow_break_remaining_arg;

/* What actually reached the panel and the pixels. */
static int32_t flow_partial_seen[8]; /* every partial's remaining_sec, in order */
static time_t flow_partial_at[8];    /* and the wall clock each was assembled against */
static int flow_partial_n;
static int32_t flow_full_remaining;
static time_t flow_full_wall;
static int32_t flow_full_break_remaining;    /* the break countdown that reached the panel */
static int32_t flow_partial_break_remaining; /* ditto, for a partial */
static uint8_t flow_binary_seen[32];         /* every binary countdown value, in order */
static uint32_t flow_binary_at[32];          /* and how far into the wait each was written */
static int flow_binary_n;
static uint8_t flow_binary_rgb[3]; /* the colour of the first one */

/* The two counters below are the pre-sleep event watch's discriminators,
   and they are deliberately NOT in the shared effect log: the end-to-end
   trace cases pin exact event sequences, and a read this common would
   drown them. wake_flow_watch_final_minute()'s very first act is to read
   the expiry wall; wake_flow_watch_break_end()'s is to ask whether a
   break is active. Neither watch touches the other's first read, so on a
   path where nothing else runs these say WHICH branch was taken —
   including the case where a watch declined immediately and left no other
   trace at all. */
static int flow_expiry_wall_reads;
static int flow_break_active_reads;

int64_t timer_expiry_wall(void) {
    flow_expiry_wall_reads++;
    return flow_expiry_wall;
}

bool timer_needs_ntp_sync(time_t now) {
    flow_needs_sync_arg = now;
    return flow_needs_sync;
}

bool timer_break_active(void) {
    flow_break_active_reads++;
    return flow_break_running;
}

/* timer.c's own arithmetic, clamp included: seconds to the break's wall
   end, never negative, and 0 whenever slot 0 is not on a break at all.
   The clamp is load-bearing — the break tail's loop condition is `> 0`,
   so a stub that returned negatives would exit the loop for a different
   reason than the device does. */
int32_t timer_break_remaining(time_t now) {
    flow_break_remaining_arg = now;
    if (!flow_break_running)
        return 0;
    int64_t left = (int64_t)flow_break_wall_end - (int64_t)now;
    return left > 0 ? (int32_t)left : 0;
}

/* timer.c's tick is what DETECTS an expiry: it is the call that moves
   RUNNING -> EXPIRED. Row 10 turns on exactly that — an expiry the
   pre-tick break gate cannot see because the tick has not run yet — so
   the transition has to be modelled rather than set by hand before the
   call. -1 (the default) leaves the state alone, which is every other
   case in this file. */
static int flow_state_after_tick;

/* Opt-in: report the ACTIVE slot's number instead of the single injected
   one. Row 3 is what needs it — "the repaint shows the snapped-back slot"
   is only observable if the number FOLLOWS the selection, which is what
   timer.c does and what the stubbed paint seam used to model directly.
   Off everywhere else, so every other case keeps one fixed value. */
static bool flow_tick_follows_slot;

int32_t timer_tick(time_t now) {
    flow_log_push(EV_TIMER_TICK);
    if (flow_log_count(EV_TIMER_TICK) == 1)
        flow_tick_arg_first = now;
    flow_tick_arg = now;
    if (flow_state_after_tick >= 0) {
        flow_state = (timer_state_t)flow_state_after_tick;
    }
    return flow_tick_follows_slot ? flow_slot_remaining[flow_active_slot] : flow_tick_ret;
}

void neopixel_stop(void) {
    flow_log_push(EV_PIXELS_OFF);
}

/* The instant each write happened is recorded as well as its value. The
   value alone is 250 ms blind: sliding every pixel update to the far side
   of the poll delay shows the same fifteen numbers in the same order, one
   quarter-second late each — which on a countdown whose entire job is to
   be in step with the wall clock is the thing that matters. */
void neopixel_status_binary4(uint8_t value, uint8_t r, uint8_t g, uint8_t b) {
    flow_log_push(EV_PIXELS_BINARY);
    if (flow_binary_n == 0) {
        flow_binary_rgb[0] = r;
        flow_binary_rgb[1] = g;
        flow_binary_rgb[2] = b;
    }
    if (flow_binary_n < (int)(sizeof flow_binary_seen / sizeof flow_binary_seen[0])) {
        flow_binary_at[flow_binary_n] = mock_delay_total_ms();
        flow_binary_seen[flow_binary_n++] = value;
    }
}

/* The state assembly used to be main.c's make_display_state(), stubbed
   here as one function. The audit's review moved it into wake_flow.c, so
   the stub goes down one level instead: the battery ADC below, and
   app_state.c's assembly rules here. Everything the old stub recorded is
   recorded by this one, at the same point in the flow, which is why every
   EV_MAKE_STATE assertion in the file keeps its number.

   What this suite is about is WHICH number was assembled and against
   WHICH clock, which is exactly the pair a wrong remaining or a stale
   clock would corrupt.

   The three break fields are injected rather than derived because the
   tick handler SNAPS st.break_remaining_sec after assembly, and only when
   the assembled state says a break is on the panel (the break screen
   itself, or the chip behind another timer). app_state.c decides those
   two flags on device; here a case sets them directly, which is what
   makes the snap's guard observable in both directions. */
static timer_state_t flow_made_state_kind;
static bool flow_made_break_banner;
static int32_t flow_made_break_remaining;

/* What the assembly was handed. flow_assembled_mv is the ADC value that
   travelled. */
static int flow_assembled_mv;
/* The version string the assembly handed over. The main screen renders it
   on the battery row, so a paint that leaves it NULL blanks the version on
   hardware — which is exactly what shipped once, because this stub used to
   drop the field on the floor. */
static const char *flow_assembled_fw;

/* Cases inject a percentage; the pair below is a FAKE CURVE and not an
   identity, following test_lock_gate. A read that got dropped or an mv
   that got substituted on the way into app_state_in_t then shows up as a
   nonsense number rather than as the right answer by luck. Deliberately
   NOT logged: the two end-to-end cases that pin a whole effect sequence
   would otherwise have to carry an event that says nothing about
   ordering. */
#define FLOW_MV_OFFSET 2500
static int flow_batt_pct;
/* battery.h documents <= 0 mV as a failed ADC read. The OTA gates have to
   tell that apart from a genuinely flat cell, so the suite needs to be
   able to produce it; a case opts in. */
static bool flow_batt_unreadable;

int battery_read_mv(void) {
    return flow_batt_unreadable ? 0 : flow_batt_pct * 10 + FLOW_MV_OFFSET;
}

/* The other half of the fake curve, in test_lock_gate's shape, so that
   dropping either side of battery_percent_from_mv(battery_read_mv())
   yields a nonsense percentage rather than the right answer by luck.

   The clamp is NOT decoration and is not a restatement of the real curve
   either: battery.h documents this function as "0-100, clamped", and that
   clamp is the entire reason ota_batt_pct() has to check the mV for <= 0
   before calling it. A stub that returned a negative number for a failed
   read would make that guard untestable — the wrong answer and the right
   one would both be negative. */
int battery_percent_from_mv(int mv) {
    int pct = (mv - FLOW_MV_OFFSET) / 10;
    if (pct < 0)
        return 0;
    if (pct > 100)
        return 100;
    return pct;
}

display_state_t app_state_display(const app_state_in_t *in, int32_t remaining, time_t now) {
    flow_log_push(EV_MAKE_STATE);
    flow_assembled_mv = in->batt_mv;
    flow_assembled_fw = in->fw_version;
    /* Row 3's pair: the slot the selection was on when the state was
       assembled, and the number that went with it. A paint that runs
       before the drain records the PRE-snap slot and the row-3 case
       fails on the value, not merely on the call sequence. */
    flow_painted_slot = flow_active_slot;
    flow_painted_remaining = remaining;
    display_state_t st;
    memset(&st, 0, sizeof st);
    st.remaining_sec = remaining;
    st.wall_time = now;
    st.timer_state = flow_made_state_kind;
    st.break_banner = flow_made_break_banner;
    st.break_remaining_sec = flow_made_break_remaining;
    /* The two chore fields the emptied-list guard reads, assembled the way
       main/app_state.c's make_display_state() assembles them — its
       `st.app_mode = timer_mode()` and its chore_store_load_names() call,
       named rather than numbered because that file moves: the mode comes
       from the LIVE accessor rather than from a fixture variable of its
       own, and the count from the store. Taking the mode live is what
       makes the guard's
       coupling real here — a guard that reverted the stored byte but handed
       the painter a stale one, or vice versa, shows up as a disagreement
       between flow_mode and flow_painted_mode instead of passing twice. */
    st.app_mode = timer_mode();
    st.chore_count = flow_chore_count;
    st.chore_outstanding = flow_chore_outstanding;
    st.chore_released = flow_chore_released;
    return st;
}

/* THE PANEL COSTS WALL TIME, and until M2-T8's fix pass these two stubs
   modelled it as free — which made an entire class of defect invisible
   here. A refresh holds the CPU with nothing polling: a partial with
   display.c's ghost-clean pass is two partial waveforms back to back, ~0.8 s
   at ~0.4 s each (an estimate, not a board measurement),
   and a full refresh is ~3 s. Design §2.5's "~1.9 s" predates c321ffc,
   which removed a fixed 1.1 s wait between the two passes.
   A press made in that stretch goes into the button latch, and whether
   anything ever takes it out again is a real question about the code under
   test. With the stubs free, the stretch did not exist on the host, the
   press could not be delivered into it (the clock only moves inside
   hal_delay_ms, which is also where the deferred-press hook fires), and
   "the press was discarded" was unfalsifiable. It was in fact discarded.

   0 IS THE DEFAULT, exactly as flow_alert_seconds is 0 by default and for
   the identical reason: several dozen cases in this file measure delay
   totals and paint orderings, and a stub that suddenly spent two seconds
   would rewrite all of them at once. A case that cares opts in, the same
   way it opts into its wake cause. Set it to FLOW_PANEL_COST_MS. */
#define FLOW_PANEL_COST_MS 800 /* a ghost-cleaned partial: two ~0.4 s passes (estimate) */
static uint32_t flow_display_ms;

/* Spent AFTER the event is logged, so the log still reads "the panel got
   it" at the moment the paint was issued, and a press delivered into the
   cost lands after that event rather than before it — which is the order
   on the device, where the latch is an ISR and the refresh is a busy
   flush. Guarded on non-zero: hal_delay_ms(0) still runs the delay hook,
   so an unguarded call would re-arm every existing case's deferred press
   at a new point in the wake. */
/* True only while the stub spends the panel's cost, so a delay hook can
   tell "a press made DURING a paint" from one made while the code polls
   (M2-T15's tail-loop cases). The hook runs at the end of hal_delay_ms,
   inside this bracket. */
static bool flow_painting;

static void flow_display_cost(void) {
    if (flow_display_ms != 0) {
        flow_painting = true;
        hal_delay_ms(flow_display_ms);
        flow_painting = false;
    }
}

void display_full_refresh(const display_state_t *st) {
    flow_log_push(EV_FULL_REFRESH);
    flow_full_remaining = st->remaining_sec;
    flow_full_wall = st->wall_time;
    flow_full_break_remaining = st->break_remaining_sec;
    flow_painted_mode = st->app_mode;
    flow_display_cost();
}

/* Both halves are recorded. The countdown marks are PINNED values, so the
   number alone cannot tell a mark painted on time from the same mark
   painted a second late — the wall clock it was assembled against is the
   only thing that can. */
void display_update(const display_state_t *st) {
    flow_log_push(EV_PARTIAL);
    flow_partial_break_remaining = st->break_remaining_sec;
    flow_painted_mode = st->app_mode;
    if (flow_partial_n < (int)(sizeof flow_partial_seen / sizeof flow_partial_seen[0])) {
        flow_partial_at[flow_partial_n] = st->wall_time;
        flow_partial_seen[flow_partial_n++] = st->remaining_sec;
    }
    flow_display_cost();
}

/* ---- the injected wake-handler model -------------------------------------

   Cycle 11's arrivals. Three of these DO NOT RETURN on device — the deep
   sleep both handlers end in, the bed-time gate they both consult, and
   (already modelled above) the break gate's own bed-time engage — and
   none of them returns here either. A returning stub would let a handler
   run on past the point where the device has stopped, and every "the wake
   ended here" assertion in this file would become unfalsifiable.

   The wake cause and the reset reason arrive from OUTSIDE the firmware
   entirely (the EXT1 decode and the reset controller), so they are plain
   injected values with a read counter each: a guard that never asked is a
   different defect from a guard that asked and decided wrongly, and the
   counters are what tell those apart. */

static esp_reset_reason_t flow_reset_reason;
static int flow_reset_reason_reads;
static button_id_t flow_wakeup_btn;
static int flow_wakeup_btn_reads;
static uint8_t flow_held_now; /* what the pads read at sleep entry */

static bool flow_window_active;
static int flow_stats_collects;
static int flow_stats_posts;

static net_finish_t flow_net_finish;
static int flow_state_after_finish;  /* -1 = the join leaves the state alone */
static int flow_chores_after_finish; /* -1 = the join leaves the list alone; else the row count it leaves */
static time_t flow_last_ntp;

static wake_sleep_mode_t flow_sleep_mode_answer; /* what lock_gate_sleep_mode says */
static wake_sleep_mode_t flow_slept_mode;        /* what enter_deep_sleep was handed */
static int flow_sleeps;
static jmp_buf flow_sleep_jmp;

static time_t flow_bedtime_arg;
static bool flow_bedtime_locks;       /* the gate ends the wake */
static wake_render_t flow_promote_in; /* what lock_gate_promote_render saw */
static int flow_promote_out;          /* -1 = pass through, as an unlocked wake does */

/* M3-T4. `releases`: the gate returns true, a lock let go this wake.
   `window_press`: BTN_NONE, or a press made during the lock's window.
   flow_press is the harness's own, below; the gate stub presses through it. */
static bool flow_gate_releases;
static button_id_t flow_gate_window_press;
static void flow_press(button_id_t btn);

esp_reset_reason_t esp_reset_reason(void) {
    flow_reset_reason_reads++;
    return flow_reset_reason;
}

button_id_t buttons_get_wakeup_button(void) {
    flow_log_push(EV_WAKEUP_BUTTON);
    flow_wakeup_btn_reads++;
    return flow_wakeup_btn;
}

uint8_t buttons_scan_held(void) {
    return flow_held_now;
}

bool net_window_active(void) {
    return flow_window_active;
}

/* The stat-snapshot gather used to be main.c's, stubbed here whole, so
   row 11 could only ever check its ORDER relative to the paint. It is
   wake_flow.c's own static now, which means the stub drops one level —
   onto the four leaves it reads — and the payload becomes checkable.

   Light gets its own fake curve, deliberately not battery's: the two
   fields are adjacent ints in app_state_in_t and an identity stub on both
   would let a swapped pair pass. Different multiplier AND different
   offset, so neither a transposition nor a doubled read of one ADC lands
   on a value the other could have produced. Like battery's, not logged —
   the end-to-end sequence cases assert on ordering, and an ADC read says
   nothing about ordering. */
#define FLOW_LIGHT_OFFSET 300
static int flow_light_pct;
static bool flow_charge_locked;

int light_read_mv(void) {
    return flow_light_pct * 7 + FLOW_LIGHT_OFFSET;
}

bool lock_gate_charge_locked(void) {
    return flow_charge_locked;
}

/* The no-clock lock (BUG-14): the day rollover stands aside for it. */
static bool flow_clock_locked;

bool lock_gate_clock_locked(void) {
    return flow_clock_locked;
}

/* ---- the OTA call sites -------------------------------------------------

   ota_flow.c has its own suite (test_ota_flow) and it asserts the
   SEQUENCE inside an update. What only this file can see is the CALL
   SITES: which wake triggers arm a check and which deliberately do not,
   where in the pre-sleep tail the apply sits relative to the join and the
   sleep, and what facts each of the two is handed. Task 12's plan entry
   said "the module's own tests cannot reach any of them", which is true
   of ota_flow's suite and not of this one — two of the four call sites
   land in wake_flow.c and are pinned below. The other two have no host
   home at all: ota_flow_check's position inside net_window_task (no
   suite exists for that file) and the ops table in app_main. */

static int flow_ota_arms;
static ota_trigger_t flow_ota_trigger; /* the last arm's trigger */
static int flow_ota_arm_batt;
static bool flow_ota_arm_locked;
static int flow_ota_batt_after_arm; /* < 0: the cell does not move */

static bool flow_ota_pending;
static int flow_ota_applies;
static int flow_ota_apply_batt;
static bool flow_ota_apply_locked;

void ota_flow_arm(ota_trigger_t trigger, int batt_pct, bool charge_locked) {
    flow_log_push(EV_OTA_ARM);
    flow_ota_arms++;
    flow_ota_trigger = trigger;
    flow_ota_arm_batt = batt_pct;
    flow_ota_arm_locked = charge_locked;
    /* The cell moving BETWEEN the two windows — minutes and a full-panel
       repaint apart on device — which is the whole reason ota_flow_apply
       takes its facts as arguments instead of reusing what was sampled
       here. Inert unless a case opts in. */
    if (flow_ota_batt_after_arm >= 0) {
        flow_batt_pct = flow_ota_batt_after_arm;
    }
}

bool ota_flow_pending(void) {
    return flow_ota_pending;
}

/* false = the spawn itself failed: no semaphore, or xTaskCreate refused
   the 16 KB stack on a fragmented heap. ota_flow_apply never runs at all
   in that case, which is why the outcome has to be reported from HERE —
   the flow module cannot report what it never saw. */
static bool flow_ota_spawn_ok = true;
static int flow_ota_spawn_failures_noted;

bool ota_task_run_apply(int batt_pct, bool charge_locked) {
    flow_log_push(EV_OTA_APPLY);
    flow_ota_applies++;
    flow_ota_apply_batt = batt_pct;
    flow_ota_apply_locked = charge_locked;
    return flow_ota_spawn_ok;
}

void ota_flow_note_spawn_failed(void) {
    flow_ota_spawn_failures_noted++;
}

/* Distinctive enough that a version arriving from anywhere else — a
   literal, a stale copy, an empty string — is visible as itself rather
   than as a plausible-looking number. */
const esp_app_desc_t *esp_app_get_description(void) {
    static const esp_app_desc_t desc = {.version = "test-fw-9.9.9"};
    return &desc;
}

/* Now the bottom of the gather rather than a stand-in for the whole of
   it: this is where EV_STATS_COLLECT is pushed, so the event still means
   "a snapshot was assembled" and still lands at the same point in every
   sequence — the four reads above it are unlogged, so no ordering case
   moves. What is new is that the assembled struct is captured, which is
   what makes the gather itself testable instead of merely counted. */
static app_state_in_t flow_stats_in;
static time_t flow_stats_now;

void app_state_stats(const app_state_in_t *in, time_t now, stats_snapshot_t *out) {
    flow_log_push(EV_STATS_COLLECT);
    flow_stats_collects++;
    flow_stats_in = *in;
    flow_stats_now = now;
    memset(out, 0, sizeof *out);
}

void net_window_post_snapshot(const stats_snapshot_t *snap) {
    (void)snap;
    flow_log_push(EV_STATS_POST);
    flow_stats_posts++;
}

/* The join. Blocks for seconds on device (the MQTT tail), can move the
   timer underneath the caller — which is what the post-join re-render
   exists for — and either fact is invisible unless the stub models it:
   -1 leaves the state alone, and 0 seconds leaves the clock alone. The
   clock step is load bearing, for the same reason the rollover's is: with
   a frozen clock, "re-read the wall time for the re-render" and "reuse
   the clock the action left behind" pass exactly the same assertions. */
static int32_t flow_finish_seconds;

net_finish_t net_apply_finish(void) {
    flow_log_push(EV_NET_FINISH);
    if (flow_finish_seconds != 0) {
        mock_time_set(hal_time_now() + flow_finish_seconds);
    }
    if (flow_state_after_finish >= 0) {
        flow_state = (timer_state_t)flow_state_after_finish;
    }
    if (flow_chores_after_finish >= 0) {
        flow_chore_count = (uint8_t)flow_chores_after_finish; /* a config payload applied in the window */
    }
    return flow_net_finish;
}

time_t timer_last_ntp_sync(void) {
    return flow_last_ntp;
}

wake_sleep_mode_t lock_gate_sleep_mode(void) {
    return flow_sleep_mode_answer;
}

/* DOES NOT RETURN on device, and does not here: the bed-time gate ends
   the wake by painting a lock screen and sleeping. Shares the landing pad
   with the break gate's own engage, because from a handler's point of
   view they are the same outcome — the wake stopped at bed time. */
bool lock_gate_check_bedtime(time_t now) {
    flow_log_push(EV_CHECK_BEDTIME);
    flow_bedtime_arg = now;
    if (flow_bedtime_locks) {
        flow_bed_engaged = true;
        flow_bed_engage_now = now;
        longjmp(flow_bed_jmp, 1);
    }
    /* A RELEASE, modelled as the real gate leaves it: the lock's own
       window ran with the flag set (so the join poll saw a locked device
       and a press made there is latched), then the flag cleared. */
    if (flow_gate_releases) {
        if (flow_gate_window_press != BTN_NONE) {
            flow_press(flow_gate_window_press); /* made while the lock screen was up */
        }
        flow_sleep_mode_answer = WAKE_SLEEP_NORMAL;
        return true;
    }
    return false;
}

/* A lock released THIS wake owes the panel a full refresh, so the gate
   gets to promote whatever the render policy chose. Pass-through by
   default — which is what the real gate does on every wake that released
   no lock — and an injected answer when a case wants to see the
   promotion arrive. A stub that ALWAYS passed through would make a call
   site that dropped the promotion entirely invisible. */
wake_render_t lock_gate_promote_render(wake_render_t wr) {
    flow_log_push(EV_PROMOTE_RENDER);
    flow_promote_in = wr;
    return flow_promote_out < 0 ? wr : (wake_render_t)flow_promote_out;
}

/* DOES NOT RETURN. Both handlers end in this, and so do the four early
   exits along the way (bed time, the fast-path break, the post-render
   break, the held-through-sleep guard) — so "the wake ended HERE" is the
   single most load-bearing observation in the cases below, and a
   returning stub would erase every one of them. */
void enter_deep_sleep(wake_sleep_mode_t mode) {
    flow_log_push(EV_SLEEP);
    flow_sleeps++;
    flow_slept_mode = mode;
    longjmp(flow_sleep_jmp, 1);
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

   ONE CLOCK FOR PRESSES AND LEVEL SAMPLES ALIKE, stepped by every touch of
   either, because the release gate reads both: a sample and a press close
   enough together have the press rejected as bounce caught by the sample.
   THAT IS THE DEVICE AND NOT THE HARNESS, which is what this paragraph said
   until M2-T12's fix pass and is exactly backwards — the gate anchors
   BUTTON_LATCH_RELEASE_SETTLE_US on the observation, so a real press within
   that of a real level sample really is rejected on real hardware. Calling
   it a harness artefact is what let the default step below hide a field
   defect: while the anchor was a full BUTTON_LATCH_DEBOUNCE_US wide, EVERY
   press within 50 ms of a poll was lost, and no case here could produce a
   press that close because every touch of this clock jumps ten windows.

   SO THE STEP IS A VARIABLE. Coarse by default, so a case that presses the
   same button twice is recording two presses and not measuring the debounce
   (test_button_latch's subject); a case about the release gate sets it to
   something inside the gate's own windows and says why. What the ordering of
   the two calls models faithfully either way — and it is the part the gate is
   about — is whether a press happened before or after somebody looked at the
   pads. */
static int64_t flow_edge_us;
static int64_t flow_edge_gap_us;

#define FLOW_EDGE_GAP_DEFAULT_US (10 * (int64_t)BUTTON_LATCH_DEBOUNCE_US)

static int64_t flow_edge_step(void) {
    flow_edge_us += flow_edge_gap_us;
    return flow_edge_us;
}

static void flow_press(button_id_t btn) {
    button_latch_record((int)btn, flow_edge_step());
}

/* buttons.c's buttons_note_levels(), which every take goes through. */
static void flow_note_levels(void) {
    button_latch_note_levels(flow_held_now, flow_edge_step());
}

/* The mid-wait press. Installed as mock_hal_time's delay hook in setUp(),
   so it fires on the passage of wall time and not on anything the code
   under test does or stops doing. Fires once. */
static void flow_deferred_press_due(void) {
    if (flow_deferred_press_ms < 0 || (int32_t)mock_delay_total_ms() < flow_deferred_press_ms)
        return;
    flow_deferred_press_ms = -1; /* one press, not one per delay */
    flow_deferred_delivered++;
    flow_press(flow_deferred_press_btn);
}

/* What is STILL latched. Consumes, so it is an end-of-case assertion.
   Goes through the latch directly, NOT through the take stubs, so asking
   what survived can never itself manufacture a deferred press. */
static uint8_t flow_latch_residue(void) {
    return button_latch_take();
}

/* THE "it really is latched" IDIOM, which a dozen cases use mid-setup and
   which has to CONSUME the latch in order to observe it — so whatever it
   reports gets pressed again on the next line.

   THE LEVEL SAMPLE IS WHY THIS IS A HELPER and not a bare take: on the
   device every take samples the pads (buttons.c), which is how
   button_latch's release gate learns a button came back up. Without it the
   re-press is an edge on a button nobody has seen released since the press
   this call just took — which is precisely the release BOUNCE the gate
   exists to reject, so the setup would silently stop putting a press back.
   flow_latch_residue() above deliberately does NOT sample: it is an
   end-of-case assertion, and a sample there could arm a gate for nothing. */
static uint8_t flow_latch_take_and_resample(void) {
    const uint8_t taken = button_latch_take();
    flow_note_levels();
    return taken;
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

/* Run a whole wake under landing pads for both of the ways it can stop
   without returning. Reports which one happened, because that IS the
   observation for most of the handler cases: FLOW_WAKE_SLEPT is the
   normal end AND every early exit, FLOW_WAKE_BEDTIME is a lock gate
   taking the wake, and FLOW_WAKE_RAN_OFF_THE_END cannot happen on device
   — both handlers finish with enter_deep_sleep — so a case that reports
   it has found a path that would fall off the end of app_main. */
typedef enum {
    FLOW_WAKE_SLEPT = 0,
    FLOW_WAKE_BEDTIME,
    FLOW_WAKE_RAN_OFF_THE_END,
} flow_wake_result_t;

static flow_wake_result_t flow_run_wake(void (*handler)(void)) {
    /* buttons_init()'s seeding level sample, which runs before either
       handler on device (main.c calls it at boot) and is what tells the
       release gate that the WAKE button is still held — so its own release
       bounce cannot read as a second press. A case that wants that models
       it by setting flow_held_now before the run, exactly as the pads
       would. */
    flow_note_levels();
    if (setjmp(flow_bed_jmp) != 0)
        return FLOW_WAKE_BEDTIME;
    if (setjmp(flow_sleep_jmp) != 0)
        return FLOW_WAKE_SLEPT;
    handler();
    return FLOW_WAKE_RAN_OFF_THE_END;
}

/* Pin the clock for a tick-handler case AND mark the wake as freshly
   synced. The NTP cadence for the clock-only states is "last_sync == 0 ||
   now - last_sync >= interval", and the fixture's epoch clock is an hour
   past any zero, so without this every IDLE/PAUSED/EXPIRED tick case
   would open a network window it never asked for. Opting INTO the sync is
   what the cadence cases below do, by moving flow_last_ntp back. */
static void flow_tick_clock(time_t at) {
    mock_time_set(at);
    flow_last_ntp = at;
}

static flow_wake_result_t flow_run_tick(void) {
    return flow_run_wake(wake_flow_handle_timer_tick);
}

static flow_wake_result_t flow_run_button(void) {
    return flow_run_wake(wake_flow_handle_button_wake);
}

/* The event watch and the two post-action tails are statics inside
   wake_flow.c; this file compiles them in, so a case can drive them
   directly rather than only through a whole wake. finish_or_break can
   sleep (its break gate), so it gets the same landing pads. */
static flow_wake_result_t flow_run_finish_or_break(button_id_t btn, timer_state_t before, time_t now, bool swapped) {
    if (setjmp(flow_bed_jmp) != 0)
        return FLOW_WAKE_BEDTIME;
    if (setjmp(flow_sleep_jmp) != 0)
        return FLOW_WAKE_SLEPT;
    finish_or_break(btn, before, now, swapped);
    return FLOW_WAKE_RAN_OFF_THE_END;
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
    flow_tick_follows_slot = false;
    /* A percentage no real curve endpoint sits on, so an assertion that
       matched a default rather than the injected read stands out. */
    flow_batt_pct = 67;
    flow_batt_unreadable = false;
    flow_assembled_mv = -1;
    flow_assembled_fw = "poison"; /* not the descriptor's string */
    /* Wake-sticky on device (one wake is one boot), so the suite zeroes it
       directly — as test_lock_gate does with the lock flags — rather than
       making wake_flow carry a reset entry point production never calls. */
    s_break_ended = false;
    s_mode_toggled = false;    /* wake-sticky on device, same as s_break_ended */
    s_chore_acked = false;     /* likewise: one wake is one boot */
    s_chore_strip_lit = false; /* likewise: the claim lasts exactly one wake */

    button_latch_reset();
    flow_edge_us = 1000000;
    flow_edge_gap_us = FLOW_EDGE_GAP_DEFAULT_US;
    flow_state = TIMER_IDLE;
    flow_pause_arg = 0;
    /* Default ALLOWED, unlike flow_b_result's inert default: A's refusal
       is the narrow case here (a RUNNING timer or a device with no chore
       list), and every case that cares about the refusal says so. The
       cases that do not care are about the plumbing — the wake mask, the
       pick masks, the ordering — and they need the press to land. */
    flow_a_allowed = true;
    flow_ack_result = BTN_ACK_NONE; /* the shipped fleet: no list, no chore mode */
    flow_ack_idx = -1;              /* poisoned: "never asked" must not read as row 0 */
    flow_ack_now = 0;
    flow_b_result = BTN_B_NONE;
    flow_b_apply_arg = 0;
    flow_slot_reloadable = false;
    flow_reload_ok = true;
    flow_select_ok = true;
    flow_select_slot = FLOW_PIANO;
    flow_state_at_select = TIMER_IDLE;
    flow_net_open = true;
    flow_ntp_ok = true;
    flow_ntp_seconds = 0;
    flow_clock_step = 0;

    /* The break gate. Stored config by default (the getters answer), a
       balance that has NOT crossed, and bed time disabled — so a case
       opts INTO each of the three things that make the gate act. */
    flow_cfg_writes = true;
    flow_interval_min = 30;
    flow_duration_min = 15;
    flow_break_due_ret = false;
    flow_break_due_from = 0; /* the plain injected answer, at every instant */
    flow_bed_min = -1;       /* disabled: bedtime_* treat negative as off */
    flow_alert_dismissed = false;
    flow_alert_seconds = 0;
    flow_bed_engaged = false;
    flow_bed_engage_alert = false;

    /* The day rollover. Not a new day by default, and a restore that
       fails — so the reset path is the one a case has to ask for. */
    flow_new_day = false;
    flow_date = "2026-07-28";
    flow_screen_used = 1234;
    flow_restore_ok = false;
    flow_reset_called = false;

    /* The chore checklist. The shipped default on every device in the
       field: the timer screen, no list configured (row C1). A case opts
       into chore mode and into having chores exactly as it opts into a new
       day above. flow_painted_mode is POISONED rather than zeroed —
       APP_MODE_TIMERS is 0 and is a value a paint can genuinely produce,
       so zeroing it would make "nothing was painted" and "Timers was
       painted" the same assertion. */
    flow_mode = APP_MODE_TIMERS;
    flow_chore_count = 0;
    flow_chore_outstanding = 0;
    flow_chore_released = false;
    /* Nothing ticked and nothing painted. flow_strip_n is the count the
       ordering assertions index into, so it has to be zeroed here or a
       case would read the previous case's frames. */
    flow_chore_acked = 0;
    flow_strip_n = 0;
    flow_ack_n = 0;
    flow_led_claims = 0;
    flow_set_mode_calls = 0;
    flow_painted_mode = (app_mode_t)-1;
    flow_clock_after_window = 0;
    flow_state_after_window = -1;
    flow_window_join_press = BTN_NONE;
    flow_window_join_polled = false;
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

    /* The awake watches. No press queued, no countdown, no stale clock —
       so a case opts into each of those the same way it opts into the
       break gate's three switches above. */
    flow_deferred_press_ms = -1;
    flow_deferred_press_btn = BTN_NONE;
    flow_deferred_delivered = 0;
    /* After mock_time_reset() above, which does not clear it. */
    mock_delay_set_hook(flow_deferred_press_due);
    flow_mask_takes = 0;
    flow_full_takes = 0;
    flow_last_full_take = 0;
    flow_expiry_wall = 0;
    flow_needs_sync = false;
    flow_tick_ret = 0;
    flow_partial_n = 0;
    flow_display_ms = 0;   /* the panel is free unless a case opts into its cost */
    flow_painting = false; /* a failed assertion inside a hook can leave it set */
    flow_binary_n = 0;
    flow_expiry_wall_reads = 0;
    flow_break_active_reads = 0;
    flow_made_state_kind = TIMER_IDLE;
    flow_made_break_banner = false;
    flow_made_break_remaining = 0;

    /* The wake handlers. A deep-sleep tick wake with nothing pending is
       the default, so every case opts INTO its wake cause, its reset
       reason, its open window and its join result the same way the break
       gate's three switches are opted into above. */
    flow_reset_reason = ESP_RST_DEEPSLEEP;
    flow_reset_reason_reads = 0;
    flow_wakeup_btn = BTN_NONE;
    flow_wakeup_btn_reads = 0;
    flow_held_now = 0;
    flow_window_active = false;
    flow_stats_collects = 0;
    flow_stats_posts = 0;
    flow_light_pct = 0;
    flow_charge_locked = false;
    flow_clock_locked = false;

    /* OTA: nothing armed, nothing buffered, a cell that does not move.
       Every case opts into each of the three, the same way it opts into
       its wake cause and its join result. The two recorded fact pairs are
       poisoned rather than zeroed — 0 % is a value the module could
       genuinely produce, so zeroing them would make "never called" and
       "called with a flat battery" the same assertion. */
    flow_ota_arms = 0;
    flow_ota_trigger = (ota_trigger_t)-1;
    flow_ota_arm_batt = -424242;
    flow_ota_arm_locked = true;
    flow_ota_batt_after_arm = -1;
    flow_ota_pending = false;
    flow_ota_applies = 0;
    flow_ota_apply_batt = -424242;
    flow_ota_apply_locked = true;
    flow_ota_spawn_ok = true;
    flow_ota_spawn_failures_noted = 0;
    memset(&flow_stats_in, 0, sizeof flow_stats_in);
    flow_stats_now = 0;
    flow_net_finish = NET_FINISH_IDLE;
    flow_state_after_finish = -1;
    flow_chores_after_finish = -1;
    flow_finish_seconds = 0;
    flow_last_ntp = 0;
    flow_sleep_mode_answer = WAKE_SLEEP_NORMAL;
    flow_sleeps = 0;
    flow_bedtime_locks = false;
    flow_promote_out = -1; /* pass through: no lock was released this wake */
    flow_gate_releases = false;
    flow_gate_window_press = BTN_NONE;
    s_lock_screen_on_glass = false; /* release to repaint on device; one wake is one boot */
    flow_state_after_tick = -1;
    flow_state_at_interrupted = -1;
    /* Wake-sticky on device, like s_break_ended above: one wake is one
       boot, so the suite zeroes the RTC pair directly rather than making
       wake_flow carry a reset entry point production never calls. */
    s_held_mask_at_sleep = 0;
    s_sleep_entry_time = 0;

    /* Poisoned: values the module cannot produce, so "never written" and
       "written with what we expected" can never be the same assertion. */
    flow_shift_arg = -424242;
    flow_delay_at_led = 0xFFFFFFFFu;
    flow_seen_interval_default = 0xFFFFu;
    flow_seen_duration_default = 0xFFFFu;
    flow_break_due_now = -1;
    flow_break_due_interval = -1;
    flow_start_break_now = -1;
    flow_start_break_dur = -1;
    flow_last_alert = (alert_kind_t)-1;
    flow_bed_engage_now = -1;
    flow_new_day_arg = -1;
    flow_screen_used_arg = -1;
    flow_summary_date = NULL;
    flow_summary_used = -424242;
    flow_summary_chores_done = -1;
    flow_summary_chores = -424242; /* not -1: that is STATS_JSON_CHORES_UNKNOWN */
    flow_acked_after_window = -1;
    flow_chore_load_ret = ESP_OK;
    flow_restore_arg = -1;
    flow_record_date_arg = -1;
    flow_needs_sync_arg = -1;
    flow_tick_arg = -1;
    flow_break_remaining_arg = -1;
    flow_full_remaining = -424242;
    flow_full_wall = -1;
    flow_full_break_remaining = -424242;
    flow_partial_break_remaining = -424242;
    flow_slept_mode = (wake_sleep_mode_t)-1;
    flow_bedtime_arg = -1;
    flow_promote_in = (wake_render_t)-1;
    for (int i = 0; i < (int)(sizeof flow_partial_seen / sizeof flow_partial_seen[0]); i++) {
        flow_partial_seen[i] = -424242;
        flow_partial_at[i] = -1;
    }
    for (int i = 0; i < (int)(sizeof flow_binary_seen / sizeof flow_binary_seen[0]); i++) {
        flow_binary_seen[i] = 0xFFu;
        flow_binary_at[i] = 0xFFFFFFFFu;
    }
    flow_binary_rgb[0] = flow_binary_rgb[1] = flow_binary_rgb[2] = 0xFFu;
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
   at `now >= break_expiry_wall`, and wake_flow_watch_break_end()'s loop exits
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
    TEST_ASSERT_EQUAL_INT(1, flow_repaint_count());
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

   The assembly stub records the slot AND its remaining at paint time, so
   the wrong order fails on the value, not merely on the call sequence.
   flow_tick_follows_slot is what makes the number follow the selection —
   the paint is real code now, so the remaining it paints comes through
   the tick rather than out of the seam stub. */
void test_row3_the_repaint_shows_the_snapped_back_slot(void) {
    flow_arm_break(flow_at(16, 0), FLOW_SCREEN, FLOW_PIANO);
    flow_tick_follows_slot = true;
    mock_time_set(flow_at(16, 0) + 3);

    TEST_ASSERT_TRUE(wake_flow_break_end_repaint());

    TEST_ASSERT_EQUAL_INT(1, flow_repaint_count());
    TEST_ASSERT_EQUAL_INT(FLOW_SCREEN, flow_painted_slot);
    TEST_ASSERT_EQUAL_INT32(FLOW_SCREEN_REMAINING, flow_painted_remaining);
    /* and the ordering that produces it */
    TEST_ASSERT_TRUE(flow_log_at(EV_SNAP_BACK) < flow_repaint_at());
}

/* The guaranteed drain runs on every wake that does timer work. A repaint
   without an edge would mean a full e-ink refresh every minute, all day —
   the battery cost the partial cadence exists to avoid. */
void test_a_wake_without_a_break_end_never_repaints(void) {
    mock_time_set(flow_at(14, 0));
    TEST_ASSERT_FALSE(wake_flow_break_end_repaint());
    TEST_ASSERT_EQUAL_INT(0, flow_repaint_count());

    /* and mid-break, which is the other way to reach the drain with no edge */
    flow_arm_break(flow_at(16, 0), FLOW_SCREEN, FLOW_SCREEN);
    mock_time_set(flow_at(15, 30));
    TEST_ASSERT_FALSE(wake_flow_break_end_repaint());
    TEST_ASSERT_EQUAL_INT(0, flow_repaint_count());
}

/* Both sites that repaint reach it after something else may already have
   drained (the break watch drains, then the guaranteed drain runs). The
   second one must not repaint again. */
void test_the_repaint_happens_once_per_edge(void) {
    flow_arm_break(flow_at(16, 0), FLOW_SCREEN, FLOW_PIANO);
    mock_time_set(flow_at(16, 0) + 1);

    TEST_ASSERT_TRUE(wake_flow_break_end_repaint());
    TEST_ASSERT_FALSE(wake_flow_break_end_repaint());

    TEST_ASSERT_EQUAL_INT(1, flow_repaint_count());
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
    TEST_ASSERT_EQUAL_INT(1, flow_repaint_count());
}

/* ---- the two renders that came back out of main.c -----------------------

   make_display_state() and paint_current_state_full() were seams declared
   in wake_flow.h and implemented in main.c on the grounds that the battery
   ADC read in the first has no host answer. That is not one of the four
   residency reasons, and lock_gate.c refutes it by reading the same ADC
   under a host test, so both are wake_flow.c statics now. Being code under
   test rather than stubs, they get cases of their own: what they were only
   trusted to do is now asserted. */

/* The ADC value has to arrive at the assembly unaltered. battery_read_mv()
   is a fake curve here rather than an identity (test_lock_gate's
   precedent), so a read that got dropped or a value that got substituted
   on the way into app_state_in_t shows up as a nonsense number instead of
   as the right answer by luck. */
void test_the_state_assembly_hands_the_battery_read_to_app_state(void) {
    flow_batt_pct = 73;

    display_state_t st = make_display_state(1234, flow_at(15, 0));

    TEST_ASSERT_EQUAL_INT(73 * 10 + FLOW_MV_OFFSET, flow_assembled_mv);
    TEST_ASSERT_EQUAL_INT32(1234, st.remaining_sec);
    TEST_ASSERT_EQUAL_INT64(flow_at(15, 0), st.wall_time);
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_MAKE_STATE));
}

/* The other field the assembly carries. The main screen renders the
   running version on the battery row, so it has to travel on the PAINT
   path, not just the stats path — and it did not:
   make_display_state() left .fw_version implicitly NULL while
   stats_collect() set it, so every main-screen paint on device took the
   empty-version branch and rendered exactly as it had before the feature
   existed. Nothing caught it because this stub recorded only batt_mv.

   Asserted against the descriptor stub's string rather than merely
   non-NULL, so substituting some other string on the way in fails too. */
void test_the_state_assembly_carries_the_firmware_version(void) {
    (void)make_display_state(0, flow_at(15, 0));
    TEST_ASSERT_NOT_NULL(flow_assembled_fw);
    TEST_ASSERT_EQUAL_STRING(esp_app_get_description()->version, flow_assembled_fw);
}

/* ...and the same through a real paint rather than the assembly called
   directly, because make_display_state() is the funnel every render site
   goes through and this is the cheapest one to drive end to end. */
void test_paint_carries_the_firmware_version(void) {
    flow_arm_break(flow_at(16, 0), FLOW_SCREEN, FLOW_PIANO);
    mock_time_set(flow_at(16, 0) + 3);

    TEST_ASSERT_TRUE(wake_flow_break_end_repaint());

    TEST_ASSERT_EQUAL_INT(1, flow_repaint_count());
    TEST_ASSERT_EQUAL_STRING(esp_app_get_description()->version, flow_assembled_fw);
}

/* Nothing but the assembly: a stat read must never transition the state
   machine, and neither must a paint's ingredient list. */
void test_the_state_assembly_neither_ticks_nor_paints(void) {
    (void)make_display_state(60, flow_at(15, 0));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_TIMER_TICK));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_FULL_REFRESH));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PARTIAL));
}

/* The repaint's body, which used to be main.c's three unwatched lines: it
   ticks against a freshly read clock, assembles against that same clock,
   and flushes the result. Ticking after the assembly, or assembling
   against a stale clock, both survive a call-sequence assertion — so the
   number and the wall time are asserted, not just the order. */
void test_the_full_repaint_ticks_then_assembles_then_flushes(void) {
    mock_time_set(flow_at(15, 30));
    flow_tick_ret = 777;

    paint_current_state_full();

    static const flow_event_t expect[] = {EV_TIMER_TICK, EV_MAKE_STATE, EV_FULL_REFRESH};
    TEST_ASSERT_EQUAL_INT((int)(sizeof expect / sizeof expect[0]), flow_log_n);
    for (int i = 0; i < flow_log_n; i++) {
        TEST_ASSERT_EQUAL_INT((int)expect[i], (int)flow_log[i]);
    }
    TEST_ASSERT_EQUAL_INT64(flow_at(15, 30), flow_tick_arg);
    TEST_ASSERT_EQUAL_INT32(777, flow_full_remaining);
    TEST_ASSERT_EQUAL_INT64(flow_at(15, 30), flow_full_wall);
}

/* And it is a FULL refresh, never the partial cadence: the two callers
   reach it exactly when the panel owes a redraw the cadence would skip —
   a break's inverted chip has vanished, or the TIME'S UP screen is still
   up. A partial there leaves the stale layout on the e-ink. */
void test_the_full_repaint_never_goes_partial(void) {
    mock_time_set(flow_at(15, 30));
    paint_current_state_full();
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_FULL_REFRESH));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PARTIAL));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PROMOTE_RENDER)); /* no policy consulted */
}

/* ---- the stat gather, the fourth seam that came out of main.c -----------

   stats_collect() was declared in wake_flow.h and implemented in main.c on
   a residency-3 claim over esp_app_get_description() and
   esp_reset_reason(). Neither is a HANDLE, so the claim never held; and
   main.c never called the function it implemented, so wake_flow.c had to
   reach back up through the header for it. Now a static here.

   Row 11 already covers WHETHER it runs and in what order. What was never
   covered is what it puts in the struct — six fields that main.c's version
   could have transposed, dropped or hard-coded with nothing to catch it.
   Each case below fails on a specific wrong value, not merely on a missing
   call. */

/* Both ADCs, in one case, because the interesting failure is between them:
   batt_mv and light_mv are adjacent ints, so a transposition compiles
   silently. The two fake curves have different multipliers and different
   offsets, which is what makes a swap land on a value neither read could
   have produced. */
void test_the_stat_gather_reads_both_adcs_into_their_own_fields(void) {
    flow_batt_pct = 73;
    flow_light_pct = 41;

    stats_snapshot_t snap;
    stats_collect(&snap);

    TEST_ASSERT_EQUAL_INT(73 * 10 + FLOW_MV_OFFSET, flow_stats_in.batt_mv);
    TEST_ASSERT_EQUAL_INT(41 * 7 + FLOW_LIGHT_OFFSET, flow_stats_in.light_mv);
    TEST_ASSERT_EQUAL_INT(1, flow_stats_collects);
}

/* Both directions: a hard-coded false would pass a one-sided case. */
void test_the_stat_gather_carries_the_charge_lock_either_way(void) {
    stats_snapshot_t snap;

    flow_charge_locked = true;
    stats_collect(&snap);
    TEST_ASSERT_TRUE(flow_stats_in.charge_locked);

    flow_charge_locked = false;
    stats_collect(&snap);
    TEST_ASSERT_FALSE(flow_stats_in.charge_locked);
}

/* The firmware version has to come from the image description. A literal
   or an empty string would satisfy "a string arrived". */
void test_the_stat_gather_publishes_the_image_version(void) {
    stats_snapshot_t snap;
    stats_collect(&snap);
    TEST_ASSERT_NOT_NULL(flow_stats_in.fw_version);
    TEST_ASSERT_EQUAL_STRING("test-fw-9.9.9", flow_stats_in.fw_version);
}

/* The reset reason is not passed through raw — it goes through this
   module's own decode. Asserting the DECODED string is what pins that the
   call is still wrapped; handing app_state the enum would still compile
   into the const char * field on many toolchains, and would publish
   garbage. */
void test_the_stat_gather_publishes_the_decoded_reset_reason(void) {
    flow_reset_reason = ESP_RST_BROWNOUT;

    stats_snapshot_t snap;
    stats_collect(&snap);

    TEST_ASSERT_EQUAL_STRING("BROWNOUT", flow_stats_in.reset_reason);
    TEST_ASSERT_EQUAL_INT(1, flow_reset_reason_reads);
}

/* The one deliberate change made when the body moved: main.c's time(NULL)
   became hal_time_now(). In a host-built TU a raw time(NULL) reads the
   REAL wall clock straight past the suite's injected one, so this case
   fails on a stale-clock regression rather than on a missing call — the
   injected time is a fixed 2020s-era stamp and the real clock is not. */
void test_the_stat_gather_reads_the_injected_clock_not_the_wall_clock(void) {
    mock_time_set(flow_at(15, 30));

    stats_snapshot_t snap;
    stats_collect(&snap);

    TEST_ASSERT_EQUAL_INT64(flow_at(15, 30), flow_stats_now);
}

/* Side-effect free, the property row 11's ordering cases cannot see: a
   stat read must never transition the state machine or touch the panel.
   Same shape as the state assembly's case above. */
void test_the_stat_gather_neither_ticks_nor_paints(void) {
    mock_time_set(flow_at(15, 30));

    stats_snapshot_t snap;
    stats_collect(&snap);

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_TIMER_TICK));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_FULL_REFRESH));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PARTIAL));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_MAKE_STATE));
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
   presses that the tick-wake drain at the end of the wake is supposed to
   act on, and those presses are then discarded at deep sleep — the press
   simply does nothing. Both polls are pinned, because both take, and a
   fix applied to only one of them shipped once already.

   C is the bit that matters to the drain; A and D are pressed alongside
   it so the mask is pinned as "only B", not merely "not C". */

void test_row5_the_pause_poll_consumes_only_the_b_bit(void) {
    flow_state = TIMER_RUNNING;
    flow_press(BTN_A);
    flow_press(BTN_B);
    flow_press(BTN_C);
    flow_press(BTN_D);
    mock_time_set(flow_at(14, 0));

    TEST_ASSERT_TRUE(wake_flow_poll_pause_button());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_PAUSE));
    TEST_ASSERT_EQUAL_HEX8((1u << BTN_A) | (1u << BTN_C) | (1u << BTN_D), flow_latch_residue());
}

/* The refusal path takes too — the take happens before the guard — so the
   mask has to be right on the branch that does nothing, which is the
   overwhelmingly common one (every poll of every grid wait). */
void test_row5_a_refused_pause_poll_still_leaves_the_others_latched(void) {
    flow_state = TIMER_IDLE;
    flow_press(BTN_A);
    flow_press(BTN_B);
    flow_press(BTN_C);
    flow_press(BTN_D);
    mock_time_set(flow_at(14, 0));

    TEST_ASSERT_FALSE(wake_flow_poll_pause_button());

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PAUSE));
    TEST_ASSERT_EQUAL_HEX8((1u << BTN_A) | (1u << BTN_C) | (1u << BTN_D), flow_latch_residue());
}

void test_row5_the_join_poll_consumes_only_the_b_bit(void) {
    flow_b_result = BTN_B_STARTED;
    flow_press(BTN_A);
    flow_press(BTN_B);
    flow_press(BTN_C);
    flow_press(BTN_D);
    mock_time_set(flow_at(14, 0));

    TEST_ASSERT_TRUE(wake_flow_poll_button_b_action());

    TEST_ASSERT_EQUAL_HEX8((1u << BTN_A) | (1u << BTN_C) | (1u << BTN_D), flow_latch_residue());
}

/* The join poll's take runs first and its guard second, so an unmasked
   take here would eat the other buttons on the far more common no-B poll
   — the join-wait poll fires every 100 ms for as long as the MQTT tail
   runs. */
void test_row5_a_join_poll_with_no_b_press_leaves_the_others_latched(void) {
    flow_press(BTN_A);
    flow_press(BTN_C);
    flow_press(BTN_D);
    mock_time_set(flow_at(14, 0));

    TEST_ASSERT_FALSE(wake_flow_poll_button_b_action());

    TEST_ASSERT_EQUAL_HEX8((1u << BTN_A) | (1u << BTN_C) | (1u << BTN_D), flow_latch_residue());
}

/* The B press itself IS consumed on the refusal path — the take is
   unconditional and precedes the state guard. Pinned rather than
   corrected: it is the shipping behaviour, and it is what stops a single
   press from pausing twice when two polls straddle it. */
void test_the_pause_poll_consumes_the_b_press_even_when_it_refuses(void) {
    flow_state = TIMER_IDLE;
    flow_press(BTN_B);
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
        flow_press(BTN_B);
        mock_time_set(flow_at(14, 0));

        TEST_ASSERT_FALSE(wake_flow_poll_pause_button());
        TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PAUSE));
        TEST_ASSERT_EQUAL_INT(idle_states[i], flow_state); /* untouched */
    }

    setUp();
    flow_state = TIMER_RUNNING;
    flow_press(BTN_B);
    mock_time_set(flow_at(14, 0));
    TEST_ASSERT_TRUE(wake_flow_poll_pause_button());
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_PAUSE));
}

void test_the_pause_poll_does_nothing_without_a_b_press(void) {
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
    flow_press(BTN_B);
    mock_time_set(flow_at(15, 47) + 23);

    TEST_ASSERT_TRUE(wake_flow_poll_pause_button());

    TEST_ASSERT_EQUAL_INT64(flow_at(15, 47) + 23, flow_pause_arg);
}

/* One press, one pause: the watches call this every 250 ms, so a poll
   that re-paused on a consumed latch would re-stamp the slot on every
   iteration of the final minute. */
void test_the_pause_poll_pauses_at_most_once_per_press(void) {
    flow_state = TIMER_RUNNING;
    flow_press(BTN_B);
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
void test_the_join_poll_is_inert_without_a_latched_b(void) {
    flow_b_result = BTN_B_STARTED;
    mock_time_set(flow_at(14, 0));

    TEST_ASSERT_FALSE(wake_flow_poll_button_b_action());

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_B_APPLY));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_LED));
    TEST_ASSERT_EQUAL_INT(TIMER_IDLE, flow_state);
}

/* BREAK and EXPIRED stay wake-press-only: the map returns NONE, and the
   poll must then report nothing AND leave the LEDs alone — the pixel
   showing "heard you" when nothing happened is worse than silence. */
void test_the_join_poll_reports_nothing_when_the_state_map_refuses(void) {
    flow_state = TIMER_BREAK;
    flow_b_result = BTN_B_NONE;
    flow_press(BTN_B);
    mock_time_set(flow_at(14, 0));

    TEST_ASSERT_FALSE(wake_flow_poll_button_b_action());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_B_APPLY)); /* it was asked... */
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_LED));     /* ...and said no */
}

void test_the_join_poll_acks_on_the_leds_for_every_accepted_action(void) {
    static const btn_b_action_t accepted[] = {BTN_B_STARTED, BTN_B_RESUMED, BTN_B_PAUSED};
    for (int i = 0; i < (int)(sizeof(accepted) / sizeof(accepted[0])); i++) {
        setUp();
        flow_b_result = accepted[i];
        flow_press(BTN_B);
        mock_time_set(flow_at(14, 0));

        TEST_ASSERT_TRUE(wake_flow_poll_button_b_action());
        TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_LED));
    }
}

/* The panel must stay quiet while the MQTT tail is transmitting — an
   e-ink refresh current alongside a radio TX burst is the brownout this
   whole rendezvous exists to avoid — and the wake has already synced, so
   there is no window to open either. The repaint rides the post-join
   changed-state re-render instead. */
void test_the_join_poll_neither_paints_nor_syncs(void) {
    flow_b_result = BTN_B_STARTED;
    flow_press(BTN_B);
    mock_time_set(flow_at(14, 0));

    TEST_ASSERT_TRUE(wake_flow_poll_button_b_action());

    TEST_ASSERT_EQUAL_INT(0, flow_repaint_count());
    TEST_ASSERT_EQUAL_INT(0, FLOW_RENDERS());
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_NET_OPEN));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_WAIT_NTP));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_SHIFT_EXPIRY));
}

void test_the_join_poll_applies_the_map_at_the_current_clock(void) {
    flow_b_result = BTN_B_STARTED;
    flow_press(BTN_B);
    mock_time_set(flow_at(9, 5) + 41);

    TEST_ASSERT_TRUE(wake_flow_poll_button_b_action());

    TEST_ASSERT_EQUAL_INT64(flow_at(9, 5) + 41, flow_b_apply_arg);
}

/* ---- ROW 18: Button B during a break ----------------------------------- */

/* B is the start/pause button and slot 0 is the break: starting the
   screen timer from inside the break it was sent on defeats the break
   entirely. Refused, logged, and — the part that matters — the state map
   is never even asked, so nothing transitions. */
void test_row18_button_b_during_a_break_is_refused_with_no_side_effects(void) {
    flow_state = TIMER_BREAK;
    flow_b_result = BTN_B_STARTED; /* the map WOULD start it if asked */
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_FALSE(flow_dispatch(BTN_B, flow_at(16, 0), TIMER_BREAK, true));

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_B_APPLY));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_LED));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_NET_OPEN));
    TEST_ASSERT_EQUAL_INT(TIMER_BREAK, flow_state);
    TEST_ASSERT_FALSE(flow_swapped_io);
    TEST_ASSERT_EQUAL_INT64(flow_at(16, 0), flow_now_io); /* clock untouched */
    TEST_ASSERT_EQUAL_UINT32(0, mock_delay_total_ms());   /* no ack hold either */
}

/* THE case the guard must NOT catch, and the reason it could not simply
   ride across with the body it used to guard.

   `before` is the ACTIVE slot's state, and TIMER_BREAK only ever lives on
   slot 0 (timer.c writes it to screen_slot() and nowhere else). So a
   break running behind a SELECTED reloadable extra arrives here as
   TIMER_EXPIRED, walks past the TIMER_BREAK guard, and reaches the map's
   EXPIRED leg — which is where the reload M0-T2 deliberately preserved
   during a break actually happens.

   Broadening the guard to "any break is running" (a timer_break_active()
   read, say) would look like a tidy-up and would silently kill it: the
   expired extra could not be reloaded for the whole break. Pinned here at
   the dispatch, and again end to end in the break tail. */
void test_the_break_guard_does_not_block_a_reload_of_an_expired_extra(void) {
    /* a Screen Break is running on slot 0 the whole time; the selected
       slot is the expired extra, so `before` is EXPIRED, not BREAK */
    flow_state = TIMER_EXPIRED;
    flow_b_result = BTN_B_RELOADED;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_B, flow_at(16, 0), TIMER_EXPIRED, true));

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_B_APPLY)); /* the map WAS reached */
    TEST_ASSERT_EQUAL_INT(TIMER_IDLE, flow_state);
    /* A reload is not a start: no ack hold, no LED and no window of its
       own — the caller owes it the LED and the repaint. */
    TEST_ASSERT_EQUAL_UINT32(0, mock_delay_total_ms());
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_LED));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_NET_OPEN));
    TEST_ASSERT_FALSE(flow_swapped_io);
}

/* The other half of the same fact: slot 0 selected during its own break
   IS `before == TIMER_BREAK`, and that press is refused before the map.
   Screen carries no def, so it could never have reloaded anyway — the
   two arms agree, which is what makes the guard safe rather than lucky. */
void test_a_break_press_on_slot_zero_is_still_refused_before_the_map(void) {
    flow_state = TIMER_BREAK;
    flow_b_result = BTN_B_RELOADED; /* the map would reload if it were asked */
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_FALSE(flow_dispatch(BTN_B, flow_at(16, 0), TIMER_BREAK, true));

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_B_APPLY));
    TEST_ASSERT_EQUAL_INT(TIMER_BREAK, flow_state);
}

/* The `before`-by-value pin, in both directions: the guard reads the
   PARAMETER and never calls timer_get_state() itself.

   Read what this does and does not prove. Both legs below construct a
   divergence by calling flow_dispatch() directly with a `before` that
   contradicts flow_state — production cannot produce one, because every
   caller captures `before` from timer_get_state() on the line before the
   dispatch call. So this pins the SIGNATURE, not a reachable state, and
   it is not evidence that the guard behaves differently from the break
   gate inside button_b_apply() (given a real `before`, the map refuses a
   break press on every leg too).

   Pinned anyway, because the parameter is load bearing for the RENDER
   policy — it is its only account of the timer state the glass was
   painted from, which is what carries a swap across a break, as
   test_row4_a_swap_during_a_break_renders_full_end_to_end proves (the
   layout's other two terms, the mode and the chore count, have records of
   their own — see wake_flow.h) — and a
   "cleanup" that re-read the live state inside the guard would compile,
   pass every other case, and quietly delete that contract. */
void test_row18_the_refusal_reads_the_before_parameter_not_timer_get_state(void) {
    /* painted BREAK, live RUNNING: still refused */
    flow_state = TIMER_RUNNING;
    flow_b_result = BTN_B_PAUSED;
    mock_time_set(flow_at(16, 0));
    TEST_ASSERT_FALSE(flow_dispatch(BTN_B, flow_at(16, 0), TIMER_BREAK, true));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_B_APPLY));

    /* painted RUNNING, live BREAK: NOT refused — the guard is not a
       disguised read of timer_get_state() */
    setUp();
    flow_state = TIMER_BREAK;
    flow_b_result = BTN_B_PAUSED;
    mock_time_set(flow_at(16, 0));
    TEST_ASSERT_TRUE(flow_dispatch(BTN_B, flow_at(16, 0), TIMER_RUNNING, true));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_B_APPLY));
}

/* The refusal is keyed on BREAK alone. An EXPIRED slot the map cannot
   reload — Screen, which carries no def, or a non-reloadable extra —
   reaches the map and is turned down THERE instead (BTN_B_NONE), which is
   a different arm with a different outcome: no ack hold, no LED, but the
   map WAS consulted. Only that case. An EXPIRED *reloadable* slot is not
   refused at all any more: the map reloads it and reports
   BTN_B_RELOADED, which is a real action, pinned by
   test_the_break_tail_treats_a_map_reload_as_a_real_action. */
void test_button_b_on_a_non_reloadable_expired_slot_is_refused_by_the_map(void) {
    flow_state = TIMER_EXPIRED;
    flow_b_result = BTN_B_NONE;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_FALSE(flow_dispatch(BTN_B, flow_at(16, 0) - 30, TIMER_EXPIRED, true));

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_B_APPLY));
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
    flow_b_result = BTN_B_STARTED;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_B, flow_at(16, 0) - 30, TIMER_IDLE, false));

    TEST_ASSERT_EQUAL_INT64(flow_at(16, 0) - 30, flow_b_apply_arg);
}

/* "Hold the pre-press colour briefly so the WHITE/AMBER -> GREEN
   transition is visible as an acknowledgement." The hold is worthless
   after the colour has already changed, so its position is the point,
   not its existence. */
void test_a_start_holds_the_pre_press_colour_before_the_led(void) {
    flow_b_result = BTN_B_STARTED;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_B, flow_at(16, 0), TIMER_IDLE, false));

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_LED));
    /* Against the constant and not against 250: the figure is a menuconfig
       knob (CONFIG_MAGTAG_STATUS_LED_ACK_HOLD_MS) so a board can sweep it,
       and a literal here would turn every sweep into a red suite. */
    TEST_ASSERT_EQUAL_UINT32(STATUS_LED_ACK_HOLD_MS, flow_delay_at_led); /* the hold had already run */
    TEST_ASSERT_EQUAL_UINT32(STATUS_LED_ACK_HOLD_MS, mock_delay_total_ms());
}

/* The ack must be on the pixels before the window blocks for seconds —
   an LED that lights only after the sync settles is not an acknowledgement
   of anything. */
void test_a_start_acks_before_it_opens_the_window(void) {
    flow_b_result = BTN_B_STARTED;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_B, flow_at(16, 0), TIMER_IDLE, true));

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
    flow_b_result = BTN_B_RESUMED;
    flow_ntp_seconds = 7; /* the sync took seven seconds */
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_B, flow_at(16, 0), TIMER_PAUSED, true));

    TEST_ASSERT_EQUAL_INT64(flow_at(16, 0) + 7, flow_now_io);
}

/* Even with no window at all the clock is re-read, because the ack hold
   above can itself cross a second boundary — and one second is the whole
   resolution the panel renders in. */
void test_a_start_without_a_window_still_re_reads_the_clock(void) {
    flow_b_result = BTN_B_STARTED;
    mock_time_set(flow_at(16, 0));
    /* Whatever the poll loop that got here has to have burned for the ack
       hold to complete the second — derived from the hold rather than
       written as 750, so the case keeps testing the boundary crossing when
       the hold is swept from menuconfig instead of silently stopping. */
    hal_delay_ms(1000 - STATUS_LED_ACK_HOLD_MS);

    TEST_ASSERT_TRUE(flow_dispatch(BTN_B, flow_at(16, 0), TIMER_IDLE, false));

    /* that + the ack hold = exactly one second */
    TEST_ASSERT_EQUAL_INT64(flow_at(16, 0) + 1, flow_now_io);
}

void test_a_synced_start_shifts_the_expiry_by_the_measured_step(void) {
    flow_b_result = BTN_B_STARTED;
    flow_ntp_ok = true;
    flow_clock_step = 4;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_B, flow_at(16, 0), TIMER_IDLE, true));

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_SHIFT_EXPIRY));
    TEST_ASSERT_EQUAL_INT64(4, flow_shift_arg);
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_NOTE_UNSYNCED));
}

/* A negative step is the interesting direction: the device clock ran
   fast, so the expiry must move EARLIER. A shift that dropped the sign
   (or took an absolute value) would extend the allocation instead. */
void test_a_backwards_clock_step_shifts_the_expiry_backwards(void) {
    flow_b_result = BTN_B_STARTED;
    flow_ntp_ok = true;
    flow_clock_step = -9;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_B, flow_at(16, 0), TIMER_IDLE, true));

    TEST_ASSERT_EQUAL_INT64(-9, flow_shift_arg);
}

/* The step is a TAKE: reading it twice would hand the second reader a
   zero, and the shift is the only consumer on this path. */
void test_the_measured_step_is_taken_exactly_once(void) {
    flow_b_result = BTN_B_STARTED;
    flow_clock_step = 3;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_B, flow_at(16, 0), TIMER_IDLE, true));

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_TAKE_STEP));
}

/* Fail-open: the timer keeps running on the uncorrected clock. But the
   sync may still settle during the MQTT tail, so the wake has to REMEMBER
   that it painted unsynced or the late step is never applied. */
void test_a_start_that_misses_the_sync_notes_it_and_shifts_nothing(void) {
    flow_b_result = BTN_B_STARTED;
    flow_ntp_ok = false;
    flow_clock_step = 11; /* would be applied if the branch were inverted */
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_B, flow_at(16, 0), TIMER_IDLE, true));

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
    flow_b_result = BTN_B_STARTED;
    flow_net_open = false;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_B, flow_at(16, 0), TIMER_IDLE, true));

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
    flow_b_result = BTN_B_STARTED;
    flow_net_open = true; /* it WOULD open if asked */
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_B, flow_at(16, 0), TIMER_IDLE, false));

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_NET_OPEN));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_WAIT_NTP));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_LED)); /* still acked */
}

/* STARTED and RESUMED share one arm, so both must get the whole
   sequence — an arm that fell through for only one of them would leave a
   resume unsynced and unacknowledged. */
void test_a_resume_takes_exactly_the_same_path_as_a_start(void) {
    static const btn_b_action_t both[] = {BTN_B_STARTED, BTN_B_RESUMED};
    for (int i = 0; i < 2; i++) {
        setUp();
        flow_b_result = both[i];
        flow_ntp_seconds = 2;
        mock_time_set(flow_at(16, 0));

        TEST_ASSERT_TRUE(flow_dispatch(BTN_B, flow_at(16, 0), TIMER_IDLE, true));
        TEST_ASSERT_EQUAL_UINT32(STATUS_LED_ACK_HOLD_MS, flow_delay_at_led);
        TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_LED));
        TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_WAIT_NTP));
        TEST_ASSERT_EQUAL_INT64(flow_at(16, 0) + 2, flow_now_io);
    }
}

/* The pause arm is deliberately bare: nothing to acknowledge visually
   (the caller renders PAUSED immediately), nothing to sync for, and no
   reason to burn the acknowledgement hold on a battery-powered device. */
void test_a_pause_neither_holds_nor_syncs_nor_moves_the_clock(void) {
    flow_state = TIMER_RUNNING;
    flow_b_result = BTN_B_PAUSED;
    mock_time_set(flow_at(16, 0) + 55);

    TEST_ASSERT_TRUE(flow_dispatch(BTN_B, flow_at(16, 0), TIMER_RUNNING, true));

    TEST_ASSERT_EQUAL_UINT32(0, mock_delay_total_ms());
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_LED));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_NET_OPEN));
    TEST_ASSERT_EQUAL_INT64(flow_at(16, 0), flow_now_io); /* caller's clock kept */
    TEST_ASSERT_FALSE(flow_swapped_io);
}

/* ---- ROW 19: Button A, the mode toggle ----------------------------------

   A was the inert arm until M2-T3; it is the Timers/Chores toggle now,
   and these two cases changed with it. What they assert did not: A still
   touches NO TIMER MACHINERY. The mode is a render selector, so an A
   press must never start, pause, reload or swap a slot, never open a
   network window, never hold for an LED ack and never step the caller's
   clock — every one of which would be a fall-through into B's or C's arm
   wearing A's name. The arm's own effect (the stored mode) is asserted in
   the M2-T3 section at the bottom of this file.

   The direct-reset arm that used to live under BTN_B is gone: reset now
   reaches the slot through button_b_apply()'s EXPIRED leg, which is a
   deliberate NARROWING (any non-RUNNING reloadable slot -> EXPIRED only).
   The gate-and-reload pair those cases drove is test_button_actions'
   business now; what remains this file's business is that B's arm reports
   BTN_B_RELOADED as a real action, pinned below and in the break tail. */

void test_row19_button_a_touches_no_timer_machinery(void) {
    /* Every OTHER arm armed to SUCCEED, so a fall-through into any of
       them is loud rather than silent. */
    flow_state = TIMER_IDLE;
    flow_b_result = BTN_B_STARTED;
    flow_slot_reloadable = true;
    flow_reload_ok = true;
    flow_select_ok = true;
    flow_net_open = true;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_A, flow_at(16, 0) - 30, TIMER_IDLE, true));

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_B_APPLY));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_RELOAD_ALLOWED));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_RELOAD));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_SELECT_NEXT));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_LED));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_NET_OPEN));
    TEST_ASSERT_EQUAL_INT(TIMER_IDLE, flow_state);
    TEST_ASSERT_EQUAL_UINT32(0, mock_delay_total_ms());        /* no ack hold */
    TEST_ASSERT_EQUAL_INT64(flow_at(16, 0) - 30, flow_now_io); /* clock untouched */
    TEST_ASSERT_FALSE(flow_swapped_io);
}

/* In every state, not just the convenient one: a leftover guard keyed on
   TIMER_BREAK (the shape A's arm had before the button swap) would change
   what A does in one state and not the others. The gate that DOES depend
   on the state — refused while RUNNING — lives in button_actions.c and is
   injected here, so this sweep is about the arm reaching no slot, not
   about the predicate. */
void test_button_a_touches_no_slot_in_any_state(void) {
    static const timer_state_t states[] = {TIMER_IDLE, TIMER_RUNNING, TIMER_PAUSED, TIMER_EXPIRED, TIMER_BREAK};
    for (unsigned i = 0; i < sizeof states / sizeof states[0]; i++) {
        setUp();
        flow_state = states[i];
        flow_b_result = BTN_B_STARTED;
        flow_slot_reloadable = true;
        flow_reload_ok = true;
        flow_select_ok = true;
        mock_time_set(flow_at(16, 0));

        TEST_ASSERT_TRUE(flow_dispatch(BTN_A, flow_at(16, 0), states[i], true));

        TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_B_APPLY));
        TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_RELOAD));
        TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_SELECT_NEXT));
        TEST_ASSERT_EQUAL_INT((int)states[i], (int)flow_state);
    }
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
        /* every arm arranged to REFUSE, so nothing legitimately sets it.
           A's refusal is its own knob rather than a consequence of
           flow_state: the gate lives in button_actions.c and is injected
           here, so leaving it at setUp's permissive default would let A's
           arm succeed and stop testing the refusal path it is listed for. */
        flow_state = TIMER_RUNNING;
        flow_a_allowed = false;
        flow_b_result = BTN_B_NONE;
        flow_slot_reloadable = false;
        flow_select_ok = false;
        mock_time_set(flow_at(16, 0));

        TEST_ASSERT_FALSE(flow_dispatch(all[i], flow_at(16, 0), TIMER_BREAK, true));
        TEST_ASSERT_FALSE_MESSAGE(flow_swapped_io, "an arm left the selection report unwritten");
    }
}

void test_only_a_successful_swap_reports_a_selection_change(void) {
    /* B's accepting arms, start and reload, are both non-selection */
    flow_b_result = BTN_B_STARTED;
    mock_time_set(flow_at(16, 0));
    TEST_ASSERT_TRUE(flow_dispatch(BTN_B, flow_at(16, 0), TIMER_IDLE, false));
    TEST_ASSERT_FALSE(flow_swapped_io);

    setUp();
    flow_b_result = BTN_B_RELOADED;
    mock_time_set(flow_at(16, 0));
    TEST_ASSERT_TRUE(flow_dispatch(BTN_B, flow_at(16, 0), TIMER_EXPIRED, false));
    TEST_ASSERT_FALSE(flow_swapped_io);

    setUp();
    mock_time_set(flow_at(16, 0));
    TEST_ASSERT_TRUE(flow_dispatch(BTN_C, flow_at(16, 0), TIMER_IDLE, false));
    TEST_ASSERT_TRUE(flow_swapped_io);
}

/* D is the user-facing "refresh everything" button and is handled by the
   wake decode, never here; BTN_NONE reaches this on a spurious EXT1 wake.
   Both must be completely inert — a fall-through into the B arm would
   start a timer on a sync press. */
void test_button_d_and_button_none_are_inert_in_the_dispatch(void) {
    static const button_id_t inert[] = {BTN_D, BTN_NONE};
    for (int i = 0; i < 2; i++) {
        setUp();
        flow_b_result = BTN_B_STARTED;
        flow_slot_reloadable = true;
        flow_select_ok = true;
        mock_time_set(flow_at(16, 0));

        TEST_ASSERT_FALSE(flow_dispatch(inert[i], flow_at(16, 0) - 30, TIMER_IDLE, true));

        TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_B_APPLY));
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

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_B_APPLY));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_LED));
    TEST_ASSERT_EQUAL_INT(0, FLOW_RENDERS());
}

/* Unlike the two single-button polls this one takes the WHOLE latch: it
   is the only consumer left before sleep, so anything it leaves behind is
   discarded anyway. Pinned because the mask is the visible difference
   between this poll and the row-5 pair, and copying their masked take
   here would strand a B or C press for a drain that never comes.

   THE TAKE IS WIDER THAN THE PICK, which is the distinction this case
   exists to hold: every bit is cleared here rather than left pending for
   a consumer that never runs, whatever the pick then chose to act on.
   The earlier wording — "A and D are out of the PICK" — was stale twice
   over: M2-T3 put A into the allowed mask (it loses the pick to B here,
   which is a different fact), and M2-T4b puts D in whenever the mode says
   CHORES. Neither changes what this case asserts, because the take never
   consulted the mask in the first place. */
void test_the_break_tail_poll_drains_the_whole_latch_including_a_and_d(void) {
    flow_state = TIMER_BREAK;    /* B is refused here, so the poll reports false */
    flow_mode = APP_MODE_TIMERS; /* so the pick is the T4a set; the take is not */
    flow_press(BTN_A);
    flow_press(BTN_B);
    flow_press(BTN_C);
    flow_press(BTN_D);
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_FALSE(wake_flow_poll_break_buttons());

    TEST_ASSERT_EQUAL_HEX8(0, flow_latch_residue());
}

/* D's ABSENCE from the allowed mask, IN TIMERS MODE — which is the whole
   of what this case now says, and it used to say more. The comment that
   stood here claimed adding (1u << BTN_D) back changed nothing observable,
   because a lone D then reached the dispatch's outer default arm and was
   refused there. M2-T4b made that false in one direction: the candidates
   are mode-gated now (wake_flow_pick_latched_press), and in CHORE mode a
   lone D is ✓3 and really does act
   (test_c4b_a_latched_d_press_in_the_break_tail_acks_row_3).

   In Timers mode nothing moved, and this case is what pins that half: D
   is not a candidate, so the poll reports false and the press is taken and
   dropped, which is what keeps a rode-in D from buying a network window.
   The mode is setUp's default and is left implicit nowhere else in this
   case — it is set explicitly below for that reason. */
void test_a_lone_d_press_in_the_break_tail_is_consumed_and_ignored(void) {
    flow_state = TIMER_BREAK;
    flow_mode = APP_MODE_TIMERS; /* load bearing as of T4b: in chores D acts */
    flow_press(BTN_D);
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_FALSE(wake_flow_poll_break_buttons());

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_B_APPLY));
    TEST_ASSERT_EQUAL_INT(0, FLOW_RENDERS());
    TEST_ASSERT_EQUAL_HEX8(0, flow_latch_residue());
}

/* MEMBERSHIP, which is a different property from priority. The priority
   case below presses B alongside C and pins that B wins — which says
   nothing about whether C is in the allowed mask at all. Dropping
   (1u << BTN_C) from it leaves every other case in this suite passing
   while silently disabling Button C for the entire break tail: the kid
   cannot walk to another timer during the break, which is the one thing
   this poll was added for. B wins a pick of its own above; this is C's,
   and it is the only reason the mask's C bit is pinned. */
void test_the_break_tail_acts_on_a_lone_button_c_press(void) {
    flow_state = TIMER_PAUSED;
    flow_select_ok = true;
    flow_state_at_select = TIMER_IDLE;
    flow_press(BTN_C);
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(wake_flow_poll_break_buttons());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_SELECT_NEXT));
    TEST_ASSERT_EQUAL_INT(TIMER_IDLE, flow_state); /* the selection really moved */
    /* Two pixels: the poll's own ack, and the one the render lights for
       the resulting state. */
    TEST_ASSERT_EQUAL_INT(2, flow_log_count(EV_LED));
    TEST_ASSERT_EQUAL_INT(1, FLOW_RENDERS());
    TEST_ASSERT_EQUAL_HEX8(0, flow_latch_residue());
}

/* THE case the break guard has to let through, and the reason that guard
   could not simply move across with the body. B on an EXPIRED reloadable
   slot returns BTN_B_RELOADED: the slot really moves (EXPIRED -> IDLE at
   full duration), so the press is a real action and owes the panel a
   repaint. Reported as "nothing happened" it would reset the slot
   silently — the panel keeps showing TIME'S UP and HA keeps the stale
   state until the next tick wake.

   A Screen Break is running throughout: the break lives on slot 0, and
   the SELECTED slot is the expired extra, so `before` arrives as
   TIMER_EXPIRED and the guard (which keys on TIMER_BREAK) never fires.
   That is the whole proof that the moved guard does not block reload
   during a break, and it is why flow_state is EXPIRED here rather than
   BREAK. */
void test_the_break_tail_treats_a_map_reload_as_a_real_action(void) {
    /* the break runs on slot 0 behind a SELECTED extra, so the active
       slot can legitimately be the EXPIRED one the press reloads */
    flow_state = TIMER_EXPIRED;
    flow_b_result = BTN_B_RELOADED;
    flow_press(BTN_B);
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(wake_flow_poll_break_buttons());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_B_APPLY));
    TEST_ASSERT_EQUAL_INT(TIMER_IDLE, flow_state); /* the slot really was reset */
    /* The tail ran: the poll's own ack, plus the one the render lights for
       the resulting state. */
    TEST_ASSERT_EQUAL_INT(2, flow_log_count(EV_LED));
    TEST_ASSERT_EQUAL_INT(1, FLOW_RENDERS());
    TEST_ASSERT_EQUAL_HEX8(0, flow_latch_residue());
}

/* One press per poll, by the latch's own B > C > D > A priority — the
   real button_latch_pick is compiled in, so this is the shipping order.
   B over C is the layout's rule: start/pause/resume is the time-sensitive
   action, and a selection the press did not get to is still on the panel
   for the next press. */
void test_the_break_tail_acts_on_the_highest_priority_latched_press(void) {
    flow_state = TIMER_IDLE;
    flow_b_result = BTN_B_STARTED;
    flow_select_ok = true; /* C would succeed if it were picked */
    flow_state_at_select = TIMER_IDLE;
    flow_press(BTN_B);
    flow_press(BTN_C);
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(wake_flow_poll_break_buttons());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_B_APPLY));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_SELECT_NEXT));
    TEST_ASSERT_EQUAL_INT(1, FLOW_RENDERS());
}

/* And A loses to everything: latched alongside a B press it is dropped
   from the ALLOWED mask entirely, so it cannot win the pick and swallow
   the press next to it. This is the swallowing bug from the other
   direction — button_latch's priority order is the first half, and
   test_pick_priority_b_over_a pins that one. */
void test_the_break_tail_never_lets_a_latched_a_press_swallow_a_b_press(void) {
    flow_state = TIMER_IDLE;
    flow_b_result = BTN_B_STARTED;
    flow_press(BTN_A);
    flow_press(BTN_B);
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(wake_flow_poll_break_buttons());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_B_APPLY));
    TEST_ASSERT_EQUAL_INT(TIMER_RUNNING, flow_state);
    TEST_ASSERT_EQUAL_HEX8(0, flow_latch_residue()); /* the A bit was dropped, not left */
}

/* A lone A press the toggle REFUSES — a timer running behind the break,
   or no chore list configured. It reaches the dispatch (A is in the
   allowed mask now, unlike D above), is turned away there, and is still
   taken out of the latch rather than left pending for a consumer that
   will not come. Nothing is painted: a refused press must not cost the
   refresh, which is the whole reason the wake mask drops A in the same
   two conditions. The HONOURED lone press is the C3 section's
   test_c3_the_break_tail_poll_acts_on_a_lone_button_a_press. */
void test_a_lone_refused_a_press_in_the_break_tail_is_consumed_and_ignored(void) {
    flow_state = TIMER_IDLE;
    flow_a_allowed = false;
    flow_b_result = BTN_B_STARTED; /* would fire loudly on a fall-through into B's arm */
    flow_press(BTN_A);
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_FALSE(wake_flow_poll_break_buttons());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_A_APPLY)); /* it DID reach the dispatch */
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_B_APPLY));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_LED));
    TEST_ASSERT_EQUAL_INT(0, FLOW_RENDERS());
    TEST_ASSERT_EQUAL_HEX8(0, flow_latch_residue());
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

    TEST_ASSERT_EQUAL_INT(1, FLOW_RENDERS());
    /* Off the break screen and onto an extra: the layout inverts, so the
       shipping policy owes a full refresh rather than a partial diff. */
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_FULL_REFRESH));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PARTIAL));
    TEST_ASSERT_EQUAL_INT(FLOW_PIANO, flow_active_slot);
}

/* The window for this wake has already been joined by the time the watch
   runs; a second one here would block the tail for seconds and paint over
   it. This is the allow_net_window=false argument, pinned at the caller. */
void test_the_break_tail_never_opens_a_second_network_window(void) {
    /* the break runs on slot 0 behind a SELECTED extra, so the active
       slot can legitimately be IDLE and A can legitimately start it */
    flow_state = TIMER_IDLE;
    flow_b_result = BTN_B_STARTED;
    flow_net_open = true;
    flow_press(BTN_B);
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(wake_flow_poll_break_buttons());

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_NET_OPEN));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_WAIT_NTP));
}

/* The render is handed the clock the DISPATCH left behind, not the one
   the poll walked in with — the acknowledgement hold alone can cross a
   second boundary, and one second is the panel's whole resolution. Set up
   so it crosses exactly, derived from the hold rather than written as a
   number so that a menuconfig sweep of the hold does not quietly move the
   crossing out from under the case. */
void test_the_break_tail_hands_the_render_the_clock_the_action_left(void) {
    flow_state = TIMER_IDLE;
    flow_b_result = BTN_B_STARTED;
    flow_press(BTN_B);
    mock_time_set(flow_at(16, 0));
    hal_delay_ms(1000 - STATUS_LED_ACK_HOLD_MS); /* what the tail's poll loop has already burned */

    TEST_ASSERT_TRUE(wake_flow_poll_break_buttons());

    /* The render ticks at the clock it was handed and assembles the panel
       against the same instant, so both halves are checked. */
    TEST_ASSERT_EQUAL_INT64(flow_at(16, 0) + 1, flow_tick_arg);
    TEST_ASSERT_EQUAL_INT64(flow_at(16, 0) + 1, flow_partial_at[0]);
}

/* A press the guards refuse changed nothing, so it must not light the
   pixels (which would read as "heard, and done") and must not repaint —
   a full refresh mid-break is seconds of e-ink and a wasted wake. The
   caller also keeps watching, which is what the false return buys. */
void test_a_refused_press_in_the_break_tail_neither_lights_nor_renders(void) {
    flow_state = TIMER_BREAK;
    flow_b_result = BTN_B_STARTED;
    flow_press(BTN_B); /* B during a break: refused by row 18 */
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_FALSE(wake_flow_poll_break_buttons());

    /* The press really was delivered: the take returned the B bit, so the
       dispatch was reached and refused there (row 18). That is what makes
       the three zeroes below a statement about the refusal rather than
       about an empty latch. Asserting flow_full_takes == 1 here instead
       would be vacuous — the break tail takes unconditionally, so the
       count is 1 whether or not anything was pressed. */
    TEST_ASSERT_EQUAL_INT(1, flow_full_takes);
    TEST_ASSERT_EQUAL_HEX8(1u << BTN_B, flow_last_full_take);
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_B_APPLY)); /* refused before the map */
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_LED));
    TEST_ASSERT_EQUAL_INT(0, FLOW_RENDERS());
    TEST_ASSERT_EQUAL_INT(-1, flow_tick_arg); /* the render was never reached */
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
       bearing: a Button C dispatch lights no pixel of its own, so the two
       pixels below are the poll's ack and the render's own. Deleting the
       poll's call leaves exactly one, lit AFTER the state assembly, so
       both assertions catch it rather than passing on a -1. */
    TEST_ASSERT_EQUAL_INT(2, flow_log_count(EV_LED));
    TEST_ASSERT_EQUAL_INT(1, FLOW_RENDERS());
    TEST_ASSERT_TRUE(flow_log_at(EV_LED) < flow_log_at(EV_MAKE_STATE));
}

/* `before` is captured BEFORE the dispatch, so the render still learns
   which layout was on the glass. Capturing it afterwards collapses
   before/after to the same state and every break-tail press goes partial
   across the inversion — regression 1520486 by another route. */
void test_the_break_tail_reports_the_state_that_was_painted_not_the_new_one(void) {
    flow_state = TIMER_BREAK; /* the break screen is what is on the glass */
    flow_state_at_select = TIMER_IDLE;
    flow_press(BTN_C);
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(wake_flow_poll_break_buttons());

    /* The dispatch moved the live state off BREAK... */
    TEST_ASSERT_EQUAL_INT(TIMER_IDLE, timer_get_state());
    /* ...and the render still crossed the inversion, which is only
       possible if `before` was the PAINTED state and not the live one. */
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_FULL_REFRESH));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PARTIAL));

    /* The shipped defect as the negative control: had the poll captured
       `before` after the dispatch, the boundary would be invisible and
       this same render goes partial — the ghost. */
    TEST_ASSERT_EQUAL_INT(WAKE_RENDER_PARTIAL,
                          wake_policy_render(timer_get_state(), timer_get_state(), true, false, true));
}

/* ---- ROW 22: the break gate's config -----------------------------------
   "Break interval configured 0" => no break ever starts. The value is a
   real HA knob, so this is the documented way to switch eye rest off. */

void test_row22_a_zero_interval_never_starts_a_break(void) {
    flow_interval_min = 0;
    flow_break_due_ret = true; /* the balance is over: irrelevant */
    TEST_ASSERT_EQUAL_INT(FLOW_GATE_NO_BREAK, flow_run_break_gate(flow_at(14, 0)));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_START_BREAK));
    flow_assert_nothing_painted();
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
   every device configured with a one-minute interval — which is what a
   bench demo actually uses. Mutation testing is what surfaced it:
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
    flow_assert_nothing_painted();
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
    /* and the paint too, all the way through to the panel: the break
       screen carries the instant the break STARTED, not the live clock. */
    TEST_ASSERT_EQUAL_INT64(flow_at(14, 30), flow_tick_arg);
    TEST_ASSERT_EQUAL_INT64(flow_at(14, 30), flow_full_wall);
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
    int painted = flow_break_paint_at();
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
   caught too. The four in the middle are the break-start paint's own body,
   which wake_flow.c has owned since the residency audit — before that this
   case saw one stubbed event where it now sees the real ones, and the
   ordering inside them was untested anywhere.
   EV_SET_MODE is C16's break-START revert, and its POSITION carries the
   rule: after the break exists, before the paint that has to show it. */
void test_the_started_break_effect_order_is_pinned_end_to_end(void) {
    flow_break_due_ret = true;
    (void)flow_run_break_gate(flow_at(14, 0));
    static const flow_event_t expect[] = {EV_CFG_INTERVAL, EV_CFG_DURATION, EV_BREAK_DUE,  EV_START_BREAK,
                                          EV_SET_MODE,     EV_PERSIST_SAVE, EV_TIMER_TICK, EV_MAKE_STATE,
                                          EV_LED,          EV_FULL_REFRESH, EV_ALERT_BREAK};
    TEST_ASSERT_EQUAL_INT((int)(sizeof expect / sizeof expect[0]), flow_log_n);
    for (int i = 0; i < flow_log_n; i++) {
        TEST_ASSERT_EQUAL_INT((int)expect[i], (int)flow_log[i]);
    }
}

/* THE case the move exists for. The LED is lit BETWEEN the state assembly
   and the flush, so the panel is blue for the whole multi-second e-paper
   refresh — a child watching the screen go inverted sees the light the
   entire time it is redrawing. Lit after the flush instead it would be a
   blink at the end of an unexplained pause, which is the same three calls
   and a different product.
   Kills: status_led_show_timer_state() moved below display_full_refresh().
   Deliberately positional and not a count — a count passes either way. */
void test_the_break_screen_lights_the_led_before_the_flush_not_after(void) {
    flow_break_due_ret = true;
    TEST_ASSERT_EQUAL_INT(FLOW_GATE_STARTED, flow_run_break_gate(flow_at(14, 0)));
    /* all three really happened: a -1 from any of them would make the
       comparisons below pass on effects that never ran */
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_MAKE_STATE));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_LED));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_FULL_REFRESH));
    /* assemble, THEN light, THEN flush */
    TEST_ASSERT_TRUE(flow_log_at(EV_MAKE_STATE) < flow_log_at(EV_LED));
    TEST_ASSERT_TRUE(flow_log_at(EV_LED) < flow_log_at(EV_FULL_REFRESH));
}

/* The other half of the same body: the state is assembled from THIS tick,
   so the break screen shows the number the tick returned rather than a
   value read before the break was started. */
void test_the_break_screen_paints_the_state_this_tick_returned(void) {
    flow_break_due_ret = true;
    flow_tick_ret = 4242;
    TEST_ASSERT_EQUAL_INT(FLOW_GATE_STARTED, flow_run_break_gate(flow_at(14, 0)));
    TEST_ASSERT_TRUE(flow_log_at(EV_TIMER_TICK) < flow_log_at(EV_MAKE_STATE));
    TEST_ASSERT_EQUAL_INT32(4242, flow_full_remaining);
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
    flow_assert_nothing_painted();
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

/* ROW 21 ON A CLOCK THAT WAS NEVER SET (BUG-11). The same crossing as the
   first row-21 case, but on the 1970 calendar a power-on reset leaves
   behind until NTP lands: 19:50 on 1970-01-01, the kind of fake evening a
   US zone reads at boot. This is the only path besides
   lock_gate_check_bedtime() that can raise the bed-time flag, and the
   gate's unset-clock skip holds that flag rather than clearing it. So
   without the guard here a break falling due offline would lock the
   buttons for as long as NTP kept failing. The break starts instead. */
void test_row21_on_an_unset_clock_the_break_starts_and_bed_time_waits(void) {
    const time_t near_epoch = (time_t)19 * 3600 + 50 * 60;
    TEST_ASSERT_FALSE(time_util_clock_plausible(near_epoch));
    flow_break_due_ret = true;
    flow_duration_min = 15;
    flow_bed_min = flow_bed_at(20, 0);
    /* Non-vacuity: the crossing arithmetic alone says "go to bed" here. */
    TEST_ASSERT_TRUE(bedtime_break_would_cross(time_util_minutes_of_day(near_epoch), 15, flow_bed_min));

    TEST_ASSERT_EQUAL_INT(FLOW_GATE_STARTED, flow_run_break_gate(near_epoch));
    TEST_ASSERT_FALSE_MESSAGE(flow_bed_engaged, "an unset clock raised the bed-time flag");
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_START_BREAK));
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
    TEST_ASSERT_TRUE(flow_log_at(EV_ALERT_EXPIRY) < flow_repaint_at());
    TEST_ASSERT_EQUAL_INT(1, flow_repaint_count());
}

void test_the_expiry_alert_runs_the_expiry_alarm_not_the_break_one(void) {
    wake_flow_fire_expiry_alert();
    TEST_ASSERT_EQUAL_INT(ALERT_EXPIRY, flow_last_alert);
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_ALERT_BREAK));
}

/* The last three used to be one event, EV_REPAINT, because the repaint was
   a main.c seam this file stubbed. It is wake_flow.c's own static now, so
   the pin covers its inside as well: the tick that feeds the number, the
   assembly, the flush — and nothing between them, which is also what makes
   flow_repaint_at() able to find it. */
void test_the_expiry_alert_effect_order_is_pinned_end_to_end(void) {
    wake_flow_fire_expiry_alert();
    static const flow_event_t expect[] = {EV_PERSIST_SAVE, EV_TIMESUP,    EV_ALERT_EXPIRY,
                                          EV_TIMER_TICK,   EV_MAKE_STATE, EV_FULL_REFRESH};
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

/* The finished day's chores (M4-T5): the acked chores among the
   configured ones, and the configured count beside them. */
void test_the_summary_counts_the_days_acked_chores(void) {
    time_t now = flow_at(0, 5);
    flow_new_day = true;
    flow_chore_count = 3;
    flow_chore_acked = 0x05; /* chores 1 and 3 */
    wake_flow_handle_day_rollover(&now);
    TEST_ASSERT_EQUAL_INT(2, flow_summary_chores_done);
    TEST_ASSERT_EQUAL_INT(3, flow_summary_chores);
}

/* THE ORDERING, by value rather than by the effect log: the window
   rewrites the acks (a chore-list edit reconciled mid-window) and the
   reset then zeroes them (the stub models the memset), so a capture
   taken anywhere but first would report 0 or the rewritten mask. All
   three were done yesterday, and the summary must say so. */
void test_the_summary_chores_are_read_before_the_window_and_the_reset(void) {
    time_t now = flow_at(0, 5);
    flow_new_day = true;
    flow_restore_ok = false;
    flow_chore_count = 3;
    flow_chore_acked = 0x07;
    flow_acked_after_window = 0x01;
    wake_flow_handle_day_rollover(&now);
    TEST_ASSERT_TRUE(flow_reset_called);
    TEST_ASSERT_EQUAL_HEX8(0, flow_chore_acked); /* the reset did wipe them */
    TEST_ASSERT_EQUAL_INT(3, flow_summary_chores_done);
    TEST_ASSERT_EQUAL_INT(3, flow_summary_chores);
}

/* A stale ack bit above the configured count is not a chore done: the
   RTC byte is stored raw (a three-chore list shortened to two keeps bit
   2), and counting it would report 3 of 2. Same rule as the live
   chores_done. */
void test_the_summary_ignores_ack_bits_above_the_configured_count(void) {
    time_t now = flow_at(0, 5);
    flow_new_day = true;
    flow_chore_count = 2;
    flow_chore_acked = 0x07;
    wake_flow_handle_day_rollover(&now);
    TEST_ASSERT_EQUAL_INT(2, flow_summary_chores_done);
    TEST_ASSERT_EQUAL_INT(2, flow_summary_chores);
}

/* No list configured (row C1, the shipped default): 0 of 0, whatever
   the ack byte holds, and the summary is still queued. */
void test_the_summary_reports_no_chores_without_a_list(void) {
    time_t now = flow_at(0, 5);
    flow_new_day = true;
    flow_chore_count = 0;
    flow_chore_acked = 0x03;
    wake_flow_handle_day_rollover(&now);
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_QUEUE_SUMMARY));
    TEST_ASSERT_EQUAL_INT(0, flow_summary_chores_done);
    TEST_ASSERT_EQUAL_INT(0, flow_summary_chores);
}

/* The list could not be READ (a flash error, chore_store_names_known()
   false): the day's count is unknown, not 0 of 0, so the summary goes
   out with the chore fields marked unknown (stats_json_summary omits
   them) while the rest of the day is still reported. Acks are set so a
   capture that ignored the verdict and counted with n = 0 would still
   report 0 of 0, which this rejects. */
void test_the_summary_marks_the_chores_unknown_when_the_list_cannot_be_read(void) {
    time_t now = flow_at(0, 5);
    flow_new_day = true;
    flow_chore_count = 3;
    flow_chore_acked = 0x07;
    flow_chore_load_ret = ESP_FAIL;
    flow_screen_used = 1200;
    wake_flow_handle_day_rollover(&now);
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_QUEUE_SUMMARY));
    TEST_ASSERT_EQUAL_INT(STATS_JSON_CHORES_UNKNOWN, flow_summary_chores);
    TEST_ASSERT_EQUAL_INT32(1200, flow_summary_used); /* the rest of the day still reported */
}

/* Nothing to report before the first day was ever recorded. */
void test_a_cold_boot_with_no_stored_date_queues_no_summary(void) {
    time_t now = flow_at(0, 5);
    flow_new_day = true;
    flow_date = "";
    wake_flow_handle_day_rollover(&now);
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_QUEUE_SUMMARY));
}

/* BUG-14: the day a power-on without NTP opened is dated by the unset
   clock. It is not a day HA can be told about — its usage was never
   saved, and the restore below replaces it with today's snapshot. Both
   spellings: the epoch's own date, and the one a zone west of UTC
   renders it as. The restore is still asked, and a successful one ends
   the rollover without a reset. */
void test_a_day_an_unset_clock_opened_queues_no_summary_and_still_restores(void) {
    static const char *const placeholders[] = {"1970-01-01", "1969-12-31"};
    for (size_t i = 0; i < sizeof placeholders / sizeof placeholders[0]; i++) {
        setUp();
        time_t now = flow_at(9, 0);
        flow_new_day = true;
        flow_date = placeholders[i];
        flow_screen_used = 1800;
        flow_restore_ok = true;
        wake_flow_handle_day_rollover(&now);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_QUEUE_SUMMARY), placeholders[i]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_PERSIST_RESTORE), placeholders[i]);
        TEST_ASSERT_FALSE_MESSAGE(flow_reset_called, placeholders[i]);
    }
}

/* BUG-14, round-2 review: BEHIND THE NO-CLOCK LOCK THE STAND-IN DAY'S
   MIDNIGHT IS NOT A ROLLOVER. Rolled, its window would run on the
   stand-in (every day-scoped HA command held) and its reset would follow
   the MQTT phase, so a fresh day would inherit the old day's retained
   bonus target with no clear queued. The lock's gate owns that day and
   settles it before its own window's MQTT phase. So: no summary, no
   clear, no update arm, no window, no restore, no reset — whether the
   clock is still unset (the 24 h stand-in midnight) or was set between
   wakes. */
void test_bug14_a_clock_locked_stand_in_day_does_not_roll_over(void) {
    static const time_t instants[] = {86400 + 60, 1785283200 + 9 * 3600};
    for (size_t i = 0; i < sizeof instants / sizeof instants[0]; i++) {
        setUp();
        time_t now = instants[i];
        flow_new_day = true;
        flow_date = "1970-01-01";
        flow_clock_locked = true;
        wake_flow_handle_day_rollover(&now);
        TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_BONUS_CLEAR));
        TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_OTA_ARM));
        TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_TRY_WINDOW));
        TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PERSIST_RESTORE));
        TEST_ASSERT_FALSE(flow_reset_called);
        TEST_ASSERT_EQUAL_INT64(instants[i], now);
    }
    /* A real day under the same flag (not reachable today) still rolls:
       the stand-aside is for the stand-in only. */
    setUp();
    time_t now = flow_at(0, 5);
    flow_new_day = true;
    flow_clock_locked = true;
    wake_flow_handle_day_rollover(&now);
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_TRY_WINDOW));
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

/* BUG-14, cycle-3 review MAJOR-1: A POWER-ON WHOSE ROLLOVER NTP LANDS LATE.
   The window posted its stats on the unset clock, so the snapshot was
   no_clock and mqtt_ha refused the clear and consumed it. The clock is set
   by the join, the snapshot is from an earlier day, and the reset starts a
   fresh day with no lock to release and re-queue the clear. The rollover
   queues it again, after the reset, for the tick's sync block to carry. */
void test_bug14_a_late_synced_power_on_rollover_requeues_the_clear(void) {
    time_t now = 60; /* the unset clock of a power-on */
    mock_time_set(60);
    flow_new_day = true;
    flow_date = "";
    flow_restore_ok = false;
    flow_clock_after_window = flow_at(6, 30);
    wake_flow_handle_day_rollover(&now);
    TEST_ASSERT_EQUAL_INT(2, flow_log_count(EV_BONUS_CLEAR));
    TEST_ASSERT_TRUE(flow_reset_called);
    /* the second one is the fresh day's: queued after the reset */
    int last = -1;
    for (int i = 0; i < flow_log_n; i++) {
        if (flow_log[i] == EV_BONUS_CLEAR) {
            last = i;
        }
    }
    TEST_ASSERT_TRUE(last > flow_log_at(EV_RECORD_DATE));
}

/* ...but not when today's snapshot comes back: the restored day keeps its
   target (owner decision Q-A)... */
void test_bug14_a_late_synced_power_on_restore_does_not_requeue_the_clear(void) {
    time_t now = 60;
    mock_time_set(60);
    flow_new_day = true;
    flow_date = "";
    flow_restore_ok = true;
    flow_clock_after_window = flow_at(6, 30);
    wake_flow_handle_day_rollover(&now);
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_BONUS_CLEAR));
}

/* ...nor when the window never set the clock: the reset recorded the
   stand-in date and the lock's gate owns that day. A clear left pending
   here would outlive a later release that restores today's snapshot. */
void test_bug14_a_power_on_rollover_that_never_syncs_does_not_requeue_the_clear(void) {
    time_t now = 60;
    mock_time_set(60);
    flow_new_day = true;
    flow_date = "";
    flow_restore_ok = false;
    flow_clock_after_window = 0;
    wake_flow_handle_day_rollover(&now);
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_BONUS_CLEAR));
    TEST_ASSERT_TRUE(flow_reset_called);
}

/* A rollover on a set clock published its clear in its own window; a
   second one would drop a target the parent sets for the new day. Exactly
   one (the end-to-end trace below pins the same for the reset branch). */
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
    /* EV_OTA_ARM sits between the bonus clear and the window because the
       update check RIDES that window: ota_flow_check runs on the network
       task and reads what the arm sampled on this one. This trace is what
       makes that a pinned position rather than an incidental one — an arm
       moved after EV_TRY_WINDOW fails here even though every count-based
       assertion in the OTA cases below would still pass. */
    static const flow_event_t expect[] = {EV_IS_NEW_DAY, EV_QUEUE_SUMMARY,   EV_BONUS_CLEAR, EV_OTA_ARM,
                                          EV_TRY_WINDOW, EV_PERSIST_RESTORE, EV_TIMER_RESET, EV_RECORD_DATE};
    TEST_ASSERT_EQUAL_INT((int)(sizeof expect / sizeof expect[0]), flow_log_n);
    for (int i = 0; i < flow_log_n; i++) {
        TEST_ASSERT_EQUAL_INT((int)expect[i], (int)flow_log[i]);
    }
}

/* ---- the render-grid wait -----------------------------------------------

   The wait LENGTH is wake_policy_grid_wait_sec's (compiled in above and
   pinned in its own suite); what belongs here is the polling half — that
   the policy is asked with the right three readings, and that the loop
   that burns the answer stays responsive to Button B for the whole of it.
   Every case reads the wait back through mock_delay_total_ms(), which is
   the only wall clock this suite has. */

void test_the_grid_wait_absorbs_the_residue_of_a_running_countdown(void) {
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 3625; /* 25 s off the round minute */
    wake_flow_wait_for_render_grid(25);
    TEST_ASSERT_EQUAL_UINT32(25000, mock_delay_total_ms());
    TEST_ASSERT_EQUAL_INT64(now + 25, hal_time_now());
}

void test_the_grid_wait_uses_the_break_countdown_when_a_break_is_selected(void) {
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_state = TIMER_BREAK;
    flow_arm_break(now + 190, FLOW_SCREEN, FLOW_SCREEN); /* 10 s off the minute */
    wake_flow_wait_for_render_grid(25);
    TEST_ASSERT_EQUAL_UINT32(10000, mock_delay_total_ms());
    /* asked at the instant the wait started, not at some later re-read */
    TEST_ASSERT_EQUAL_INT64(now, flow_break_remaining_arg);
}

void test_the_grid_wait_lands_a_clock_only_state_on_the_wall_minute(void) {
    /* IDLE/PAUSED/EXPIRED show only the clock, so the grid is the wall
       :00 — and the expiry, which is meaningless here, is never read. */
    time_t now = flow_at(14, 0) + 35;
    mock_time_set(now);
    flow_state = TIMER_PAUSED;
    flow_expiry_wall = (int64_t)now + 3625;
    wake_flow_wait_for_render_grid(25);
    TEST_ASSERT_EQUAL_UINT32(25000, mock_delay_total_ms());
}

void test_a_residue_exactly_at_the_cap_is_waited_out(void) {
    time_t now = flow_at(14, 0) + 35; /* 25 s to the wall minute */
    mock_time_set(now);
    flow_state = TIMER_IDLE;
    wake_flow_wait_for_render_grid(25);
    TEST_ASSERT_EQUAL_UINT32(25000, mock_delay_total_ms());
}

void test_a_residue_one_second_past_the_cap_renders_in_place(void) {
    time_t now = flow_at(14, 0) + 34; /* 26 s to the wall minute */
    mock_time_set(now);
    flow_state = TIMER_IDLE;
    wake_flow_wait_for_render_grid(25);
    TEST_ASSERT_EQUAL_UINT32(0, mock_delay_total_ms());
}

void test_the_cap_the_caller_passes_is_the_one_that_applies(void) {
    /* The 25 in the tick handler is a call-site number, not a constant
       baked in here: the same residue is waited out under a bigger cap
       and abandoned under a smaller one. */
    time_t now = flow_at(14, 0) + 35;
    mock_time_set(now);
    flow_state = TIMER_IDLE;
    wake_flow_wait_for_render_grid(24);
    TEST_ASSERT_EQUAL_UINT32(0, mock_delay_total_ms());
    mock_time_set(now);
    wake_flow_wait_for_render_grid(60);
    TEST_ASSERT_EQUAL_UINT32(25000, mock_delay_total_ms());
}

void test_a_state_already_on_its_grid_never_polls_at_all(void) {
    /* to == 0 means not one poll — which is what leaves a latched press
       to the tick handler's own drain instead of eating it here. */
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_state = TIMER_IDLE;
    flow_press(BTN_B);
    wake_flow_wait_for_render_grid(25);
    TEST_ASSERT_EQUAL_UINT32(0, mock_delay_total_ms());
    TEST_ASSERT_EQUAL_INT(0, flow_mask_takes);
    TEST_ASSERT_EQUAL_UINT8(1u << BTN_B, flow_latch_residue());
}

void test_an_expiry_that_has_already_passed_is_not_waited_on(void) {
    /* The watch/alert path owns a passed event; standing here for 25 s
       first would delay the TIME'S UP by that much. */
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now - 5;
    wake_flow_wait_for_render_grid(25);
    TEST_ASSERT_EQUAL_UINT32(0, mock_delay_total_ms());
}

void test_the_grid_wait_polls_ten_times_a_second(void) {
    /* The cadence is the contract, not just the total: a press must never
       wait longer than ~100 ms to be seen. */
    time_t now = flow_at(14, 0) + 57; /* 3 s to the wall minute */
    mock_time_set(now);
    flow_state = TIMER_IDLE;
    wake_flow_wait_for_render_grid(25);
    TEST_ASSERT_EQUAL_INT(30, flow_mask_takes);
    TEST_ASSERT_EQUAL_UINT32(3000, mock_delay_total_ms());
}

/* ROW 6: Button B during a long grid wait while RUNNING. Buttons are only
   dispatched on EXT1 wake, so before 2d9d61f a press made during this
   wait simply vanished — up to 25 s of a device that looks broken. */
void test_row6_b_press_during_a_long_grid_wait_pauses_at_that_moment(void) {
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 3625; /* a 25 s wait ahead */
    flow_deferred_press_btn = BTN_B;
    flow_deferred_press_ms = 12000; /* pressed twelve seconds in */
    wake_flow_wait_for_render_grid(25);
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_PAUSE));
    TEST_ASSERT_EQUAL_INT(TIMER_PAUSED, (int)flow_state);
    /* Paused THERE — the wait aborts rather than running out its 25 s and
       pausing at the end (which would lose 13 s of the child's time). */
    TEST_ASSERT_EQUAL_UINT32(12000, mock_delay_total_ms());
    TEST_ASSERT_EQUAL_INT64(now + 12, flow_pause_arg);
    TEST_ASSERT_EQUAL_UINT8(0, flow_latch_residue());
}

/* KNOWN DEFECT, PINNED DELIBERATELY — NOT A BLESSING.

   wake_flow_poll_pause_button() consumes the B bit BEFORE it checks the
   state, so a press made while the timer is not RUNNING is swallowed and
   nothing later in the wake can act on it. Through this wait that is up
   to 25 seconds in which Button B does nothing at all — the user-visible
   symptom is "I pressed start and it ignored me".

   It is left exactly as it shipped because this refactor is strictly
   behaviour-preserving and fixing a bug mid-move destroys the ability to
   prove nothing changed; the fix is recorded for its own commit (see the
   long comment in wake_flow_poll_pause_button). When that commit lands
   THIS TEST MUST FAIL — a press should survive to the tick handler's
   drain — and it must be rewritten deliberately, not deleted quietly. */
void test_row6_b_press_while_not_running_is_eaten_KNOWN_BUG(void) {
    time_t now = flow_at(14, 0) + 35;
    mock_time_set(now);
    flow_state = TIMER_PAUSED; /* a B press here means "resume" */
    flow_deferred_press_btn = BTN_B;
    flow_deferred_press_ms = 5000;
    wake_flow_wait_for_render_grid(25);

    /* The press REALLY HAPPENED. Without this the whole case is vacuous:
       every assertion below is equally true of a wait during which nobody
       pressed anything, so deleting the two arming lines above would leave
       it passing. The delay hook arms the press on wall time alone, so
       this holds whatever the code under test does with it. */
    TEST_ASSERT_EQUAL_INT(1, flow_deferred_delivered);
    TEST_ASSERT_EQUAL_UINT32(25000, mock_delay_total_ms()); /* it landed inside the wait */

    /* And it was EATEN: taken out of the latch and thrown away. The pair
       is what makes this a bug report rather than a description — the
       press was delivered (above) and is now gone (below) without having
       done anything at all. */
    TEST_ASSERT_EQUAL_UINT8(0, flow_latch_residue());
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PAUSE));
    TEST_ASSERT_EQUAL_INT(TIMER_PAUSED, (int)flow_state);
    /* The take ran on every poll, which is the mechanism: guard-first
       would have taken nothing and left the press for the tick drain. */
    TEST_ASSERT_EQUAL_INT(250, flow_mask_takes);
}

void test_a_press_latched_before_the_grid_wait_pauses_before_the_first_delay(void) {
    /* The poll runs at the TOP of each pass, not the bottom: a press the
       ISR latched while the wake was still busy (a slow sync, an e-ink
       flush) is acted on immediately rather than after a first 100 ms of
       standing still. Mutation testing found this: moving the poll below
       the delay left every other assertion in this file intact. */
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 3625;
    flow_press(BTN_B);
    wake_flow_wait_for_render_grid(25);
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_PAUSE));
    TEST_ASSERT_EQUAL_UINT32(0, mock_delay_total_ms());
    TEST_ASSERT_EQUAL_INT64(now, flow_pause_arg);
}

void test_the_grid_wait_leaves_the_other_buttons_latched_for_the_tick_drain(void) {
    time_t now = flow_at(14, 0) + 35;
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 3625;
    flow_press(BTN_A);
    flow_press(BTN_C);
    flow_press(BTN_D);
    wake_flow_wait_for_render_grid(25);
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PAUSE));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)((1u << BTN_A) | (1u << BTN_C) | (1u << BTN_D)), flow_latch_residue());
}

/* ---- the break tail ------------------------------------------------------

   Keyed on slot 0, so it covers a break running behind another selected
   timer just as well as the break screen itself. */

void test_the_break_watch_is_inert_when_no_break_is_running(void) {
    mock_time_set(flow_at(14, 0));
    wake_flow_watch_break_end();
    TEST_ASSERT_EQUAL_INT(0, flow_log_n);
    TEST_ASSERT_EQUAL_UINT32(0, mock_delay_total_ms());
}

/* ROW 2, first half: a RUNNING extra suppresses the end outright — no
   chime, no snap, nothing to wait for. */
void test_row2_a_running_extra_leaves_the_break_tail_alone(void) {
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_arm_break(now + 5, FLOW_SCREEN, FLOW_PIANO);
    flow_extra_running = true;
    wake_flow_watch_break_end();
    TEST_ASSERT_EQUAL_INT(0, flow_log_n);
    TEST_ASSERT_EQUAL_UINT32(0, mock_delay_total_ms());
}

void test_a_break_end_beyond_the_watch_window_is_left_to_the_planner(void) {
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_arm_break(now + SLEEP_PLAN_WATCH_SEC + 1, FLOW_SCREEN, FLOW_SCREEN);
    wake_flow_watch_break_end();
    TEST_ASSERT_EQUAL_INT(0, flow_log_n);
    TEST_ASSERT_EQUAL_UINT32(0, mock_delay_total_ms());
}

void test_a_break_end_exactly_at_the_watch_window_is_watched(void) {
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_arm_break(now + SLEEP_PLAN_WATCH_SEC, FLOW_SCREEN, FLOW_SCREEN);
    wake_flow_watch_break_end();
    TEST_ASSERT_EQUAL_UINT32((uint32_t)SLEEP_PLAN_WATCH_SEC * 1000u, mock_delay_total_ms());
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_CHIME));
}

void test_the_break_watch_lights_the_state_before_it_starts_waiting(void) {
    /* The blue pixel is the "still on a break" ack for the whole tail; lit
       after the wait it would only ever be seen for a tick. */
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_arm_break(now + 5, FLOW_SCREEN, FLOW_SCREEN);
    wake_flow_watch_break_end();
    TEST_ASSERT_EQUAL_INT(0, flow_log_at(EV_LED));
    /* The loop itself logs nothing, so a position alone cannot tell "lit
       first" from "lit after the wait" — the delay total at the moment it
       lit can. */
    TEST_ASSERT_EQUAL_UINT32(0, flow_delay_at_led);
    TEST_ASSERT_EQUAL_UINT32(5000, mock_delay_total_ms());
}

void test_a_single_second_of_break_tail_is_still_watched(void) {
    /* brem == 1 is the smallest tail there is; it still gets the pixel and
       still gets waited out, rather than falling through to the drain as
       an already-ended break would. */
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_arm_break(now + 1, FLOW_SCREEN, FLOW_SCREEN);
    wake_flow_watch_break_end();
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_LED));
    TEST_ASSERT_EQUAL_UINT32(1000, mock_delay_total_ms());
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_CHIME));
}

void test_the_break_watch_waits_out_the_tail_then_chimes_and_repaints(void) {
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_arm_break(now + 5, FLOW_PIANO, FLOW_SCREEN);
    wake_flow_watch_break_end();
    TEST_ASSERT_EQUAL_UINT32(5000, mock_delay_total_ms());
    TEST_ASSERT_EQUAL_INT64(now + 5, hal_time_now());
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_CHIME));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_SNAP_BACK));
    TEST_ASSERT_EQUAL_INT(1, flow_repaint_count());
    /* the repaint shows the slot the snap landed on, not slot 0 */
    TEST_ASSERT_EQUAL_INT(FLOW_PIANO, flow_painted_slot);
}

void test_a_break_whose_end_has_already_passed_repaints_without_waiting(void) {
    /* brem == 0: the tail is over before it began, so there is nothing to
       stay awake for — but the edge still owes the panel a repaint. */
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_arm_break(now, FLOW_SCREEN, FLOW_SCREEN);
    wake_flow_watch_break_end();
    TEST_ASSERT_EQUAL_UINT32(0, mock_delay_total_ms());
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_LED));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_CHIME));
    TEST_ASSERT_EQUAL_INT(1, flow_repaint_count());
}

void test_the_break_watch_polls_four_times_a_second(void) {
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_arm_break(now + 2, FLOW_SCREEN, FLOW_SCREEN);
    wake_flow_watch_break_end();
    TEST_ASSERT_EQUAL_INT(8, flow_full_takes);
    TEST_ASSERT_EQUAL_UINT32(2000, mock_delay_total_ms());
}

/* ROW 1: Button C pressed during the final seconds of a break. Before
   1f954da the tail spun with no poll at all and every press made during
   it was thrown away at deep sleep — "I couldn't move to another timer in
   the final minute of the screen break". */
void test_row1_button_c_in_the_break_tail_moves_the_selection_and_ends_the_watch(void) {
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_arm_break(now + 20, FLOW_SCREEN, FLOW_SCREEN);
    flow_state = TIMER_BREAK; /* the break screen itself is showing */
    flow_select_ok = true;
    flow_select_slot = FLOW_PIANO;
    flow_state_at_select = TIMER_IDLE;
    flow_deferred_press_btn = BTN_C;
    flow_deferred_press_ms = 3000;
    wake_flow_watch_break_end();
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_SELECT_NEXT));
    TEST_ASSERT_EQUAL_INT(FLOW_PIANO, flow_active_slot);
    /* repainted through the shared post-action render, which crossed the
       break-screen inversion because `before` was still the layout on the
       glass — a partial here is the ghost regression */
    TEST_ASSERT_EQUAL_INT(1, FLOW_RENDERS());
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_FULL_REFRESH));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PARTIAL));
    /* and the watch stopped there: it did not sit out the last 17 s. The
       render's own early drain ticked once and found nothing (the end is
       still 17 s out), so the break is left running for the planner. */
    TEST_ASSERT_EQUAL_UINT32(3000, mock_delay_total_ms());
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_BREAK_TICK));
    TEST_ASSERT_TRUE(flow_break_running);
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_CHIME));
    TEST_ASSERT_EQUAL_INT(0, flow_repaint_count());
}

/* ROW 2, second half: Button B on a break-eligible extra. C walked the
   selection over during the break; A starts it, and the running extra is
   then exactly what suppresses the break end. */
void test_row2_button_b_on_a_break_eligible_extra_starts_it_and_suppresses_the_end(void) {
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_arm_break(now + 20, FLOW_SCREEN, FLOW_PIANO);
    flow_state = TIMER_IDLE; /* Piano, selected but not started */
    flow_b_result = BTN_B_STARTED;
    flow_deferred_press_btn = BTN_B;
    flow_deferred_press_ms = 2000;
    wake_flow_watch_break_end();
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_B_APPLY));
    TEST_ASSERT_EQUAL_INT(TIMER_RUNNING, (int)flow_state);
    TEST_ASSERT_EQUAL_INT(1, FLOW_RENDERS());
    /* 2000 ms of tail, then the dispatch's own hold on the pre-press
       colour — the watch stopped at the press and did not sit out the
       remaining 18 s. The hold is a menuconfig figure, so it is named
       rather than added in. */
    TEST_ASSERT_EQUAL_UINT32(2000 + STATUS_LED_ACK_HOLD_MS, mock_delay_total_ms());
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_CHIME));
    /* Suppressed from here on: the started extra is what the top-of-watch
       guard reads, so re-entering the watch has nothing left to wait for
       and the end will pass silently at the next tick wake. */
    TEST_ASSERT_TRUE(timer_any_extra_running());
    flow_log_n = 0;
    wake_flow_watch_break_end();
    TEST_ASSERT_EQUAL_INT(0, flow_log_n);
    TEST_ASSERT_EQUAL_UINT32(2000 + STATUS_LED_ACK_HOLD_MS, mock_delay_total_ms()); /* unchanged: it returned */
}

void test_a_press_latched_before_the_break_tail_is_serviced_before_the_first_delay(void) {
    /* The poll runs at the TOP of each pass, not the bottom — the same
       property as the grid wait, and for a sharper reason here: on a tail
       of a second or two, a poll that only ran after the first 250 ms
       would service the press late, and on the shortest tail of all would
       never run before the break ended and chimed over it. That is row 1's
       own symptom ("I couldn't move to another timer in the final minute
       of the screen break") coming back through the side door.

       Button C rather than A: the start/resume arm holds the pre-press
       colour for STATUS_LED_ACK_HOLD_MS, which would muddy the very number
       under test. */
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_arm_break(now + 20, FLOW_SCREEN, FLOW_SCREEN);
    flow_state = TIMER_BREAK;
    flow_select_ok = true;
    flow_select_slot = FLOW_PIANO;
    flow_state_at_select = TIMER_IDLE;
    flow_press(BTN_C);
    wake_flow_watch_break_end();
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_SELECT_NEXT));
    TEST_ASSERT_EQUAL_INT(1, FLOW_RENDERS());
    TEST_ASSERT_EQUAL_UINT32(0, mock_delay_total_ms());
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_CHIME));
}

void test_a_refused_press_in_the_break_tail_does_not_end_the_watch(void) {
    /* Button B on the break screen itself is refused by the guard matrix,
       so the tail carries on to its own end rather than being cut short
       by a press that changed nothing. */
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_arm_break(now + 3, FLOW_SCREEN, FLOW_SCREEN);
    flow_state = TIMER_BREAK;
    flow_deferred_press_btn = BTN_B;
    flow_deferred_press_ms = 1000;
    wake_flow_watch_break_end();
    /* The press was genuinely delivered — it reached the dispatch, which
       refused it — so the watch carrying on is about the refusal and not
       about a latch that stayed empty. */
    TEST_ASSERT_EQUAL_INT(1, flow_deferred_delivered);
    TEST_ASSERT_EQUAL_UINT32(3000, mock_delay_total_ms());
    TEST_ASSERT_EQUAL_INT(0, FLOW_RENDERS());
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_CHIME));
    TEST_ASSERT_EQUAL_INT(1, flow_repaint_count());
}

/* ---- the final minute ----------------------------------------------------

   The RUNNING tail owns the last ~75 s: countdown partials, the binary
   LED count, the pause poll, the mid-watch break check, and the expiry
   alert at zero. */

void test_the_final_minute_watch_is_inert_once_the_expiry_has_passed(void) {
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now; /* remaining == 0 */
    wake_flow_watch_final_minute();
    TEST_ASSERT_EQUAL_INT(0, flow_log_n);
    TEST_ASSERT_EQUAL_UINT32(0, mock_delay_total_ms());
    TEST_ASSERT_EQUAL_INT(0, flow_full_takes); /* not even the entry discard */
}

void test_one_second_of_countdown_is_still_the_final_minute(void) {
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 1;
    wake_flow_watch_final_minute();
    TEST_ASSERT_EQUAL_UINT32(1000, mock_delay_total_ms());
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_ALERT_EXPIRY));
}

void test_a_countdown_beyond_the_watch_window_is_left_to_the_planner(void) {
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + SLEEP_PLAN_WATCH_SEC + 1;
    wake_flow_watch_final_minute();
    TEST_ASSERT_EQUAL_INT(0, flow_log_n);
    TEST_ASSERT_EQUAL_UINT32(0, mock_delay_total_ms());
}

void test_a_countdown_exactly_at_the_watch_window_is_watched(void) {
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + SLEEP_PLAN_WATCH_SEC;
    wake_flow_watch_final_minute();
    TEST_ASSERT_EQUAL_UINT32((uint32_t)SLEEP_PLAN_WATCH_SEC * 1000u, mock_delay_total_ms());
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_ALERT_EXPIRY));
}

void test_the_final_minute_watch_discards_presses_made_before_it_started(void) {
    /* A resume with <70 s left flows straight into this watch; a stale edge
       left by the very press that resumed would otherwise re-pause it
       instantly. Only presses made DURING the watch may pause. ("Stale
       edge" rather than "release bounce" since M2-T12 — see
       test_the_wake_press_stale_edge_is_drained_before_the_tail.) */
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 2;
    flow_press(BTN_B);
    wake_flow_watch_final_minute();
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PAUSE));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_ALERT_EXPIRY));
    TEST_ASSERT_EQUAL_UINT8(0, flow_latch_residue());
}

void test_a_stale_clock_with_room_to_spare_sharpens_the_expiry_first(void) {
    /* Expiry is a wall time, so a clock step here directly sharpens the
       moment the alert fires — but only when the sync (~5-9 s) fits. */
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 16;
    flow_needs_sync = true;
    wake_flow_watch_final_minute();
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_TRY_WINDOW));
    TEST_ASSERT_EQUAL_INT64(now, flow_needs_sync_arg);
    TEST_ASSERT_TRUE(flow_log_at(EV_TRY_WINDOW) < flow_log_at(EV_LED));
}

void test_fifteen_seconds_left_is_too_little_room_to_sync(void) {
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 15;
    flow_needs_sync = true;
    wake_flow_watch_final_minute();
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_TRY_WINDOW));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_ALERT_EXPIRY));
}

void test_a_fresh_clock_needs_no_sync_however_much_room_there_is(void) {
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 70;
    flow_needs_sync = false;
    wake_flow_watch_final_minute();
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_TRY_WINDOW));
}

void test_a_sync_that_moved_the_timer_out_of_running_ends_the_watch(void) {
    /* The window's reconcile can reset or expire the timer (a config edit
       landing mid-sync); the alert, if there was one, has already fired
       inside the window. Watching a countdown that no longer exists would
       double-fire it. */
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 40;
    flow_needs_sync = true;
    flow_state_after_window = TIMER_IDLE;
    wake_flow_watch_final_minute();
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_TRY_WINDOW));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_LED));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_ALERT_EXPIRY));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PARTIAL));
    TEST_ASSERT_EQUAL_UINT32(0, mock_delay_total_ms());
}

void test_the_final_minute_watch_lights_the_state_before_it_counts_down(void) {
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 60;
    wake_flow_watch_final_minute();
    TEST_ASSERT_TRUE(flow_log_at(EV_LED) >= 0);
    TEST_ASSERT_TRUE(flow_log_at(EV_LED) < flow_log_at(EV_CFG_INTERVAL));
    TEST_ASSERT_TRUE(flow_log_at(EV_LED) < flow_log_at(EV_PARTIAL));
}

void test_the_countdown_steps_at_the_quarter_minute_marks(void) {
    /* Pinned values, not the live remaining: the text has to read exactly
       00:01:00 / 00:00:45 / 00:00:30 / 00:00:15. */
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 60;
    wake_flow_watch_final_minute();
    TEST_ASSERT_EQUAL_INT(4, flow_partial_n);
    TEST_ASSERT_EQUAL_INT32(60, flow_partial_seen[0]);
    TEST_ASSERT_EQUAL_INT32(45, flow_partial_seen[1]);
    TEST_ASSERT_EQUAL_INT32(30, flow_partial_seen[2]);
    TEST_ASSERT_EQUAL_INT32(15, flow_partial_seen[3]);
    TEST_ASSERT_EQUAL_UINT32(60000, mock_delay_total_ms());
}

void test_a_late_entry_skips_the_marks_it_has_already_passed(void) {
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 40; /* the 60 and 45 marks are gone */
    wake_flow_watch_final_minute();
    TEST_ASSERT_EQUAL_INT(2, flow_partial_n);
    TEST_ASSERT_EQUAL_INT32(30, flow_partial_seen[0]);
    TEST_ASSERT_EQUAL_INT32(15, flow_partial_seen[1]);
}

void test_each_countdown_mark_is_painted_against_a_freshly_read_clock(void) {
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 20;
    wake_flow_watch_final_minute();
    /* The 15 s mark is painted at the instant it is reached — five seconds
       into the watch — and against the clock read AT that instant, not the
       one the watch walked in with. The value alone cannot say that: it is
       a pinned 15 either way. */
    TEST_ASSERT_EQUAL_INT(1, flow_partial_n);
    TEST_ASSERT_EQUAL_INT32(15, flow_partial_seen[0]);
    TEST_ASSERT_EQUAL_INT64(now + 5, flow_partial_at[0]);
    TEST_ASSERT_EQUAL_UINT32(20000, mock_delay_total_ms());
}

void test_a_wake_one_second_late_does_not_repaint_the_sixty_second_mark(void) {
    /* 59 s left: the 00:01:00 mark has already gone by, and painting it
       anyway would put a number on the panel that is a second in the past
       — which is the whole reason the first step is chosen by policy
       rather than assumed to be zero. */
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 59;
    wake_flow_watch_final_minute();
    TEST_ASSERT_EQUAL_INT(3, flow_partial_n);
    TEST_ASSERT_EQUAL_INT32(45, flow_partial_seen[0]);
    TEST_ASSERT_EQUAL_INT64(now + 14, flow_partial_at[0]);
    TEST_ASSERT_EQUAL_INT32(30, flow_partial_seen[1]);
    TEST_ASSERT_EQUAL_INT32(15, flow_partial_seen[2]);
}

void test_the_last_fifteen_seconds_ride_the_pixels_as_a_binary_count(void) {
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 16; /* one second above the threshold */
    wake_flow_watch_final_minute();
    TEST_ASSERT_EQUAL_INT(15, flow_binary_n); /* 15 down to 1, once each */
    for (int i = 0; i < 15; i++) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(15 - i), flow_binary_seen[i]);
        /* On the second, not a poll later: the write happens before the
           delay that ends the pass, so the pixels change as the number
           does. */
        TEST_ASSERT_EQUAL_UINT32((uint32_t)(i + 1) * 1000u, flow_binary_at[i]);
    }
    /* light green; the driver scales it by the configured brightness */
    TEST_ASSERT_EQUAL_UINT8(20, flow_binary_rgb[0]);
    TEST_ASSERT_EQUAL_UINT8(60, flow_binary_rgb[1]);
    TEST_ASSERT_EQUAL_UINT8(20, flow_binary_rgb[2]);
}

void test_the_binary_count_is_written_once_per_second_not_once_per_poll(void) {
    /* Four polls to the second: a pixel write on every one of them would
       be four times the RMT traffic for the same picture. */
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 4;
    wake_flow_watch_final_minute();
    TEST_ASSERT_EQUAL_INT(4, flow_binary_n);
    TEST_ASSERT_EQUAL_INT(16, flow_mask_takes);
}

void test_the_expiry_alert_fires_when_the_countdown_reaches_zero(void) {
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 3;
    wake_flow_watch_final_minute();
    /* the tick that moves RUNNING -> EXPIRED comes first, against the
       clock at the end of the wait, and the alert sequence follows it.
       Two ticks, not one: the second is the alert's own post-alarm
       repaint, which was a stubbed main.c seam until it moved into
       wake_flow.c — hence the FIRST tick's clock below. */
    TEST_ASSERT_EQUAL_INT(2, flow_log_count(EV_TIMER_TICK));
    TEST_ASSERT_EQUAL_INT(1, flow_repaint_count());
    TEST_ASSERT_EQUAL_INT64(now + 3, flow_tick_arg_first);
    TEST_ASSERT_TRUE(flow_log_at(EV_TIMER_TICK) < flow_log_at(EV_PERSIST_SAVE));
    TEST_ASSERT_TRUE(flow_log_at(EV_PERSIST_SAVE) < flow_log_at(EV_TIMESUP));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_ALERT_EXPIRY));
}

void test_a_pause_press_in_the_final_minute_repaints_and_cancels_the_expiry(void) {
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 30;
    flow_tick_ret = 1234;
    flow_deferred_press_btn = BTN_B;
    flow_deferred_press_ms = 5000;
    wake_flow_watch_final_minute();
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_PAUSE));
    TEST_ASSERT_EQUAL_UINT32(5000, mock_delay_total_ms());
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_ALERT_EXPIRY));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_TIMESUP));
    /* The whole exit sequence, in order and with nothing between: pause,
       pixels cleared, tick, state assembled against a freshly read clock,
       LED, flush. The LED sits BETWEEN the assembly and the refresh so
       the panel is amber for the whole of the multi-second flush rather
       than only after it — a pair of positions could not say that; the
       contiguous tail can. */
    static const flow_event_t tail[] = {EV_PAUSE, EV_PIXELS_OFF, EV_TIMER_TICK, EV_MAKE_STATE, EV_LED, EV_FULL_REFRESH};
    const int n_tail = (int)(sizeof tail / sizeof tail[0]);
    TEST_ASSERT_TRUE(flow_log_n >= n_tail);
    for (int i = 0; i < n_tail; i++) {
        TEST_ASSERT_EQUAL_INT((int)tail[i], (int)flow_log[flow_log_n - n_tail + i]);
    }
    TEST_ASSERT_EQUAL_INT64(now + 5, flow_tick_arg);
    TEST_ASSERT_EQUAL_INT32(1234, flow_full_remaining);
    TEST_ASSERT_EQUAL_INT64(now + 5, flow_full_wall);
}

void test_the_final_minute_watch_polls_four_times_a_second(void) {
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 2;
    wake_flow_watch_final_minute();
    TEST_ASSERT_EQUAL_INT(8, flow_mask_takes);
    TEST_ASSERT_EQUAL_UINT32(2000, mock_delay_total_ms());
}

/* ROW 9: a break that becomes due INSIDE the final-minute watch. The
   per-wake check has already passed by then, so before f2008da the break
   was silently swallowed by the expiry; 54357fa is why the break screen
   that follows has to reach the panel rather than being dropped by the
   driver's refresh-rate guard. */
void test_row9_a_break_due_inside_the_final_minute_paints_it_and_ends_the_watch(void) {
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 60;
    flow_break_due_from = now + 20; /* the balance crosses mid-watch */
    wake_flow_watch_final_minute();
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_START_BREAK));
    TEST_ASSERT_EQUAL_INT(1, flow_break_paint_count());
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_ALERT_BREAK));
    /* started at the instant it came due, not at the end of the minute */
    TEST_ASSERT_EQUAL_INT64(now + 20, flow_start_break_now);
    TEST_ASSERT_EQUAL_UINT32(20000, mock_delay_total_ms());
    /* The binary-countdown pixels are cleared before the break screen —
       and exactly once. A count of 0 would make the position comparison
       below pass on an effect that never happened (flow_log_at returns
       -1); a count of 2 would mean the balance was asked against a clock
       that runs ahead of the one the gate then uses, clearing the pixels
       on a pass that starts nothing. */
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_PIXELS_OFF));
    TEST_ASSERT_TRUE(flow_log_at(EV_PIXELS_OFF) < flow_break_paint_at());
    /* and the expiry, still 40 s away, was neither fired nor ticked past.
       The single tick in the log is the break paint's OWN — the device
       always did it, and this case only read as zero while
       paint_break_started() was a stub here rather than wake_flow.c's
       code. It runs after the break has started and against the instant
       the break started, which is 40 s short of the expiry, so it is not
       the watch ticking past the end. */
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_ALERT_EXPIRY));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_TIMESUP));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_TIMER_TICK));
    TEST_ASSERT_TRUE(flow_log_at(EV_START_BREAK) < flow_log_at(EV_TIMER_TICK));
    TEST_ASSERT_EQUAL_INT64(now + 20, flow_tick_arg);
}

void test_row9_the_balance_is_asked_on_every_poll_not_only_at_entry(void) {
    /* The whole point of f2008da: the check lives INSIDE the loop. */
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 20;
    wake_flow_watch_final_minute();
    TEST_ASSERT_EQUAL_INT(80, flow_log_count(EV_BREAK_DUE)); /* 20 s at 250 ms */
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_START_BREAK));
    /* nothing due: the expiry is still the thing that happens */
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_ALERT_EXPIRY));
}

void test_row9_the_break_interval_is_read_once_before_the_loop(void) {
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 20;
    wake_flow_watch_final_minute();
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_CFG_INTERVAL));
    TEST_ASSERT_TRUE(flow_log_at(EV_CFG_INTERVAL) < flow_log_at(EV_BREAK_DUE));
    /* in seconds, from the stored minutes */
    TEST_ASSERT_EQUAL_INT32(30 * 60, flow_break_due_interval);
}

void test_row9_a_zero_break_interval_is_never_asked_of_the_balance(void) {
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 5;
    flow_interval_min = 0;     /* eye-rest breaks disabled */
    flow_break_due_ret = true; /* would fire the instant it was asked */
    wake_flow_watch_final_minute();
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_BREAK_DUE));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_START_BREAK));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_ALERT_EXPIRY));
}

void test_the_watch_falls_back_to_the_compile_time_break_interval(void) {
    /* NVS never stored one: the caller's pre-seeded default has to be
       what the balance is judged against, not zero (which would disable
       the mid-watch check entirely). */
    time_t now = flow_at(14, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 5;
    flow_cfg_writes = false;
    wake_flow_watch_final_minute();
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_BREAK_INTERVAL_MIN, flow_seen_interval_default);
    TEST_ASSERT_EQUAL_INT32((int32_t)NVS_DEFAULT_BREAK_INTERVAL_MIN * 60, flow_break_due_interval);
}

/* ---- CYCLE 11: the post-action render -----------------------------------

   render_action_result is a static inside wake_flow.c now, so these drive
   it directly. Its four arguments are only observable THROUGH the
   shipping wake_policy_render(), so every case below is written as a
   discriminating pair or with an inline negative control: the assertion
   is not "the render was told X" but "the panel got what X implies, and
   not-X would have produced something else". */

void test_the_post_action_render_drains_the_break_before_the_tick(void) {
    time_t now = flow_at(15, 0);
    mock_time_set(now);
    flow_arm_break(now - 5, FLOW_PIANO, FLOW_SCREEN); /* elapsed, interrupted Piano */

    render_action_result(BTN_B, TIMER_IDLE, now, false);

    /* The edge really was there to drain — otherwise "drained first" is a
       statement about nothing. */
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_CHIME));
    TEST_ASSERT_TRUE(flow_log_at(EV_BREAK_TICK) < flow_log_at(EV_TIMER_TICK));
    TEST_ASSERT_TRUE(flow_log_at(EV_SNAP_BACK) < flow_log_at(EV_TIMER_TICK));
    /* and the tick that feeds the paint therefore sees the snapped-back
       slot, which is the whole point of draining early */
    TEST_ASSERT_EQUAL_INT(FLOW_PIANO, flow_active_slot);
}

void test_the_post_action_render_ticks_at_the_clock_it_was_handed(void) {
    mock_time_set(flow_at(15, 30)); /* the live clock, deliberately different */

    render_action_result(BTN_B, TIMER_IDLE, flow_at(15, 0), false);

    TEST_ASSERT_EQUAL_INT64(flow_at(15, 0), flow_tick_arg);
    TEST_ASSERT_EQUAL_INT(1, FLOW_RENDERS());
    TEST_ASSERT_EQUAL_INT64(flow_at(15, 0), flow_partial_at[0]);
}

/* Button D is the user-facing "refresh everything" button. The policy
   would have chosen a partial for this exact state pair, so the pair
   below is what proves force_full is doing the work. */
void test_button_d_forces_a_full_refresh_the_policy_made_partial(void) {
    mock_time_set(flow_at(15, 0));
    flow_state = TIMER_IDLE;

    render_action_result(BTN_D, TIMER_IDLE, flow_at(15, 0), false);

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_FULL_REFRESH));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PARTIAL));
}

void test_the_same_state_pair_under_any_other_button_stays_partial(void) {
    mock_time_set(flow_at(15, 0));
    flow_state = TIMER_IDLE;

    render_action_result(BTN_B, TIMER_IDLE, flow_at(15, 0), false);

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_FULL_REFRESH));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_PARTIAL));
}

void test_a_transition_into_expired_alerts_instead_of_painting(void) {
    mock_time_set(flow_at(15, 0));
    flow_state = TIMER_EXPIRED;

    render_action_result(BTN_B, TIMER_RUNNING, flow_at(15, 0), false);

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_ALERT_EXPIRY));
    /* the alert owns the panel and the pixels: no paint and no LED of the
       render's own — the one full refresh in the log is the alert's own
       post-alarm repaint, which is why that one is subtracted here and
       asserted separately below */
    TEST_ASSERT_EQUAL_INT(0, FLOW_RENDER_FLUSHES());
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PARTIAL));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_LED));
    TEST_ASSERT_EQUAL_INT(1, flow_repaint_count());
}

/* Row 20's suppression, carried through the render rather than asserted
   against the policy directly: landing on an already-EXPIRED slot by a
   swap must not replay that timer's alarm. */
void test_a_reported_swap_onto_an_expired_slot_paints_and_never_alerts(void) {
    mock_time_set(flow_at(15, 0));
    flow_state = TIMER_EXPIRED;

    render_action_result(BTN_C, TIMER_RUNNING, flow_at(15, 0), true);

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_ALERT_EXPIRY));
    TEST_ASSERT_EQUAL_INT(1, FLOW_RENDERS());
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_PARTIAL));
}

/* Button D's full refresh does not outrank TIME'S UP: the alert arm is
   checked first and returns without painting at all. */
void test_button_d_still_yields_to_the_expiry_alert(void) {
    mock_time_set(flow_at(15, 0));
    flow_state = TIMER_EXPIRED;

    render_action_result(BTN_D, TIMER_RUNNING, flow_at(15, 0), false);

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_ALERT_EXPIRY));
    TEST_ASSERT_EQUAL_INT(0, FLOW_RENDER_FLUSHES()); /* the alert's repaint is not D's */
}

/* The pixel is instant; the e-ink flush behind it takes seconds. Lighting
   it after the flush makes every press feel dead for that whole time. */
void test_the_post_action_render_lights_the_state_before_it_flushes(void) {
    mock_time_set(flow_at(15, 0));
    flow_state = TIMER_IDLE;

    render_action_result(BTN_D, TIMER_IDLE, flow_at(15, 0), false);

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_LED));
    TEST_ASSERT_TRUE(flow_log_at(EV_LED) < flow_log_at(EV_FULL_REFRESH));
}

/* A break that ended this wake is sticky, and every render after it owes
   the panel a full refresh — the inverted chip has gone. Paired with the
   partial case above, which is the same state pair without the flag. */
void test_a_break_that_ended_this_wake_forces_the_post_action_render_full(void) {
    time_t now = flow_at(15, 0);
    mock_time_set(now);
    flow_arm_break(now - 1, FLOW_SCREEN, FLOW_SCREEN);
    flow_extra_running = true; /* silent end: no chime, no snap */
    flow_state = TIMER_IDLE;

    render_action_result(BTN_B, TIMER_IDLE, now, false);

    TEST_ASSERT_TRUE(wake_flow_break_ended_this_wake()); /* the edge was real */
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_FULL_REFRESH));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PARTIAL));
}

/* ---- CYCLE 11, ROW 11: stats posted only after the paint completes ------

   f96962d: the e-ink refresh overlapping the MQTT tail browned out the
   rail. The snapshot rendezvous doubles as power serialization — the
   network task holds the MQTT phase until the orchestrator posts, and the
   orchestrator posts only once the panel has finished drawing.

   These are ORDERING cases. flow_log_at() returns -1 for an event that
   never happened, so every comparison is guarded by a presence assertion
   on BOTH sides: a bare `a < b` would pass on a -1 and say nothing. */

void test_row11_the_stats_are_posted_only_after_the_paint_completes(void) {
    mock_time_set(flow_at(15, 0));
    flow_state = TIMER_IDLE;
    flow_window_active = true;

    finish_action_and_render(BTN_D, TIMER_IDLE, flow_at(15, 0), false);

    /* both really happened */
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_FULL_REFRESH));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_STATS_POST));
    /* and in that order */
    TEST_ASSERT_TRUE(flow_log_at(EV_FULL_REFRESH) < flow_log_at(EV_STATS_POST));
}

void test_row11_a_partial_paint_also_completes_before_the_stats_go_out(void) {
    mock_time_set(flow_at(15, 0));
    flow_state = TIMER_IDLE;
    flow_window_active = true;

    finish_action_and_render(BTN_B, TIMER_IDLE, flow_at(15, 0), false);

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_PARTIAL));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_STATS_POST));
    TEST_ASSERT_TRUE(flow_log_at(EV_PARTIAL) < flow_log_at(EV_STATS_POST));
}

/* The alert path paints too — TIME'S UP, the alarm, then the main layout
   through the repaint seam — and all of it has to be behind us before the
   radio is let go. */
void test_row11_the_whole_expiry_alert_completes_before_the_stats_go_out(void) {
    mock_time_set(flow_at(15, 0));
    flow_state = TIMER_EXPIRED;
    flow_window_active = true;

    finish_action_and_render(BTN_B, TIMER_RUNNING, flow_at(15, 0), false);

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_ALERT_EXPIRY));
    TEST_ASSERT_EQUAL_INT(1, flow_repaint_count());
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_STATS_POST));
    TEST_ASSERT_TRUE(flow_log_at(EV_ALERT_EXPIRY) < flow_log_at(EV_STATS_POST));
    TEST_ASSERT_TRUE(flow_repaint_at() < flow_log_at(EV_STATS_POST));
}

void test_row11_the_snapshot_is_collected_before_it_is_posted(void) {
    mock_time_set(flow_at(15, 0));
    flow_window_active = true;

    finish_action_and_render(BTN_B, TIMER_IDLE, flow_at(15, 0), false);

    TEST_ASSERT_EQUAL_INT(1, flow_stats_collects);
    TEST_ASSERT_EQUAL_INT(1, flow_stats_posts);
    TEST_ASSERT_TRUE(flow_log_at(EV_STATS_COLLECT) < flow_log_at(EV_STATS_POST));
}

/* The release is what lets the network task open its MQTT session, so the
   join must come after it or the rendezvous deadlocks on device. */
void test_row11_the_join_runs_after_the_mqtt_phase_is_released(void) {
    mock_time_set(flow_at(15, 0));
    flow_window_active = true;

    finish_action_and_render(BTN_B, TIMER_IDLE, flow_at(15, 0), false);

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_STATS_POST));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_NET_FINISH));
    TEST_ASSERT_TRUE(flow_log_at(EV_STATS_POST) < flow_log_at(EV_NET_FINISH));
}

/* No window: nothing is collected and nothing is posted. The render
   assertion is the positive control — the tail still ran, so the two
   zeroes are about the window guard and not about a call that never
   happened. */
void test_a_tail_with_no_window_open_collects_and_posts_nothing(void) {
    mock_time_set(flow_at(15, 0));
    flow_window_active = false;

    finish_action_and_render(BTN_B, TIMER_IDLE, flow_at(15, 0), false);

    TEST_ASSERT_EQUAL_INT(1, FLOW_RENDERS());
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_NET_FINISH));
    TEST_ASSERT_EQUAL_INT(0, flow_stats_collects);
    TEST_ASSERT_EQUAL_INT(0, flow_stats_posts);
}

/* ---- CYCLE 11: the post-join re-render ---------------------------------- */

void test_a_quiet_join_renders_exactly_once(void) {
    mock_time_set(flow_at(15, 0));
    flow_net_finish = NET_FINISH_IDLE;

    finish_action_and_render(BTN_B, TIMER_IDLE, flow_at(15, 0), false);

    TEST_ASSERT_EQUAL_INT(1, FLOW_RENDERS());
}

void test_a_join_that_reports_a_change_renders_again(void) {
    mock_time_set(flow_at(15, 0));
    flow_net_finish = NET_FINISH_CHANGED;

    finish_action_and_render(BTN_B, TIMER_IDLE, flow_at(15, 0), false);

    TEST_ASSERT_EQUAL_INT(2, FLOW_RENDERS());
}

/* The other half of the condition: a join that reports nothing but moved
   the timer anyway (a Button B action landing during it) still owes the
   panel a repaint. */
void test_a_quiet_join_that_moved_the_timer_renders_again(void) {
    mock_time_set(flow_at(15, 0));
    flow_state = TIMER_IDLE;
    flow_net_finish = NET_FINISH_IDLE;
    flow_state_after_finish = TIMER_RUNNING;

    finish_action_and_render(BTN_B, TIMER_IDLE, flow_at(15, 0), false);

    TEST_ASSERT_EQUAL_INT(2, FLOW_RENDERS());
}

/* An alerted join has already handled the display in full. Set up so the
   OTHER half of the condition would fire — the state moved — which is
   what makes this a statement about NET_FINISH_ALERTED winning. */
void test_an_alerted_join_never_re_renders_even_when_the_state_moved(void) {
    mock_time_set(flow_at(15, 0));
    flow_state = TIMER_IDLE;
    flow_net_finish = NET_FINISH_ALERTED;
    flow_state_after_finish = TIMER_RUNNING;

    finish_action_and_render(BTN_B, TIMER_IDLE, flow_at(15, 0), false);

    TEST_ASSERT_EQUAL_INT(TIMER_RUNNING, flow_state); /* the move really happened */
    TEST_ASSERT_EQUAL_INT(1, FLOW_RENDERS());
}

/* `painted` is sampled AFTER the first render and BEFORE the join, so it
   names the state that is actually on the glass. */
void test_the_re_render_compares_against_what_was_painted(void) {
    mock_time_set(flow_at(15, 0));
    flow_state = TIMER_PAUSED;
    flow_net_finish = NET_FINISH_IDLE;
    flow_state_after_finish = TIMER_EXPIRED;

    finish_action_and_render(BTN_B, TIMER_PAUSED, flow_at(15, 0), false);

    /* PAUSED (painted) -> EXPIRED (after the join) is a real transition,
       and the re-render passes selection_changed = false, so it alerts.
       Had it passed true, the alert would be suppressed — which is the
       negative control below. */
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_ALERT_EXPIRY));
    TEST_ASSERT_EQUAL_INT(WAKE_RENDER_PARTIAL, wake_policy_render(TIMER_PAUSED, TIMER_EXPIRED, true, false, true));
}

void test_the_re_render_reads_the_wall_clock_afresh(void) {
    mock_time_set(flow_at(15, 0));
    flow_state = TIMER_IDLE;
    flow_net_finish = NET_FINISH_CHANGED;
    flow_finish_seconds = 9; /* the MQTT tail took nine seconds */

    finish_action_and_render(BTN_B, TIMER_IDLE, flow_at(15, 0), false);

    TEST_ASSERT_EQUAL_INT(2, FLOW_RENDERS());
    TEST_ASSERT_EQUAL_INT64(flow_at(15, 0), flow_partial_at[0]);     /* the paint */
    TEST_ASSERT_EQUAL_INT64(flow_at(15, 0) + 9, flow_partial_at[1]); /* the re-paint */
    TEST_ASSERT_EQUAL_INT64(flow_at(15, 0) + 9, flow_tick_arg);      /* and its tick */
}

void test_the_re_render_drains_the_break_before_its_own_tick(void) {
    time_t now = flow_at(15, 0);
    mock_time_set(now);
    flow_state = TIMER_IDLE;
    flow_net_finish = NET_FINISH_CHANGED;
    /* the break's wall end lands DURING the join, not before the paint */
    flow_arm_break(now + 5, FLOW_PIANO, FLOW_SCREEN);
    flow_finish_seconds = 9;

    finish_action_and_render(BTN_B, TIMER_IDLE, now, false);

    /* the edge was genuinely drained by the re-render's own drain */
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_CHIME));
    TEST_ASSERT_TRUE(flow_log_at(EV_CHIME) > flow_log_at(EV_NET_FINISH));
    TEST_ASSERT_EQUAL_INT(FLOW_PIANO, flow_active_slot);
    /* the snap is what the SECOND tick has to see */
    TEST_ASSERT_EQUAL_INT(2, flow_log_count(EV_TIMER_TICK));
    TEST_ASSERT_TRUE(flow_log_at(EV_CHIME) < flow_log_at_nth(EV_TIMER_TICK, 2));
}

void test_button_d_forces_the_re_render_full_as_well(void) {
    mock_time_set(flow_at(15, 0));
    flow_state = TIMER_IDLE;
    flow_net_finish = NET_FINISH_CHANGED;

    finish_action_and_render(BTN_D, TIMER_IDLE, flow_at(15, 0), false);

    TEST_ASSERT_EQUAL_INT(2, FLOW_RENDERS());
    TEST_ASSERT_EQUAL_INT(2, flow_log_count(EV_FULL_REFRESH));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PARTIAL));
}

/* ---- CYCLE 11: finish_or_break ------------------------------------------ */

/* The break gate runs BEFORE the render and, when it fires, takes the
   whole wake: the break screen is already painted, so the action's own
   render must never happen on top of it. */
void test_a_break_due_at_the_tail_takes_the_wake_before_any_render(void) {
    mock_time_set(flow_at(15, 0));
    flow_break_due_ret = true;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_finish_or_break(BTN_B, TIMER_IDLE, flow_at(15, 0), false));

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_START_BREAK)); /* it really fired */
    TEST_ASSERT_EQUAL_INT(1, flow_break_paint_count());
    TEST_ASSERT_EQUAL_INT(0, FLOW_RENDERS()); /* and nothing painted over it */
    TEST_ASSERT_EQUAL_INT(1, flow_sleeps);
}

void test_the_break_screen_releases_the_mqtt_phase_then_joins_then_sleeps(void) {
    mock_time_set(flow_at(15, 0));
    flow_break_due_ret = true;
    flow_window_active = true;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_finish_or_break(BTN_B, TIMER_IDLE, flow_at(15, 0), false));

    TEST_ASSERT_EQUAL_INT(1, flow_break_paint_count());
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_STATS_POST));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_NET_FINISH));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_SLEEP));
    TEST_ASSERT_TRUE(flow_break_paint_at() < flow_log_at(EV_STATS_POST));
    TEST_ASSERT_TRUE(flow_log_at(EV_STATS_POST) < flow_log_at(EV_NET_FINISH));
    TEST_ASSERT_TRUE(flow_log_at(EV_NET_FINISH) < flow_log_at(EV_SLEEP));
}

void test_no_break_due_falls_straight_through_to_the_render(void) {
    mock_time_set(flow_at(15, 0));
    flow_break_due_ret = false;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_RAN_OFF_THE_END,
                          flow_run_finish_or_break(BTN_B, TIMER_IDLE, flow_at(15, 0), false));

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_BREAK_DUE)); /* the gate was asked */
    TEST_ASSERT_EQUAL_INT(0, flow_break_paint_count());
    TEST_ASSERT_EQUAL_INT(1, FLOW_RENDERS());
    TEST_ASSERT_EQUAL_INT(0, flow_sleeps);
}

/* The gate takes `now` rather than reading the clock, because the tail's
   caller passes the instant the dispatched action left behind — not the
   live one, which the acknowledgement hold may already have moved. */
void test_the_tail_asks_the_break_gate_with_the_clock_it_was_handed(void) {
    mock_time_set(flow_at(15, 30)); /* deliberately not the argument */

    (void)flow_run_finish_or_break(BTN_B, TIMER_IDLE, flow_at(15, 0), false);

    TEST_ASSERT_EQUAL_INT64(flow_at(15, 0), flow_break_due_now);
}

/* ---- CYCLE 11: the pre-sleep event watch -------------------------------- */

/* A RUNNING slot owns the final minute; everything else can only be
   waiting on a break end. The two counters are each watch's very first
   read, so they say which branch ran even when both decline. */
void test_a_running_timer_gets_the_final_minute_watch(void) {
    mock_time_set(flow_at(15, 0));
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)flow_at(15, 0) + 3600; /* far out: declines */

    maybe_wait_for_event();

    TEST_ASSERT_TRUE(flow_expiry_wall_reads > 0);
    TEST_ASSERT_EQUAL_INT(0, flow_break_active_reads);
}

void test_every_state_but_running_gets_the_break_watch(void) {
    static const timer_state_t OTHERS[] = {TIMER_IDLE, TIMER_PAUSED, TIMER_BREAK, TIMER_EXPIRED};
    for (unsigned i = 0; i < sizeof OTHERS / sizeof OTHERS[0]; i++) {
        setUp();
        mock_time_set(flow_at(15, 0));
        flow_state = OTHERS[i];

        maybe_wait_for_event();

        TEST_ASSERT_TRUE(flow_break_active_reads > 0);
        TEST_ASSERT_EQUAL_INT(0, flow_expiry_wall_reads);
    }
}

/* The guaranteed drain. The final-minute watch deliberately ignores a
   break ending inside its window, so an edge left behind by it has
   exactly one owner: this call, after the watch and before sleep. */
void test_the_drain_after_the_final_minute_watch_repaints_a_missed_edge(void) {
    time_t now = flow_at(15, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 3600; /* the watch declines outright */
    flow_arm_break(now - 1, FLOW_SCREEN, FLOW_SCREEN);
    flow_extra_running = true; /* silent end, so only the repaint shows */

    maybe_wait_for_event();

    TEST_ASSERT_TRUE(flow_expiry_wall_reads > 0); /* the watch really was the branch */
    TEST_ASSERT_EQUAL_INT(1, flow_repaint_count());
}

void test_the_drain_after_the_break_watch_repaints_a_missed_edge(void) {
    time_t now = flow_at(15, 0);
    mock_time_set(now);
    flow_state = TIMER_IDLE;
    /* latched by a foreign tick, with slot 0 no longer marked active — so
       the break watch declines and only the drain can find it */
    flow_latched = true;
    flow_latched_wall = now;
    flow_extra_running = true;

    maybe_wait_for_event();

    TEST_ASSERT_TRUE(flow_break_active_reads > 0);
    TEST_ASSERT_EQUAL_INT(1, flow_repaint_count());
}

/* Reached on every path, including the one where neither watch did
   anything at all: the drain ticks unconditionally, which is what makes
   "every tick is safe" true rather than a promise per call site. */
void test_the_drain_is_reached_even_when_neither_watch_acts(void) {
    mock_time_set(flow_at(15, 0));
    flow_state = TIMER_IDLE;

    maybe_wait_for_event();

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_BREAK_TICK));
    TEST_ASSERT_EQUAL_INT(0, flow_repaint_count()); /* nothing to repaint */
}

/* ---- CYCLE 11, ROW 8: the grid wait is for deep-sleep wakes only --------

   6639bde: cold boot held a blank panel through the up-to-25 s alignment
   (field log: 19 s). The alignment buys nothing when there is nothing on
   screen. Every case below is a PAIR that differs only in the reset
   reason, so "no wait" is never mistaken for "nothing to wait for". */

void test_row8_a_power_on_wake_paints_immediately(void) {
    flow_tick_clock(flow_at(15, 0) + 35); /* 25 s of residue to the wall minute */
    flow_reset_reason = ESP_RST_POWERON;
    flow_state = TIMER_IDLE;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_UINT32(0, mock_delay_total_ms());
    TEST_ASSERT_EQUAL_INT(1, FLOW_RENDERS()); /* it did paint */
}

void test_row8_a_deep_sleep_wake_with_the_same_residue_waits_it_out(void) {
    flow_tick_clock(flow_at(15, 0) + 35);
    flow_reset_reason = ESP_RST_DEEPSLEEP;
    flow_state = TIMER_IDLE;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_UINT32(25000, mock_delay_total_ms());
    TEST_ASSERT_EQUAL_INT(1, FLOW_RENDERS());
}

/* Every non-DEEPSLEEP reason takes the immediate paint, not just POWERON:
   the guard is an inequality against one value, and an external reset
   (monitor DTR/RTS) reaches it just as a brownout or a panic does. */
void test_row8_every_reason_but_deep_sleep_skips_the_grid_wait(void) {
    static const esp_reset_reason_t REASONS[] = {ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW,
                                                 ESP_RST_PANIC,   ESP_RST_WDT, ESP_RST_BROWNOUT};
    for (unsigned i = 0; i < sizeof REASONS / sizeof REASONS[0]; i++) {
        setUp();
        flow_tick_clock(flow_at(15, 0) + 35);
        flow_reset_reason = REASONS[i];
        flow_state = TIMER_IDLE;

        TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

        TEST_ASSERT_EQUAL_UINT32(0, mock_delay_total_ms());
        TEST_ASSERT_EQUAL_INT(1, FLOW_RENDERS());
    }
}

/* The other half of the same fix: the pixel that covers the remaining few
   seconds of a cold boot. Deep-sleep tick wakes stay dark — a dim blink
   every minute all day is not worth the battery. */
void test_row8_a_cold_boot_lights_the_state_pixel_before_the_paint(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_reset_reason = ESP_RST_POWERON;
    flow_state = TIMER_IDLE;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_LED));
    TEST_ASSERT_TRUE(flow_log_at(EV_LED) < flow_log_at(EV_MAKE_STATE));
}

void test_row8_a_deep_sleep_tick_wake_stays_dark(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_reset_reason = ESP_RST_DEEPSLEEP;
    flow_state = TIMER_IDLE;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(1, FLOW_RENDERS()); /* the wake really ran */
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_LED));
}

/* Both guards read the reset reason for themselves. Pinned as a count
   because merging them into one cached read is exactly the sort of
   tidy-up that would change nothing on host and everything in a wake
   where the reason is re-latched between them. */
void test_row8_the_reset_reason_is_asked_at_both_gates(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_reset_reason = ESP_RST_DEEPSLEEP;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(2, flow_reset_reason_reads);
}

/* ---- CYCLE 11, ROW 10: expiry and break in the same tick ---------------

   aa8be4c. Three distinct call sites of the break gate, in a deliberate
   order: the fast path BEFORE the grid wait, the post-render check AFTER
   the alert, and finish_or_break's own (opposite-shaped) one. Only the
   middle one can produce expiry-then-break in a single wake. */

void test_row10_an_expiry_and_a_break_in_the_same_tick_alert_then_break(void) {
    time_t now = flow_at(15, 0);
    mock_time_set(now);
    flow_reset_reason = ESP_RST_DEEPSLEEP;
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now;       /* the expiry is at this instant... */
    flow_state_after_tick = TIMER_EXPIRED; /* ...and this wake's tick finds it */
    /* Not due on arrival, due once the alert has held the CPU: exactly
       the collision the post-render check exists for. */
    flow_break_due_from = now + 10;
    flow_alert_seconds = 15;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    /* Both halves genuinely happened... */
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_ALERT_EXPIRY));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_START_BREAK));
    TEST_ASSERT_EQUAL_INT(1, flow_break_paint_count());
    /* ...in that order, in ONE wake. */
    TEST_ASSERT_TRUE(flow_log_at(EV_ALERT_EXPIRY) < flow_break_paint_at());
    TEST_ASSERT_EQUAL_INT(1, flow_sleeps);
    /* and the fast-path gate WAS asked first and said no, which is what
       makes the ordering a property of the code and not of the fixture */
    TEST_ASSERT_TRUE(flow_log_at(EV_BREAK_DUE) < flow_log_at(EV_ALERT_EXPIRY));
}

/* The re-read the same commit added: `now` predates the render, and the
   alert holds the CPU for ~15 s. Asking the gate with the stale value
   would miss a break that came due during the alarm. */
void test_row10_the_post_render_gate_is_asked_with_a_freshly_read_clock(void) {
    time_t now = flow_at(15, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now;
    flow_state_after_tick = TIMER_EXPIRED;
    flow_break_due_from = now + 10;
    flow_alert_seconds = 15;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    /* flow_break_due_now holds the LAST instant the gate was asked at */
    TEST_ASSERT_EQUAL_INT64(now + 15, flow_break_due_now);
}

/* The fast path, which runs before this wake's timer_tick and therefore
   cannot see an expiry the tick is about to detect. With a break already
   due on arrival it wins outright and the wake ends without a paint. */
void test_row10_a_break_already_due_on_arrival_wins_before_the_paint(void) {
    time_t now = flow_at(15, 0);
    mock_time_set(now);
    flow_state = TIMER_IDLE;
    flow_break_due_ret = true;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(1, flow_break_paint_count());
    TEST_ASSERT_EQUAL_INT(0, FLOW_RENDERS());           /* no main layout was drawn */
    TEST_ASSERT_EQUAL_UINT32(0, mock_delay_total_ms()); /* nor waited for */
    TEST_ASSERT_EQUAL_INT(1, flow_sleeps);
}

/* The gate is a no-op on every ordinary wake, and it is asked twice — the
   fast path and the post-render check. Both must survive: deleting either
   is invisible unless the count is pinned. */
void test_row10_an_ordinary_tick_asks_the_break_gate_twice(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_state = TIMER_IDLE;
    flow_break_due_ret = false;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(2, flow_log_count(EV_BREAK_DUE));
    TEST_ASSERT_EQUAL_INT(0, flow_break_paint_count());
}

/* ---- CYCLE 11: the rest of the tick handler ----------------------------- */

void test_the_tick_wake_rolls_the_day_then_checks_bed_time_then_drains(void) {
    flow_tick_clock(flow_at(15, 0));

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_IS_NEW_DAY));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_CHECK_BEDTIME));
    TEST_ASSERT_TRUE(flow_log_at(EV_IS_NEW_DAY) < flow_log_at(EV_CHECK_BEDTIME));
    TEST_ASSERT_TRUE(flow_log_at(EV_CHECK_BEDTIME) < flow_log_at(EV_BREAK_TICK));
}

void test_the_bed_time_gate_can_end_a_tick_wake(void) {
    flow_tick_clock(flow_at(21, 0));
    flow_bedtime_locks = true;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_BEDTIME, flow_run_tick());

    TEST_ASSERT_TRUE(flow_bed_engaged); /* it really engaged */
    TEST_ASSERT_EQUAL_INT(0, FLOW_RENDERS());
    TEST_ASSERT_EQUAL_INT(0, flow_sleeps); /* the gate owns the sleep, not us */
}

/* The rollover opens its own network window and can step the clock; every
   later decision in the wake has to key off the corrected value. */
void test_the_bed_time_gate_sees_the_rollover_corrected_clock(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_new_day = true;
    flow_clock_after_window = flow_at(21, 30); /* the window corrected a bad clock */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    /* The rollover's own window ran, and ran BEFORE the gate — the gate
       is then handed the value it corrected. (A second window follows
       later in the wake: six hours of clock step puts the regular sync
       cadence past due too, which is the shipped behaviour.) */
    TEST_ASSERT_TRUE(flow_log_at(EV_TRY_WINDOW) >= 0);
    TEST_ASSERT_TRUE(flow_log_at(EV_TRY_WINDOW) < flow_log_at(EV_CHECK_BEDTIME));
    TEST_ASSERT_EQUAL_INT64(flow_at(21, 30), flow_bedtime_arg);
}

/* IDLE re-syncs on the slow menuconfig cadence. Pinned in both directions
   so "the sync ran" is never confused with "the cadence was ignored". */
void test_an_idle_wake_past_the_sync_cadence_opens_one_window(void) {
    mock_time_set(flow_at(15, 0));
    flow_state = TIMER_IDLE;
    flow_last_ntp = flow_at(15, 0) - IDLE_SYNC_INTERVAL_SEC;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_TRY_WINDOW));
}

void test_an_idle_wake_inside_the_sync_cadence_opens_none(void) {
    mock_time_set(flow_at(15, 0));
    flow_state = TIMER_IDLE;
    flow_last_ntp = flow_at(15, 0) - IDLE_SYNC_INTERVAL_SEC + 1;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_TRY_WINDOW));
    TEST_ASSERT_EQUAL_INT(1, FLOW_RENDERS()); /* the wake still ran */
}

/* A sync that stepped the clock must be what the rest of the wake uses —
   the grid wait, the tick and the paint all key off it. */
void test_a_sync_that_stepped_the_clock_is_what_the_paint_uses(void) {
    mock_time_set(flow_at(15, 0));
    flow_state = TIMER_IDLE;
    flow_last_ntp = flow_at(15, 0) - IDLE_SYNC_INTERVAL_SEC;
    flow_clock_after_window = flow_at(16, 0); /* on the wall minute: no grid wait */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT64(flow_at(16, 0), flow_tick_arg);
}

void test_a_running_countdown_is_snapped_to_the_minute_before_it_is_painted(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_state = TIMER_RUNNING;
    flow_needs_sync = false;
    flow_expiry_wall = (int64_t)flow_at(15, 0) + 3659; /* far past the watch window */
    flow_tick_ret = 3659;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(1, FLOW_RENDERS());
    TEST_ASSERT_EQUAL_INT32(3660, flow_partial_seen[0]);
}

/* The snap is guarded on RUNNING: a clock-only state's "remaining" is not
   a countdown and must reach the panel untouched. */
void test_a_non_running_state_is_painted_with_the_raw_remaining(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_state = TIMER_PAUSED;
    flow_tick_ret = 3659;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT32(3659, flow_partial_seen[0]);
}

/* The same jitter snap for the break countdown, whether it is the break
   screen's own big clock or the chip behind another timer. */
void test_the_break_countdown_on_the_panel_is_snapped_too(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_state = TIMER_IDLE;
    flow_made_break_banner = true; /* the chip behind another timer */
    flow_made_break_remaining = 119;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT32(120, flow_partial_break_remaining);
}

void test_the_break_screens_own_countdown_is_snapped_too(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_state = TIMER_IDLE;
    flow_made_state_kind = TIMER_BREAK; /* the break screen itself */
    flow_made_break_remaining = 119;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT32(120, flow_partial_break_remaining);
}

/* The guard's negative half: a panel with no break on it leaves the field
   alone. Same 119 as the two cases above, so only the guard differs. */
void test_a_panel_with_no_break_leaves_the_break_countdown_alone(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_state = TIMER_IDLE;
    flow_made_state_kind = TIMER_IDLE;
    flow_made_break_banner = false;
    flow_made_break_remaining = 119;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT32(119, flow_partial_break_remaining);
}

/* A lock released this wake owes the panel a full refresh, and the gate
   is what knows that. Three cases: the promotion is applied, it is not
   invented, and it can reach the alert arm. */
void test_the_lock_gate_may_promote_the_render_to_full(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_state = TIMER_IDLE;
    flow_promote_out = WAKE_RENDER_FULL;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(WAKE_RENDER_PARTIAL, flow_promote_in); /* what it was handed */
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_FULL_REFRESH));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PARTIAL));
}

void test_an_unpromoted_render_stays_partial(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_state = TIMER_IDLE;
    flow_promote_out = WAKE_RENDER_PARTIAL;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_PARTIAL));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_FULL_REFRESH));
}

void test_the_promoted_value_is_what_the_render_switch_reads(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_state = TIMER_IDLE;
    flow_promote_out = WAKE_RENDER_EXPIRY_ALERT;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_ALERT_EXPIRY));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PARTIAL));
    TEST_ASSERT_EQUAL_INT(0, FLOW_RENDER_FLUSHES()); /* the alert's repaint is not the switch's */
}

/* The latch drain: a press made while this wake was awake (sync, grid
   wait, e-ink flush) is acted on before sleep, or it evaporates. */
void test_a_press_latched_during_the_wake_is_dispatched_before_sleep(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_state = TIMER_IDLE;
    flow_b_result = BTN_B_STARTED;
    flow_press(BTN_B);
    /* the press really is in the latch */
    TEST_ASSERT_EQUAL_HEX8(1u << BTN_B, flow_latch_take_and_resample());
    flow_press(BTN_B);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_B_APPLY));
    TEST_ASSERT_EQUAL_INT(TIMER_RUNNING, flow_state);
}

/* THE swallowing case, end to end on the tick path. A press of the mode
   toggle must not cost the user the start press made alongside it — a
   screen selector is never worth a start/pause. ONE thing stops it now:
   A last in button_latch_pick's priority order. Until M2-T3 there were
   two, because A was also out of this drain's ALLOWED mask; admitting it
   (a bound button's press has to reach its arm) spent the belt and left
   the braces, so this case is load bearing in a way it was not before.
   Revert priority[] to its old A-first value and this case starts the
   timer never: pick returns A, the toggle runs, and the B press is gone
   with it because the drain's take is unmasked.

   Also pinned directly on the real pick by test_button_latch's
   test_pick_priority_b_over_a and test_pick_priority_b_over_all, and in
   this file by test_the_break_tail_acts_on_the_highest_priority_latched_press
   and test_c3_a_button_b_press_still_beats_a_button_a_press.

   The symptom is the worst kind: the device's primary control appears
   dead, with no feedback and no way to tell it from a flat battery. */
void test_a_latched_a_press_never_swallows_the_b_press_beside_it(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_state = TIMER_IDLE;
    flow_b_result = BTN_B_STARTED;
    flow_press(BTN_A);
    flow_press(BTN_B);
    /* both really are in the latch */
    TEST_ASSERT_EQUAL_HEX8((1u << BTN_A) | (1u << BTN_B), flow_latch_take_and_resample());
    flow_press(BTN_A);
    flow_press(BTN_B);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_B_APPLY));
    TEST_ASSERT_EQUAL_INT(TIMER_RUNNING, flow_state);
}

/* A latched ALONE now wins the pick and runs its own arm — and that arm
   reaches the mode and nothing else. The B map is armed to START so a
   fall-through out of A's arm into B's would move the slot loudly; the
   bit is still cleared either way, because the take is unmasked and
   happens whatever the pick decides. The mode half is asserted in the
   C3 section (test_c3_the_tick_wake_latch_drain_acts_on_a_latched_button_a_press);
   what is here is the slot NOT moving. */
void test_a_latched_a_press_alone_reaches_its_own_arm_and_no_slot(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_state = TIMER_IDLE;
    flow_b_result = BTN_B_STARTED; /* would fire loudly on a fall-through into B */
    flow_press(BTN_A);
    TEST_ASSERT_EQUAL_HEX8(1u << BTN_A, flow_latch_take_and_resample());
    flow_press(BTN_A);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_A_APPLY));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_B_APPLY));
    TEST_ASSERT_EQUAL_INT(TIMER_IDLE, flow_state);
    TEST_ASSERT_EQUAL_HEX8(0, flow_latch_residue()); /* taken, then dropped */
}

/* D's TIMER action — the update check and the network window — is
   unreachable from the tick drain. A D that rode in on a tick wake must
   not buy a second window, so it is taken and thrown away.

   IN TIMERS MODE, and the qualifier is M2-T4b's. This comment used to
   read "D is excluded from the drain's pick mask ... MEMBERSHIP IS NOT
   PINNED HERE, and cannot be: adding (1u << BTN_D) back to the mask
   changes nothing observable ... the mask mutant is the single survivor
   of this cycle's battery." Every sentence of that is now wrong.
   wake_flow_pick_latched_press() admits D while the mode byte says
   CHORES, membership is pinned in BOTH directions — W18 (D admitted
   unconditionally) dies on
   test_c4b_a_latched_d_press_outside_chore_mode_is_still_dropped and W21
   (the drain keeps the old A|B|C pick) dies on
   test_c4b_a_latched_d_press_in_chore_mode_acks_row_3 — and that battery
   has no survivors at all.

   This was BUG-3 (docs/planning/refactor.bugdiscoveries.md), recorded
   against the BREAK-TAIL mask with the tick drain folded in as the second
   call site. Its condition — "the moment BTN_D gains a dispatch arm" —
   fired in M2-T4a/T4b, the decision it asked for was made deliberately
   (admit D, and only in chore mode), and both call sites got the case it
   asked for. The register entry is closed there rather than here.

   What this case still pins, and the reason it did not become a
   duplicate of the c4b pair: it is the TIMER leg, asserted as
   EV_NET_OPEN == 0. The c4b negative control proves no ack happens; this
   one proves no WINDOW opens. */
void test_a_latched_d_press_in_timers_mode_is_never_dispatched_by_the_tick_drain(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_state = TIMER_IDLE;
    flow_press(BTN_D);
    TEST_ASSERT_EQUAL_HEX8(1u << BTN_D, flow_latch_take_and_resample());
    flow_press(BTN_D);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_NET_OPEN));
    TEST_ASSERT_EQUAL_HEX8(0, flow_latch_residue()); /* taken, then dropped */
}

/* A wake that already ran a window must not open a second one for the
   latched press: the clock is corrected and the buffered HA effects are
   already applied. Paired with the un-synced case below, which does. */
void test_a_synced_wake_denies_the_latched_press_a_second_window(void) {
    mock_time_set(flow_at(15, 0));
    flow_state = TIMER_IDLE;
    flow_last_ntp = flow_at(15, 0) - IDLE_SYNC_INTERVAL_SEC; /* sync is due */
    flow_b_result = BTN_B_STARTED;
    flow_press(BTN_B);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_TRY_WINDOW)); /* the wake did sync */
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_B_APPLY));    /* and did dispatch */
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_NET_OPEN));   /* but opened nothing */
}

void test_an_unsynced_wake_lets_the_latched_press_open_its_own_window(void) {
    mock_time_set(flow_at(15, 0));
    flow_state = TIMER_IDLE;
    flow_last_ntp = flow_at(15, 0); /* no sync due */
    flow_b_result = BTN_B_STARTED;
    flow_press(BTN_B);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_TRY_WINDOW));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_B_APPLY));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_NET_OPEN));
}

/* The drain must run BEFORE the event watch: the final-minute watch
   discards pre-watch latched presses at entry, so a drain moved after it
   would silently lose every press made during the wake. */
void test_the_latch_drain_runs_before_the_event_watch(void) {
    time_t now = flow_at(15, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 3600; /* the watch declines, but reads first */
    flow_needs_sync = false;
    flow_b_result = BTN_B_PAUSED;
    flow_press(BTN_B);
    TEST_ASSERT_EQUAL_HEX8(1u << BTN_B, flow_latch_take_and_resample());
    flow_press(BTN_B);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_B_APPLY)); /* the press was acted on */
    TEST_ASSERT_EQUAL_INT(TIMER_PAUSED, flow_state);
}

/* A press the guards refuse changed nothing, so the tail must not run:
   no LED of its own, no render, no window drain. */
void test_a_refused_latched_press_never_reaches_the_tail(void) {
    flow_tick_clock(flow_at(15, 0));
    /* The map is asked and turns the press down (BTN_B_NONE: a Screen
       Break, or an EXPIRED slot that cannot reload). Asserting the map WAS
       consulted is what separates this from a case where no press
       arrived at all. */
    flow_state = TIMER_EXPIRED;
    flow_b_result = BTN_B_NONE;
    flow_press(BTN_B);
    TEST_ASSERT_EQUAL_HEX8(1u << BTN_B, flow_latch_take_and_resample());
    flow_press(BTN_B);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_B_APPLY)); /* it was asked */
    TEST_ASSERT_EQUAL_INT(1, FLOW_RENDERS());             /* only the tick's own */
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_NET_FINISH));
}

void test_a_tick_wake_always_ends_in_deep_sleep_under_the_gates_mode(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_sleep_mode_answer = WAKE_SLEEP_BEDTIME;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(1, flow_sleeps);
    TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_BEDTIME, flow_slept_mode);
}

/* ---- CYCLE 11, ROW 7: the held-through-sleep guard ---------------------

   120569f: EXT1 ANY_LOW is level-triggered, so a button still held when
   the 3 s release-wait times out re-wakes the chip instantly and replayed
   its action (found holding A+D; the "Held-button dismissal" check in
   docs/hardware_checklist.md).

   The guard is three ANDed operands, and every case below flips exactly
   one of them while proving the wake really was decoded — the "nothing
   happened" assertions are otherwise equally true of a wake that never
   saw a button at all. */

void test_row7_a_button_held_through_a_two_second_sleep_is_ignored(void) {
    mock_time_set(flow_at(15, 0));
    flow_wakeup_btn = BTN_B;
    s_held_mask_at_sleep = 1u << BTN_B;
    s_sleep_entry_time = (int64_t)flow_at(15, 0) - 2;
    flow_b_result = BTN_B_STARTED; /* would fire loudly if it were dispatched */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    /* The wake WAS a button wake and the decode DID happen... */
    TEST_ASSERT_EQUAL_INT(1, flow_wakeup_btn_reads);
    /* ...and then nothing at all: no ack, no rollover, no action, no paint */
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_LED));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_IS_NEW_DAY));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_B_APPLY));
    TEST_ASSERT_EQUAL_INT(0, FLOW_RENDERS());
    TEST_ASSERT_EQUAL_INT(1, flow_sleeps);
}

/* The whole window, boundary included. 0, 1 and 2 seconds are all a
   continuation; 3 is a new press. */
void test_row7_the_ignore_window_is_two_seconds_inclusive(void) {
    for (int gap = 0; gap <= 3; gap++) {
        setUp();
        mock_time_set(flow_at(15, 0));
        flow_wakeup_btn = BTN_B;
        s_held_mask_at_sleep = 1u << BTN_B;
        s_sleep_entry_time = (int64_t)flow_at(15, 0) - gap;
        flow_b_result = BTN_B_STARTED;

        TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

        TEST_ASSERT_EQUAL_INT(1, flow_wakeup_btn_reads);
        if (gap <= 2) {
            TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_B_APPLY));
            TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_LED));
        } else {
            TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_B_APPLY));
            TEST_ASSERT_TRUE(flow_log_count(EV_LED) > 0);
        }
    }
}

/* A DIFFERENT button within the same two seconds is a new press: the mask
   is consulted per button, not as "something was held". */
void test_row7_a_different_button_inside_the_window_is_a_new_press(void) {
    mock_time_set(flow_at(15, 0));
    flow_wakeup_btn = BTN_B;
    s_held_mask_at_sleep = 1u << BTN_C; /* C was held, B woke us */
    s_sleep_entry_time = (int64_t)flow_at(15, 0);
    flow_b_result = BTN_B_STARTED;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_B_APPLY));
    TEST_ASSERT_TRUE(flow_log_count(EV_LED) > 0);
}

/* Nothing recorded as held: a wake this soon after sleep is still a real
   press (release-then-repress inside two seconds is possible). */
void test_row7_an_empty_held_mask_never_ignores_anything(void) {
    mock_time_set(flow_at(15, 0));
    flow_wakeup_btn = BTN_B;
    s_held_mask_at_sleep = 0;
    s_sleep_entry_time = (int64_t)flow_at(15, 0);
    flow_b_result = BTN_B_STARTED;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_B_APPLY));
}

/* BTN_NONE is 4, one bit past the four real buttons, so the `btn !=
   BTN_NONE` half of the guard is only load bearing against a mask with
   that bit set — which is what this case builds. Without it an undecoded
   wake inside the window would be swallowed silently. */
void test_row7_an_undecoded_wake_is_never_treated_as_a_continuation(void) {
    mock_time_set(flow_at(15, 0));
    flow_wakeup_btn = BTN_NONE;
    s_held_mask_at_sleep = 0xFFu; /* bit 4 set: the mask test alone would pass */
    s_sleep_entry_time = (int64_t)flow_at(15, 0);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT(1, flow_wakeup_btn_reads);
    TEST_ASSERT_TRUE(flow_log_count(EV_LED) > 0); /* the wake ran normally */
    TEST_ASSERT_EQUAL_INT(1, FLOW_RENDERS());
}

/* Each of the four buttons is guarded on its own bit — A included, even
   though it wakes only conditionally: the guard is a pure mask test and
   must not grow a special case. B and C are the ones a hold is least
   likely to be noticed on, since neither opens a window of its own. */
void test_row7_every_button_is_guarded_on_its_own_bit(void) {
    static const button_id_t BTNS[] = {BTN_A, BTN_B, BTN_C, BTN_D};
    for (unsigned i = 0; i < sizeof BTNS / sizeof BTNS[0]; i++) {
        setUp();
        mock_time_set(flow_at(15, 0));
        flow_wakeup_btn = BTNS[i];
        s_held_mask_at_sleep = (uint8_t)(1u << (int)BTNS[i]);
        s_sleep_entry_time = (int64_t)flow_at(15, 0);

        TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

        TEST_ASSERT_EQUAL_INT(1, flow_wakeup_btn_reads);
        TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_LED));
        TEST_ASSERT_EQUAL_INT(0, FLOW_RENDERS());
        TEST_ASSERT_EQUAL_INT(1, flow_sleeps);
    }
}

/* The write half of the guard, which enter_deep_sleep() calls while the
   pads are still digital. */
void test_the_sleep_entry_note_records_the_pads_and_the_clock(void) {
    mock_time_set(flow_at(15, 0));
    flow_held_now = (1u << BTN_A) | (1u << BTN_D);

    wake_flow_note_sleep_entry();

    TEST_ASSERT_EQUAL_HEX8((1u << BTN_A) | (1u << BTN_D), s_held_mask_at_sleep);
    TEST_ASSERT_EQUAL_INT64((int64_t)flow_at(15, 0), s_sleep_entry_time);
}

/* End to end through the RTC pair: what the sleep recorded is what the
   next wake's guard reads. Paired with the release below, which is the
   same round trip with the pads let go. */
void test_row7_the_guard_reads_back_what_the_sleep_entry_recorded(void) {
    mock_time_set(flow_at(15, 0));
    flow_held_now = 1u << BTN_B;
    wake_flow_note_sleep_entry();

    mock_time_set(flow_at(15, 0) + 1); /* the level-triggered instant re-wake */
    flow_wakeup_btn = BTN_B;
    flow_b_result = BTN_B_STARTED;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_B_APPLY));
}

void test_row7_a_sleep_that_recorded_no_hold_lets_the_next_press_through(void) {
    mock_time_set(flow_at(15, 0));
    flow_held_now = 0; /* released before the wait timed out */
    wake_flow_note_sleep_entry();

    mock_time_set(flow_at(15, 0) + 1);
    flow_wakeup_btn = BTN_B;
    flow_b_result = BTN_B_STARTED;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_B_APPLY));
}

/* ---- CYCLE 11: the rest of the button handler --------------------------- */

/* The pixel is the "button heard" ack and has to beat everything else,
   including the rollover's network window (seconds) and the bed-time
   gate. */
void test_the_button_wake_acks_on_the_pixels_before_any_other_work(void) {
    mock_time_set(flow_at(15, 0));
    flow_wakeup_btn = BTN_B;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_TRUE(flow_log_count(EV_LED) > 0);
    TEST_ASSERT_TRUE(flow_log_at(EV_LED) < flow_log_at(EV_IS_NEW_DAY));
    TEST_ASSERT_TRUE(flow_log_at(EV_LED) < flow_log_at(EV_CHECK_BEDTIME));
}

void test_the_button_wake_rolls_over_then_beds_then_drains_the_break(void) {
    mock_time_set(flow_at(15, 0));
    flow_wakeup_btn = BTN_B;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_TRUE(flow_log_at(EV_IS_NEW_DAY) < flow_log_at(EV_CHECK_BEDTIME));
    TEST_ASSERT_TRUE(flow_log_at(EV_CHECK_BEDTIME) < flow_log_at(EV_BREAK_TICK));
}

void test_the_bed_time_gate_can_end_a_button_wake(void) {
    mock_time_set(flow_at(21, 0));
    flow_wakeup_btn = BTN_B;
    flow_bedtime_locks = true;
    flow_b_result = BTN_B_STARTED;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_BEDTIME, flow_run_button());

    TEST_ASSERT_TRUE(flow_bed_engaged);
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_B_APPLY)); /* the press is swallowed */
}

void test_a_wake_press_is_dispatched_through_the_shared_guard_matrix(void) {
    mock_time_set(flow_at(15, 0));
    flow_wakeup_btn = BTN_C;
    flow_select_ok = true;
    flow_state_at_select = TIMER_IDLE;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_SELECT_NEXT));
    TEST_ASSERT_EQUAL_INT(FLOW_PIANO, flow_active_slot);
    TEST_ASSERT_EQUAL_INT(1, FLOW_RENDERS());
}

/* The other half of the same decode, and the primary control on the
   device: a B wake press reaches the state map and starts the timer. */
void test_a_wake_press_on_button_b_runs_the_state_map(void) {
    mock_time_set(flow_at(15, 0));
    flow_wakeup_btn = BTN_B;
    flow_state = TIMER_IDLE;
    flow_b_result = BTN_B_STARTED;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_B_APPLY));
    TEST_ASSERT_EQUAL_INT(TIMER_RUNNING, flow_state);
    TEST_ASSERT_EQUAL_INT(1, FLOW_RENDERS());
}

/* A shares B and C's arm in the wake decode, so the thing to pin here is
   that sharing the ARM does not mean sharing the BODY: nothing B or C
   does may run for an A press. The B map is armed to START and the swap
   to succeed, so a fall-through past A's `case` into either would be loud
   rather than silent — which is the failure mode of grouping cases in a
   switch, and the reason this case survives the binding that made
   A's decode real. The mode assertion is the C3 section's
   (test_c3_a_button_a_ext1_wake_toggles_the_mode). */
void test_a_wake_press_on_button_a_moves_no_timer(void) {
    mock_time_set(flow_at(15, 0));
    flow_wakeup_btn = BTN_A;
    flow_state = TIMER_IDLE;
    flow_b_result = BTN_B_STARTED;
    flow_select_ok = true;
    flow_net_open = true;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT(1, flow_wakeup_btn_reads); /* it really was decoded */
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_B_APPLY));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_SELECT_NEXT));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_NET_OPEN)); /* and opened nothing */
    TEST_ASSERT_EQUAL_INT(TIMER_IDLE, flow_state);
    TEST_ASSERT_EQUAL_INT(1, FLOW_RENDERS()); /* the wake still paints the truth */
    TEST_ASSERT_EQUAL_INT(1, flow_sleeps);
}

/* A wake press always gets a window if one will open — unlike the tick
   drain, which is gated on whether this wake already synced. */
void test_a_wake_press_is_always_allowed_its_network_window(void) {
    mock_time_set(flow_at(15, 0));
    flow_wakeup_btn = BTN_B;
    flow_b_result = BTN_B_STARTED;
    flow_net_open = true;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_NET_OPEN));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_WAIT_NTP));
}

/* Button D is the force-sync button: it opens a window and waits for the
   clock before the paint, then renders full because D always does. */
void test_button_d_syncs_before_the_paint_and_re_reads_the_clock(void) {
    mock_time_set(flow_at(15, 0));
    flow_wakeup_btn = BTN_D;
    flow_net_open = true;
    flow_ntp_seconds = 7; /* the sync took seven seconds */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_NET_OPEN));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_WAIT_NTP));
    TEST_ASSERT_TRUE(flow_log_at(EV_WAIT_NTP) < flow_log_at(EV_MAKE_STATE));
    TEST_ASSERT_EQUAL_INT64(flow_at(15, 0) + 7, flow_tick_arg);
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_FULL_REFRESH));
}

void test_button_d_with_no_window_available_skips_the_ntp_wait(void) {
    mock_time_set(flow_at(15, 0));
    flow_wakeup_btn = BTN_D;
    flow_net_open = false;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_NET_OPEN)); /* it did ask */
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_WAIT_NTP));
    TEST_ASSERT_EQUAL_INT(1, FLOW_RENDERS()); /* and still painted */
}

/* An EXT1 wake whose decode came back empty still paints: the wake
   happened, so something moved, and the panel should show the truth. */
void test_an_undecoded_button_wake_still_renders_and_sleeps(void) {
    mock_time_set(flow_at(15, 0));
    flow_wakeup_btn = BTN_NONE;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_B_APPLY));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_SELECT_NEXT));
    TEST_ASSERT_EQUAL_INT(1, FLOW_RENDERS());
    TEST_ASSERT_EQUAL_INT(1, flow_sleeps);
}

/* A STALE EDGE on the wake press's own button must not replay through the
   awake-press consumers: a resume with <70 s left flows straight into the
   final-minute watch, where a stale B edge would instantly re-pause.

   "A stale edge" and not "release bounce", since M2-T12: button_latch's
   release gate rejects a falling edge on a pad nobody has seen come back
   up, so bounce on a still-held button never reaches the latch at all.
   What this and the two cases below model is therefore the edge that CAN
   still be latched — a second tap during the action, or chatter after a
   level sample already observed the pad up — which is exactly what
   flow_press() records. The drain is unchanged; only the population of
   edges reaching it is smaller. */
void test_the_wake_press_stale_edge_is_drained_before_the_tail(void) {
    mock_time_set(flow_at(15, 0));
    flow_wakeup_btn = BTN_C;
    flow_select_ok = false; /* C refused, so nothing else consumes the latch */
    flow_press(BTN_B);
    /* the stale edge really is in the latch */
    TEST_ASSERT_EQUAL_HEX8(1u << BTN_B, flow_latch_take_and_resample());
    flow_press(BTN_B);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    /* it was taken and dropped, not acted on */
    TEST_ASSERT_TRUE(flow_full_takes > 0);
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_B_APPLY));
    TEST_ASSERT_EQUAL_HEX8(0, flow_latch_residue());
}

/* A resume with under 70 s left lands straight in the final-minute watch.
   Without the drain above, the stale edge re-pauses it instantly — this is
   that path end to end, and the assertion is that it does NOT. See the
   case above for why "stale edge" and not "release bounce" since M2-T12. */
void test_a_resume_into_the_final_minute_is_not_re_paused_by_its_own_stale_edge(void) {
    time_t now = flow_at(15, 0);
    mock_time_set(now);
    flow_wakeup_btn = BTN_B;
    flow_b_result = BTN_B_RESUMED;
    flow_expiry_wall = (int64_t)now + 40; /* inside the watch window */
    flow_net_open = false;
    flow_press(BTN_B); /* the stale edge left by the press that resumed */
    TEST_ASSERT_EQUAL_HEX8(1u << BTN_B, flow_latch_take_and_resample());
    flow_press(BTN_B);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_B_APPLY)); /* the resume itself */
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PAUSE));   /* and no re-pause */
    TEST_ASSERT_EQUAL_INT(TIMER_RUNNING, flow_state);
}

void test_a_button_wake_always_ends_in_deep_sleep_under_the_gates_mode(void) {
    mock_time_set(flow_at(15, 0));
    flow_wakeup_btn = BTN_B;
    flow_sleep_mode_answer = WAKE_SLEEP_CHARGE_LOCK;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT(1, flow_sleeps);
    TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_CHARGE_LOCK, flow_slept_mode);
}

/* A break that came due while the press was being handled paints the
   break screen and takes the wake, instead of rendering the action. */
void test_a_button_wake_whose_action_pushed_the_balance_over_breaks(void) {
    mock_time_set(flow_at(15, 0));
    flow_wakeup_btn = BTN_B;
    flow_b_result = BTN_B_RESUMED;
    flow_net_open = false;
    flow_break_due_ret = true;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_B_APPLY)); /* the resume ran */
    TEST_ASSERT_EQUAL_INT(1, flow_break_paint_count());
    TEST_ASSERT_EQUAL_INT(0, FLOW_RENDERS());
}

/* ---- CYCLE 11: BUG-1, pinned as an observation, not a fix ---------------

   docs/planning/refactor.bugdiscoveries.md BUG-1 asks whether the
   sync/idle cadence path polls the button latch after the network window
   closes and before sleep. It does: the tick handler's drain is
   unconditional and runs on every path. These two cases record the
   CURRENT behaviour so that a later fix has to change them deliberately.

   What the sync path does NOT do is poll DURING the window — and the
   grid wait that follows it consumes a B press and throws it away
   whenever the timer is not RUNNING (BUG-2). On an IDLE hourly sync wake
   both of those windows are open at once.

   That is NOT the mechanism the BUG-1 report describes, and this file
   must not be read as if it were. BUG-1's symptom was a **C** press, and
   the grid-wait poll takes the B bit alone — button_latch_take_masked()
   clears only the masked bits, so a latched C survives it and reaches the
   drain. The second test below therefore pins BUG-2 on the sync path, not
   BUG-1; BUG-1's mechanism is still unidentified. Neither is fixed. */

void test_bug1_an_idle_sync_wake_does_drain_the_latch_before_sleep(void) {
    mock_time_set(flow_at(15, 0));
    flow_state = TIMER_IDLE;
    flow_last_ntp = flow_at(15, 0) - IDLE_SYNC_INTERVAL_SEC;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_TRY_WINDOW)); /* the window ran */
    TEST_ASSERT_TRUE(flow_full_takes >= 1);                  /* and the latch was taken */
}

/* The BUG-2 half, on the IDLE sync path specifically: a press made during
   the grid wait while the timer is not RUNNING is consumed by the pause
   poll and discarded, so the drain that follows finds nothing. KNOWN BUG,
   deliberately preserved — the delivery assertion is what stops this
   reading as "no press was made". */
void test_row6_the_bug2_eat_also_happens_on_the_idle_sync_path_KNOWN_BUG(void) {
    mock_time_set(flow_at(15, 0) + 35); /* 25 s of residue to the wall minute */
    flow_state = TIMER_IDLE;
    flow_last_ntp = flow_at(15, 0) - IDLE_SYNC_INTERVAL_SEC;
    flow_b_result = BTN_B_STARTED;
    flow_deferred_press_btn = BTN_B;
    flow_deferred_press_ms = 5000; /* five seconds into the grid wait */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    /* The press was genuinely delivered, by wall time, through the real
       latch — not as a side effect of anything the code under test did. */
    TEST_ASSERT_EQUAL_INT(1, flow_deferred_delivered);
    /* And it did nothing: eaten by the pause poll's unconditional masked
       take while the timer was IDLE. */
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_B_APPLY));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PAUSE));
    TEST_ASSERT_EQUAL_HEX8(0, flow_latch_residue());
}

/* ---- CYCLE 11: mutants the first pass let through ----------------------

   Every case below was written because a specific perturbation of the
   moved code survived the suite. The mutant each one kills is named, so a
   later edit that makes the case vacuous is obvious rather than silent. */

/* Kills: the grid-wait cap raised to 26. A 25 s residue is waited out
   under both caps; a 26 s residue is waited out only under the wrong one. */
void test_row8_a_residue_one_second_past_the_cap_renders_in_place(void) {
    flow_tick_clock(flow_at(15, 0) + 34); /* 26 s to the wall minute */
    flow_reset_reason = ESP_RST_DEEPSLEEP;
    flow_state = TIMER_IDLE;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_UINT32(0, mock_delay_total_ms());
    TEST_ASSERT_EQUAL_INT(1, FLOW_RENDERS()); /* it rendered, just off-grid */
}

/* Kills: `before` captured AFTER the grid wait, and the render's
   button_wake argument flipped. Both need a state change that happens
   INSIDE the wait and is not an expiry — which is exactly what the wait's
   pause poll produces, and the only thing that does. */
void test_row8_a_pause_inside_the_grid_wait_is_a_change_the_render_must_see(void) {
    time_t now = flow_at(15, 0) + 35; /* 25 s of residue */
    flow_tick_clock(now);
    flow_reset_reason = ESP_RST_DEEPSLEEP;
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 3625; /* 25 s of countdown residue too */
    flow_deferred_press_btn = BTN_B;
    flow_deferred_press_ms = 5000; /* five seconds into the wait */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    /* The press arrived on wall time, through the real latch, and paused. */
    TEST_ASSERT_EQUAL_INT(1, flow_deferred_delivered);
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_PAUSE));
    TEST_ASSERT_EQUAL_INT(TIMER_PAUSED, flow_state);
    /* before = RUNNING (captured ahead of the wait), after = PAUSED: a
       real state change on a NON-button wake, so the render is full. */
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_FULL_REFRESH));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PARTIAL));
    /* The two negative controls, inline: capturing `before` after the wait
       collapses the pair, and calling the policy as a button wake hides a
       change that crosses no break-screen boundary. Both go partial. */
    TEST_ASSERT_EQUAL_INT(WAKE_RENDER_PARTIAL, wake_policy_render(TIMER_PAUSED, TIMER_PAUSED, false, false, false));
    TEST_ASSERT_EQUAL_INT(WAKE_RENDER_PARTIAL, wake_policy_render(TIMER_RUNNING, TIMER_PAUSED, true, false, false));
}

/* Kills: the guaranteed drain moved AHEAD of the watches. The edge this
   finds does not exist when the drain runs — the break's wall end passes
   during the watch — so a drain-first ordering finds nothing at all. */
void test_the_guaranteed_drain_catches_an_edge_that_appeared_during_the_watch(void) {
    time_t now = flow_at(15, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 10; /* the watch owns the next ten seconds */
    flow_arm_break(now + 5, FLOW_SCREEN, FLOW_SCREEN);

    maybe_wait_for_event();

    /* the watch really ran to the expiry */
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_ALERT_EXPIRY));
    TEST_ASSERT_EQUAL_UINT32(10000, mock_delay_total_ms());
    /* and the break end that fell inside it was chimed and repainted by
       the drain AFTERWARDS — a drain before the watch sees nothing */
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_CHIME));
    TEST_ASSERT_TRUE(flow_log_at(EV_CHIME) > flow_log_at(EV_ALERT_EXPIRY));
}

/* Kills: the clock re-read after the sync window deleted. The window
   blocks for seconds and can step the clock; the fast-path break gate is
   the first thing that reads the corrected value. */
void test_a_sync_that_stepped_the_clock_is_what_the_fast_path_gate_sees(void) {
    time_t base = flow_at(15, 0);
    flow_tick_clock(base);
    flow_state = TIMER_IDLE;
    flow_last_ntp = base - IDLE_SYNC_INTERVAL_SEC; /* a sync is due */
    flow_clock_after_window = base + 40;           /* and it corrects the clock */
    flow_break_due_from = base + 40;               /* due only at the corrected value */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_TRY_WINDOW)); /* the window ran */
    TEST_ASSERT_EQUAL_INT(1, flow_break_paint_count());      /* the fast path fired */
    TEST_ASSERT_EQUAL_INT(0, FLOW_RENDERS());                /* before any paint */
}

/* Kills: the minute snap's operand shifted by one. The countdown here is
   30 s off the grid, so the snap leaves it exactly alone — a +-1 near a
   minute boundary is absorbed by the snap itself and invisible. */
void test_an_off_grid_countdown_is_painted_exactly_as_the_tick_reported_it(void) {
    time_t now = flow_at(15, 0);
    flow_tick_clock(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 3630; /* 30 s residue: no grid wait */
    flow_tick_ret = 3630;                   /* 30 s residue: no snap either */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT32(3630, flow_partial_seen[0]);
}

/* Kills: the break countdown's snap operand shifted by one, for the same
   reason. */
void test_an_off_grid_break_countdown_reaches_the_panel_untouched(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_state = TIMER_IDLE;
    flow_made_break_banner = true;
    flow_made_break_remaining = 90; /* 30 s residue */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT32(90, flow_partial_break_remaining);
}

/* Kills: the break_ended argument dropped from the tick handler's render.
   The state pair is unchanged, so the flag is the only thing standing
   between a full refresh and a ghosted panel. */
void test_a_break_that_ended_this_wake_forces_the_tick_render_full(void) {
    time_t now = flow_at(15, 0);
    flow_tick_clock(now);
    flow_state = TIMER_IDLE;
    flow_arm_break(now - 1, FLOW_SCREEN, FLOW_SCREEN);
    flow_extra_running = true; /* silent end: only the flag is left */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_TRUE(wake_flow_break_ended_this_wake()); /* the edge was real */
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_FULL_REFRESH));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PARTIAL));
    /* the negative control: without the flag this exact pair goes partial */
    TEST_ASSERT_EQUAL_INT(WAKE_RENDER_PARTIAL, wake_policy_render(TIMER_IDLE, TIMER_IDLE, false, false, false));
}

/* Kills: the tick drain moved after the event watch. The watch DISCARDS
   pre-watch latched presses at entry, so a drain that runs second finds
   an empty latch and the press is lost — the exact failure the ordering
   comment warns about. */
void test_the_latch_drain_runs_before_the_event_watch_can_discard_it(void) {
    time_t now = flow_at(15, 0);
    flow_tick_clock(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 40; /* INSIDE the watch window */
    flow_b_result = BTN_B_PAUSED;
    flow_press(BTN_B);
    TEST_ASSERT_EQUAL_HEX8(1u << BTN_B, flow_latch_take_and_resample()); /* it is latched */
    flow_press(BTN_B);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_B_APPLY));
    TEST_ASSERT_EQUAL_INT(TIMER_PAUSED, flow_state);
    /* and because the press was serviced, the watch never sat out the
       final minute at all */
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_ALERT_EXPIRY));
}

/* Kills: the post-render break gate moved after the latch drain. The gate
   does not return, so ordering decides whether the press is ever
   dispatched. Button C, not B: the grid wait's pause poll takes only the
   B bit, so a C press survives the wait to reach the drain. */
void test_row10_the_post_render_break_takes_the_wake_before_the_latched_press(void) {
    time_t base = flow_at(15, 0);
    flow_tick_clock(base + 35); /* 25 s of grid wait to burn */
    flow_state = TIMER_IDLE;
    flow_select_ok = true;
    flow_state_at_select = TIMER_IDLE;
    flow_break_due_from = base + 40; /* not due on arrival; due after the wait */
    flow_press(BTN_C);
    TEST_ASSERT_EQUAL_HEX8(1u << BTN_C, flow_latch_take_and_resample());
    flow_press(BTN_C);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_UINT32(25000, mock_delay_total_ms()); /* the wait happened */
    TEST_ASSERT_EQUAL_INT(1, flow_break_paint_count());
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_SELECT_NEXT)); /* the wake ended first */
}

/* Kills: `before` captured before the break drain in the button handler.
   The snap back moves the SELECTION, so the state the render compares
   against is the interrupted slot's — and that slot is already EXPIRED,
   whose alarm was heard when it actually ran and must not be replayed. */
void test_the_button_wake_captures_the_state_the_break_drain_left_behind(void) {
    time_t now = flow_at(15, 0);
    mock_time_set(now);
    flow_wakeup_btn = BTN_NONE;
    flow_arm_break(now - 1, FLOW_PIANO, FLOW_SCREEN);
    flow_state = TIMER_IDLE;
    flow_state_at_interrupted = TIMER_EXPIRED; /* Piano expired before the break */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    /* the snap really happened, and it really moved the state */
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_SNAP_BACK));
    TEST_ASSERT_EQUAL_INT(TIMER_EXPIRED, flow_state);
    /* so `before` is EXPIRED too, and no alarm is replayed */
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_ALERT_EXPIRY));
    /* the negative control: had `before` been captured ahead of the drain
       it would still say IDLE, and this render alerts */
    TEST_ASSERT_EQUAL_INT(WAKE_RENDER_EXPIRY_ALERT, wake_policy_render(TIMER_IDLE, TIMER_EXPIRED, true, true, false));
}

/* Kills: the stale-edge drain moved after the tail. The tail can end the
   wake outright (its break gate does not return), so a drain placed after
   it never runs at all and the edge is still latched at sleep. ("Stale
   edge" rather than "release bounce" since M2-T12 — see
   test_the_wake_press_stale_edge_is_drained_before_the_tail.) */
void test_a_break_at_the_tail_still_drains_the_stale_edge_first(void) {
    mock_time_set(flow_at(15, 0));
    flow_wakeup_btn = BTN_C;
    flow_select_ok = false; /* refused, so nothing else consumes the latch */
    flow_break_due_ret = true;
    flow_press(BTN_B);
    TEST_ASSERT_EQUAL_HEX8(1u << BTN_B, flow_latch_take_and_resample());
    flow_press(BTN_B);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT(1, flow_break_paint_count()); /* the tail ended it */
    TEST_ASSERT_EQUAL_HEX8(0, flow_latch_residue());    /* drained anyway */
}

/* Kills: the event watch deleted from the button handler. A resume that
   lands inside the final minute has to be watched out, or the expiry is
   an hour late. */
void test_the_button_wake_reaches_the_event_watch_before_it_sleeps(void) {
    time_t now = flow_at(15, 0);
    mock_time_set(now);
    flow_wakeup_btn = BTN_NONE;
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 20; /* inside the watch window */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_ALERT_EXPIRY));
    TEST_ASSERT_EQUAL_UINT32(20000, mock_delay_total_ms());
}

/* ---- CYCLE 13: the three decisions the residency audit moved out of main.c

   Nothing here is new behaviour — each case pins what main.c was already
   doing, at the point where the decision stopped being untestable. The
   wake-cause decode is the one that matters most: it is the single
   largest fork in the firmware (two handlers, neither of which returns)
   and until now the only branch in the tree with no coverage at all. */

/* The suite drives the decode directly rather than through flow_run_wake,
   which only takes a void(void). Same two landing pads: whichever handler
   the decode picks, it ends in a sleep that does not return. */
static flow_wake_result_t flow_run_wake_causes(uint32_t causes) {
    if (setjmp(flow_bed_jmp) != 0)
        return FLOW_WAKE_BEDTIME;
    if (setjmp(flow_sleep_jmp) != 0)
        return FLOW_WAKE_SLEPT;
    wake_flow_handle_wake(causes);
    return FLOW_WAKE_RAN_OFF_THE_END;
}

/* Which handler ran, told apart by a call only one of them makes:
   buttons_get_wakeup_button() is the button handler's first act and the
   tick handler never asks it. Reading the wake button is what makes this
   a fact about the decode rather than about the fixture — both paths
   reach a sleep, so flow_sleeps alone cannot separate them. */
#define FLOW_TOOK_BUTTON_PATH() (flow_wakeup_btn_reads > 0)

void test_row13_an_ext1_wake_goes_to_the_button_handler(void) {
    flow_tick_clock(flow_at(15, 0));

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_wake_causes(BIT(ESP_SLEEP_WAKEUP_EXT1)));

    TEST_ASSERT_TRUE(FLOW_TOOK_BUTTON_PATH());
}

void test_row13_a_timer_wake_goes_to_the_tick_handler(void) {
    flow_tick_clock(flow_at(15, 0));

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_wake_causes(BIT(ESP_SLEEP_WAKEUP_TIMER)));

    TEST_ASSERT_FALSE(FLOW_TOOK_BUTTON_PATH());
}

/* A cold boot reports NO cause at all (esp_sleep_get_wakeup_causes()
   returns 0 when the reset was not an exit from deep sleep). It has to
   land on the tick handler: that is the path that installs the day, syncs
   the clock and paints. Falling through to the button handler would make
   a power-on wait for a wake button that was never pressed. */
void test_row13_a_cold_boot_reports_no_cause_and_still_ticks(void) {
    flow_tick_clock(flow_at(15, 0));

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_wake_causes(0));

    TEST_ASSERT_FALSE(FLOW_TOOK_BUTTON_PATH());
}

/* Kills the equality rewrite. `causes` is a BITMASK — the silicon can
   report more than one source for the same wake — so `causes ==
   BIT(EXT1)` looks identical on every case above and silently sends a
   button press that coincided with the RTC alarm to the tick handler,
   where the press is never decoded. */
void test_row13_ext1_alongside_the_timer_is_still_a_button_wake(void) {
    flow_tick_clock(flow_at(15, 0));

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT,
                          flow_run_wake_causes(BIT(ESP_SLEEP_WAKEUP_EXT1) | BIT(ESP_SLEEP_WAKEUP_TIMER)));

    TEST_ASSERT_TRUE(FLOW_TOOK_BUTTON_PATH());
}

/* ---- the panic-loop breaker -------------------------------------------- */

/* The S2 ROM USB console can panic when a host port-open races the boot
   prints; each panic reboots, re-enumerates and re-races. Staying quiet
   after a panic is what lets the host's open complete against silence. */
void test_row13_a_panic_reset_stays_quiet_before_the_boot_prints(void) {
    mock_time_set(flow_at(15, 0));
    flow_reset_reason = ESP_RST_PANIC;

    wake_flow_boot_quiet_after_panic();

    TEST_ASSERT_EQUAL_UINT32(2000, mock_delay_total_ms());
}

/* The other arm, and the one that costs battery if it rots: a quiet
   window on every wake would add two seconds of CPU to each of them.
   Swept across every reason rather than one, because the guard is an
   equality against a single value. */
void test_row13_no_other_reset_reason_delays_the_boot(void) {
    static const esp_reset_reason_t REASONS[] = {ESP_RST_DEEPSLEEP, ESP_RST_POWERON, ESP_RST_EXT,     ESP_RST_SW,
                                                 ESP_RST_INT_WDT,   ESP_RST_WDT,     ESP_RST_BROWNOUT};
    for (unsigned i = 0; i < sizeof REASONS / sizeof REASONS[0]; i++) {
        setUp();
        mock_time_set(flow_at(15, 0));
        flow_reset_reason = REASONS[i];

        wake_flow_boot_quiet_after_panic();

        TEST_ASSERT_EQUAL_UINT32(0, mock_delay_total_ms());
    }
}

/* ---- the last-chance break-end drain ------------------------------------

   enter_deep_sleep() calls this as a safety net. The pre-sleep event
   watch drains on every path that does timer work, so a true return here
   means a new code path skipped the drain — it reports, it does not
   chime, because this also runs from the awake failsafe's esp_timer
   context where audio is not safe. */

void test_row13_an_undrained_break_end_is_taken_at_sleep(void) {
    mock_time_set(flow_at(15, 0));
    flow_latched = true; /* an edge nothing serviced this wake */
    flow_latched_wall = flow_at(14, 59);

    TEST_ASSERT_TRUE(wake_flow_report_undrained_break_end());
}

/* Not a second return value but a second CALL: the point of the take is
   that it CONSUMES the latch, so an implementation that merely peeked
   would leave the edge to be reported again. Only the second call can
   tell those apart. */
void test_row13_the_undrained_take_actually_consumes_the_latch(void) {
    mock_time_set(flow_at(15, 0));
    flow_latched = true;
    flow_latched_wall = flow_at(14, 59);

    TEST_ASSERT_TRUE(wake_flow_report_undrained_break_end());
    TEST_ASSERT_FALSE(wake_flow_report_undrained_break_end());
}

/* The path every healthy wake takes. */
void test_row13_a_wake_that_drained_its_break_reports_nothing_at_sleep(void) {
    mock_time_set(flow_at(15, 0));

    TEST_ASSERT_FALSE(wake_flow_report_undrained_break_end());
}

/* Deliberately NOT wake_flow_break_end(): this is a raw take, so it must
   not tick the break, must not chime, must not snap the selection back
   and must not set the wake-sticky break-ended flag — every one of those
   would be a side effect landing after the panel has already gone to
   sleep. Pinned because "reuse the owner instead" is the obvious tidy-up
   and it is wrong here.

   ONE side effect is deliberately exempt from that rule and is asserted a
   few cases further down instead: the C16 mode revert. It is not a THIS
   WAKE effect at all — it writes an RTC byte the NEXT wake's paint reads
   — so "lands after the panel has gone to sleep" is not an argument
   against it. See test_c16_an_undrained_break_end_at_sleep_still_reverts
   for the whole argument. The four absences below are unaffected, and the
   exemption is BOUNDED rather than merely named: the last two assertions
   pin the exempt write at exactly one AND the whole effect log at that
   one entry, so a SECOND write added here — a timer_chore_set_released
   (false), say — still fails a test whose name says "beyond the take"
   and is meant to mean it. */
void test_row13_the_sleep_drain_has_no_side_effects_beyond_the_take(void) {
    mock_time_set(flow_at(15, 0));
    flow_latched = true;
    flow_latched_wall = flow_at(14, 59);
    /* setUp leaves active == interrupted == FLOW_SCREEN, which makes the
       snap-back stub a no-op and the slot assertion below unable to fail.
       Point them at different slots so a snap is actually observable —
       the snap is the one side effect here with a VISIBLE consequence
       (it changes which timer the panel would name), so it is the arm
       that most needs a live assertion rather than a decorative one. */
    flow_active_slot = FLOW_PIANO;

    TEST_ASSERT_TRUE(wake_flow_report_undrained_break_end());

    TEST_ASSERT_FALSE(wake_flow_break_ended_this_wake());
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_BREAK_TICK));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_CHIME));
    /* Both: the event count catches a snap regardless of fixture, the
       slot catches one that somehow bypassed the stub's logging. */
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_SNAP_BACK));
    TEST_ASSERT_EQUAL_INT(FLOW_PIANO, flow_active_slot);
    /* BOUNDS the exemption the header names, rather than restating it.
       The four assertions above pin four NAMED absences; what the header
       permits is a CATEGORY ("an RTC byte the next wake reads"), and a
       category cannot be pinned by absences — a second write of that kind
       would be a new side effect wearing the exemption's coat while every
       assertion above still passed.

       Two assertions, because either one alone leaves the hole open. The
       first bounds the exempt write to the ONE the C16 revert costs. It
       counts CALLS and not changes — the stub logs every timer_set_mode()
       whatever value is stored — so it is exactly 1 however setUp left
       flow_mode. But it sees only MODE writes, and the obvious next
       arrival is not one: a timer_chore_set_released(false) added here
       would sail straight past it, which is the exact shape this is here
       to stop. So the second bounds the WHOLE effect log to that single
       entry. The take itself logs nothing (its stub only clears the
       latch), so "one event, and it is the mode write" is precisely the
       claim the function's name makes.

       Residual hole, written down so it is not mistaken for coverage: an
       RTC accessor whose stub does not log is invisible to both. Every
       setter stub in this file logs — that is the convention these two
       assertions lean on, and a new setter stub that skips it is the
       thing to catch in review. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_SET_MODE),
                                  "the sleep drain grew a second mode write beyond the C16 revert");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_n, "the sleep drain grew a side effect beyond the C16 mode revert");
}

/* ---- M2-T2: the painted mode and its reverts (design rows C16, C17) -----

   One RTC byte says which screen the device paints. This module owns
   three of the four edges that move it, and — far more important — owns
   the guarantee that it moves NOTHING ELSE.

   THE EDGES (design 4.2, restated only as far as the wake flow is
   concerned):

     day rollover   reverts, and does so through timer_reset()'s memset.
                    wake_flow.c must not add a second explicit clear:
                    APP_MODE_TIMERS is 0, the memset already lands on it,
                    and a redundant clear is a second site to keep in step.
     break end      reverts, at BOTH points where the latch is consumed.
     emptied list   reverts, at paint time, when a stored chore mode meets
                    a list that is no longer configured.
     the final ack  does NOT revert. This is the one that gets written
                    backwards, because "all done, back to timers" is the
                    intuitive behaviour and is explicitly not what the
                    design wants: bouncing out on the last tick would make
                    a mis-press cost a trip back through the mode button.

   AND THE NON-EDGE, which is the part worth more than all four: the mode
   is a RENDER SELECTOR. Every wake still does its day rollover, its
   bedtime gate, its tick, its break-end drain and its network window
   whichever screen is showing. The tempting implementation is an early
   return into a chore-screen handler, and that exact shape has already
   stranded the break-end latch once in this codebase — and `take` in
   timer_break_take_ended is a CONSUMING read, so a stranded edge is lost
   permanently rather than merely deferred. Hence
   test_c16_a_chore_mode_tick_wake_runs_exactly_the_same_effects, which
   compares two whole wakes event for event. */

/* The effect log with the one legitimately mode-dependent entry removed,
   so two wakes in different modes can be compared as sequences. EV_SET_MODE
   is the ONLY event allowed to differ; anything else that varies with the
   mode is the bug this section exists to catch. */
static int flow_log_without_mode_stores(flow_event_t *out, int cap) {
    int n = 0;
    for (int i = 0; i < flow_log_n && n < cap; i++) {
        if (flow_log[i] == EV_SET_MODE)
            continue;
        out[n++] = flow_log[i];
    }
    return n;
}

/* A deliberately BUSY tick wake, armed identically in either mode: a
   break that has already ended (the latch this section is really about),
   an NTP sync that is due, and a configured chore list so the
   emptied-list guard stays out of it. Everything else is setUp's default
   deep-sleep tick wake. */
static void flow_arm_busy_tick_wake(app_mode_t mode) {
    flow_tick_clock(flow_at(15, 0));
    flow_last_ntp = flow_at(15, 0) - 4 * 3600; /* well past IDLE_SYNC_INTERVAL_SEC */
    flow_mode = mode;
    flow_chore_count = 3;
    flow_chore_outstanding = 1;
    flow_arm_break(flow_at(14, 59), FLOW_PIANO, FLOW_SCREEN);
}

/* --- the break-end edge --- */

void test_c16_a_break_end_takes_the_mode_back_to_timers(void) {
    mock_time_set(flow_at(15, 0));
    flow_mode = APP_MODE_CHORES;
    flow_arm_break(flow_at(14, 59), FLOW_PIANO, FLOW_SCREEN);

    TEST_ASSERT_TRUE(wake_flow_break_end());

    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_TIMERS, (int)flow_mode,
                                  "a break ended and left the device painting the chore screen");
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_SET_MODE));
}

/* The silent end — an extra timer is running, so wake_policy_break_chime
   refuses and the owner returns early down a second path. The revert has
   to sit ABOVE that branch: the break is just as over either way, the
   wake-sticky flag has already been set so the panel gets a full refresh
   either way, and a revert that only fired when the chime did would leave
   chore mode standing on exactly the wake that repaints without
   explaining itself. */
void test_c16_a_silently_ended_break_reverts_the_mode_too(void) {
    mock_time_set(flow_at(15, 0));
    flow_mode = APP_MODE_CHORES;
    flow_extra_running = true; /* the violin is going: no chime */
    flow_arm_break(flow_at(14, 59), FLOW_PIANO, FLOW_SCREEN);

    TEST_ASSERT_TRUE(wake_flow_break_end());

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_CHIME)); /* the premise */
    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_TIMERS, (int)flow_mode,
                                  "the revert was hung off the chime instead of off the break end");
}

/* The guard, and the mutation this section most wants to kill: a
   timer_set_mode() hoisted ABOVE the take fires on every wake, which
   would make chore mode impossible to stay in for more than one wake and
   would look exactly like "the toggle does not work" in the field. */
void test_c16_a_wake_with_no_break_edge_leaves_the_mode_alone(void) {
    mock_time_set(flow_at(15, 0));
    flow_mode = APP_MODE_CHORES;
    /* No break running and nothing latched: the take returns false. */

    TEST_ASSERT_FALSE(wake_flow_break_end());

    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_CHORES, (int)flow_mode, "a wake with no break end still moved the mode");
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_SET_MODE));
}

/* The SECOND consumption site, and the reason it needs one: `take` is a
   consuming read, so an edge eaten here is gone. If only the owner
   reverted, a break that ended after the last drain point in the wake —
   during the pre-sleep event watch, or inside the second network window —
   would be consumed by this safety net with the mode left in chores, and
   nothing afterwards would ever revert it. "A consumed break end always
   leaves Timers" then holds at both sites rather than at one.

   This is the one side effect the raw take is allowed, and the case above
   it (test_row13_the_sleep_drain_has_no_side_effects_beyond_the_take)
   carries the boundary: the four things forbidden there are all effects
   on THIS wake's panel, audio or selection, landing after the panel has
   finished. An RTC byte is not one of those — it is read by the NEXT
   wake's paint, which is the earliest moment anything could act on it
   anyway. It is also safe from the awake failsafe's esp_timer context,
   where the forbidden four are not: a single-byte store into RTC memory
   takes no lock and allocates nothing. */
void test_c16_an_undrained_break_end_at_sleep_still_reverts(void) {
    mock_time_set(flow_at(15, 0));
    flow_mode = APP_MODE_CHORES;
    flow_latched = true; /* an edge nothing serviced this wake */
    flow_latched_wall = flow_at(14, 59);

    TEST_ASSERT_TRUE(wake_flow_report_undrained_break_end());

    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_TIMERS, (int)flow_mode,
                                  "a break end consumed at sleep left chore mode standing forever");
}

void test_c16_a_sleep_with_no_undrained_edge_leaves_the_mode_alone(void) {
    mock_time_set(flow_at(15, 0));
    flow_mode = APP_MODE_CHORES;

    TEST_ASSERT_FALSE(wake_flow_report_undrained_break_end());

    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_CHORES, (int)flow_mode,
                                  "every sleep reverted the mode, not just a drained one");
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_SET_MODE));
}

/* --- the day-rollover edge --- */

/* Two assertions, and the second is the point. The rollover reverts —
   but through timer_reset()'s memset, which this suite's stub models
   (see its comment), NOT through a clear wake_flow.c writes itself.
   M1-T6 established that adding an explicit clear here changes no test
   and buys a second site to keep in step with a struct that has already
   grown fields once, so the absence is pinned rather than left to
   reviewer memory. */
void test_c16_a_day_rollover_reverts_the_mode_through_timer_reset(void) {
    time_t now = flow_at(0, 1);
    mock_time_set(now);
    flow_mode = APP_MODE_CHORES;
    flow_new_day = true;
    flow_restore_ok = false; /* a genuine date change, not a power cycle */

    wake_flow_handle_day_rollover(&now);

    TEST_ASSERT_TRUE(flow_reset_called); /* the premise: the reset path ran */
    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_TIMERS, (int)flow_mode,
                                  "a day rollover left the device painting chore mode");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        0, flow_log_count(EV_SET_MODE),
        "the rollover clears the mode via the timer_reset memset; a second explicit clear was added");
}

/* Row 13's path: the date looked new but a same-day NVS snapshot came
   back, so no day actually rolled over and the reset never runs. Nothing
   about that is a reason to throw the user off the chore screen — it is a
   power cycle with a corrected clock, not midnight. */
void test_c16_a_same_day_restore_is_not_a_rollover_and_keeps_chore_mode(void) {
    time_t now = flow_at(0, 1);
    mock_time_set(now);
    flow_mode = APP_MODE_CHORES;
    flow_new_day = true;
    flow_restore_ok = true; /* the snapshot wins */

    wake_flow_handle_day_rollover(&now);

    TEST_ASSERT_FALSE(flow_reset_called); /* the premise */
    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_CHORES, (int)flow_mode,
                                  "a same-day restore threw the user off the chore screen");
}

/* --- the emptied-list edge --- */

/* Design 4.2 calls this "a guard at paint time, not a stored revert",
   and the plan's M2-T2 row calls it a revert; it is written as both,
   inside the wake flow's one paint-time choke point, and this case is why
   that is not a fudge. The guard STORES as well as fixing up the state it
   is handed, because timer_mode() is what M2-T3's Button A toggle will
   read: a render-only fallback would leave the stored byte saying CHORES
   while the panel says Timers, and the first press of A would toggle from
   the stale value back to Timers — i.e. appear to do nothing. Both halves
   are asserted here for that reason. */
void test_c16_an_emptied_chore_list_reverts_the_mode_at_paint_time(void) {
    mock_time_set(flow_at(15, 0));
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 0; /* the list was emptied from Home Assistant */

    wake_flow_repaint_current_state();

    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_TIMERS, (int)flow_mode, "the emptied list left the stored mode in chores");
    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_TIMERS, (int)flow_painted_mode,
                                  "the emptied list painted a chore screen with no chores on it");
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_SET_MODE));
}

/* Idempotent, and cheaply so: the second paint finds the mode already
   reverted and stores nothing. A guard that fired unconditionally would
   be invisible on device and would show up here as two stores. */
void test_c16_the_emptied_list_guard_stores_once_not_on_every_paint(void) {
    mock_time_set(flow_at(15, 0));
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 0;

    wake_flow_repaint_current_state();
    wake_flow_repaint_current_state();

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_SET_MODE));
}

/* The state every device in the field is in today, and the reason the
   guard tests the MODE as well as the count: no chores configured, the
   timer screen showing. There is nothing to revert, so nothing may be
   stored. A guard written as `chore_count == 0` alone is behaviourally
   invisible — it would only ever store the value already there — but it
   would put an RTC write in the path of every paint on every device that
   has never used the feature, which is all of them. */
void test_c16_the_shipped_default_paints_timers_and_stores_nothing(void) {
    mock_time_set(flow_at(15, 0));
    /* setUp's defaults ARE the shipped default: APP_MODE_TIMERS, no list. */

    wake_flow_repaint_current_state();

    TEST_ASSERT_EQUAL_INT(APP_MODE_TIMERS, (int)flow_painted_mode);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_SET_MODE),
                                  "the emptied-list guard stores on every paint of a device with no chores configured");
}

/* THE ONE THAT GETS WRITTEN BACKWARDS. Every chore is ticked, the day has
   released, the screen says "Screen time unlocked" — and the mode stays
   in chores, waiting for A. The fixture sets chore_outstanding to 0 and
   chore_released to true precisely so that a guard keyed on either of
   them, rather than on the count, fails here instead of shipping. */
void test_c16_the_final_ack_does_not_revert_the_mode(void) {
    mock_time_set(flow_at(15, 0));
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_chore_outstanding = 0; /* all three ticked */
    flow_chore_released = true; /* the day's remainder has been granted */

    wake_flow_repaint_current_state();

    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_CHORES, (int)flow_mode, "the last ack bounced the user out of chore mode");
    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_CHORES, (int)flow_painted_mode, "the last ack painted the timer screen");
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_SET_MODE));
}

/* --- the non-edge: the mode selects the paint and nothing else --- */

/* THE HEADLINE CASE. The same busy wake run twice, once in each mode,
   compared event for event with only the mode stores filtered out. A
   break that has ended, a sync that is due, a grid wait, a tick, a paint,
   the post-render break re-check, the pre-sleep watch, the OTA tail and
   the sleep — if any of that learns to care which screen is showing, the
   sequences diverge and this fails.

   setUp() is called between the runs rather than the fixture being
   unpicked by hand: it is a plain function, it resets the mock clock, the
   latch, the effect log and every fixture variable, and re-deriving the
   second run from a hand-rolled subset is exactly how a case like this
   rots into comparing two different wakes. */
void test_c16_a_chore_mode_tick_wake_runs_exactly_the_same_effects(void) {
    flow_event_t timers_log[1024];
    int timers_n;

    flow_arm_busy_tick_wake(APP_MODE_TIMERS);
    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());
    timers_n = flow_log_without_mode_stores(timers_log, (int)(sizeof timers_log / sizeof timers_log[0]));
    TEST_ASSERT_GREATER_THAN_INT(10, timers_n); /* the premise: a wake worth comparing */

    setUp();

    flow_arm_busy_tick_wake(APP_MODE_CHORES);
    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    flow_event_t chores_log[1024];
    int chores_n = flow_log_without_mode_stores(chores_log, (int)(sizeof chores_log / sizeof chores_log[0]));

    TEST_ASSERT_EQUAL_INT_MESSAGE(timers_n, chores_n,
                                  "chore mode changed how much the wake did, not just what it painted");
    for (int i = 0; i < timers_n; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)timers_log[i], (int)chores_log[i],
                                      "chore mode changed the wake's effect sequence");
    }
}

/* The same guarantee stated directly at the latch, because the sequence
   comparison above would still pass if BOTH wakes stranded the edge.
   Three separate observations, because they fail differently: the chime
   is the audible half, the snap back is the visible half, and the second
   take is the one that proves the latch was CONSUMED rather than left for
   a safety net to find. */
void test_c16_a_chore_mode_tick_wake_still_drains_the_break_end(void) {
    flow_arm_busy_tick_wake(APP_MODE_CHORES);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_CHIME), "a chore-mode wake swallowed the break-over chime");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_SNAP_BACK),
                                  "a chore-mode wake skipped the snap back to the interrupted timer");
    TEST_ASSERT_TRUE_MESSAGE(wake_flow_break_ended_this_wake(), "a chore-mode wake did not mark the break as ended");
    TEST_ASSERT_FALSE_MESSAGE(wake_flow_report_undrained_break_end(),
                              "a chore-mode wake left the break-end latch for the sleep safety net");
}

/* And on the other entry point. A button wake reaches the drain through a
   different prologue (the held-through-sleep guard, the EXT1 decode, the
   immediate LED ack), so it is a separate path to strand the latch on. */
void test_c16_a_chore_mode_button_wake_still_drains_the_break_end(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_wakeup_btn = BTN_B;
    flow_arm_break(flow_at(14, 59), FLOW_PIANO, FLOW_SCREEN);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_CHIME),
                                  "a chore-mode button wake swallowed the break-over chime");
    TEST_ASSERT_FALSE_MESSAGE(wake_flow_report_undrained_break_end(),
                              "a chore-mode button wake left the break-end latch undrained");
}

/* Row C17. An unattended wake — the RTC alarm, nobody in the room —
   repaints whatever screen is showing and leaves the NeoPixels dark. The
   pixel half is already true by construction (the LED call is gated on
   the reset reason not being a deep-sleep exit) and this is what stops
   M2-T8 from quietly contradicting it while adding ack feedback: the
   pixels are a BUTTON-wake affordance, and a chore screen on the panel is
   not a reason to light them. */
void test_c17_an_unattended_chore_mode_wake_repaints_with_the_pixels_dark(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_chore_outstanding = 2;
    flow_reset_reason = ESP_RST_DEEPSLEEP; /* the alarm, not a power-on */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, flow_log_count(EV_FULL_REFRESH) + flow_log_count(EV_PARTIAL),
                                         "an unattended chore-mode wake painted nothing at all");
    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_CHORES, (int)flow_painted_mode,
                                  "an unattended wake painted the timer screen over the chore screen");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_LED), "an unattended chore-mode wake lit the pixels");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_CHORE_LEDS),
                                  "an unattended chore-mode wake painted the chore strip");
}

/* ---- M2-T8: the NeoPixel ack sequencing (design §2.5) -------------------

   §2.5 moves the acknowledgement OFF the panel and onto the pixels,
   because the panel cannot be the fast path: a ghost-cleaned partial is
   an estimated ~0.8 s of CPU-holding refresh (FLOW_PANEL_COST_MS), a full
   one ~3 s (also an estimate), and
   since M2-T15 the panel also waits a whole quiet window for the gesture
   to settle before it starts. So the sequence is the feature:

     a button wake paints the PRE-PRESS strip first, so you see what the
     device thought before you touched it;
     STATUS_LED_ACK_HOLD_MS later the pressed row flips red -> green, and
     THAT TRANSITION IS THE ACK;
     the panel's partial runs behind it;
     and a second press in the same awake window flips INSTANTLY, because
     the starting state has already been seen.

   Every one of those four clauses fails silently in the field — the box
   still gets ticked, the panel still repaints, and only the FEEL is
   wrong — so each gets a case that fails for its own reason.

   The helper below is the setup those cases share. Three chores, none
   ticked, chore mode, and an ack that lands: the situation a child is in
   at the start of a day. */
static void flow_arm_chore_wake(button_id_t wake_btn) {
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_chore_acked = 0; /* nothing ticked yet: three red rows and a red gate */
    flow_ack_result = BTN_ACK_TOGGLED;
    flow_wakeup_btn = wake_btn;
}

/* Clause 1. The FIRST thing a button wake paints is the state the press
   has not yet changed — which is only observable because the ack stub
   moves the mask, so the pre-press frame and the flip carry different
   bytes. An implementation that painted once, after the apply, passes
   every count-based assertion in this file and fails here. */
void test_t8_a_chore_mode_button_wake_paints_the_pre_press_state_first(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_B);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(
        2, flow_strip_n, "a chore-mode button wake did not paint the strip twice (pre-press, flip)");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, flow_strip_mask[0],
                                   "the first frame was not the PRE-press mask: the ack was painted before it landed");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(3, flow_strip_rows[0],
                                    "the pre-press frame did not carry the configured row count");
    TEST_ASSERT_FALSE_MESSAGE(flow_strip_released[0], "the pre-press frame claimed the day was already released");
}

/* Clause 2, the transition itself. ✓1 is BUTTON_CHORE_IDX_B == row 0, so
   the flip has to set bit 0 and nothing else. Inverting the flip — or
   painting the pre-press mask twice — dies here. */
void test_t8_the_flip_carries_the_row_the_press_ticked(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_B);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_GREATER_OR_EQUAL_INT(2, flow_strip_n);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE((uint8_t)(1u << BUTTON_CHORE_IDX_B), flow_strip_mask[1],
                                   "the flip did not carry the row the press ticked");
    /* And the LAST frame of the wake still does: a repaint later in the
       tail must not put the pre-press state back on the strip. */
    TEST_ASSERT_EQUAL_HEX8_MESSAGE((uint8_t)(1u << BUTTON_CHORE_IDX_B), flow_strip_mask[flow_strip_n - 1],
                                   "something later in the wake repainted the strip with the pre-press mask");
}

/* Clause 2's timing. The hold is the whole reason the pre-press frame is
   worth painting — without it both frames land in the same instant and
   nobody sees a transition, only a final state. Read as the gap BETWEEN
   the two frames rather than as an absolute, so the case keeps meaning
   the same thing if anything ahead of the press ever delays. */
void test_t8_the_first_ack_of_a_wake_holds_the_pre_press_state_before_it_flips(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_B);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_GREATER_OR_EQUAL_INT(2, flow_strip_n);
    TEST_ASSERT_EQUAL_INT(1, flow_ack_n);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(STATUS_LED_ACK_HOLD_MS, flow_strip_at[1] - flow_strip_at[0],
                                     "the pre-press state was not held before the flip");
    /* The same gap, read from the apply rather than from the previous
       frame: the hold has to sit between the press landing and the pixel
       moving, not merely somewhere earlier in the wake. */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(STATUS_LED_ACK_HOLD_MS, flow_strip_at[1] - flow_ack_at[0],
                                     "the hold did not sit between the ack and its flip");
}

/* The same figure Button B's start/resume has held since long before the
   checklist existed, and the point of the case is that it is ONE figure:
   two constants for the same human-perceptible interval on the same
   device would be an inconsistency no user could interpret, and nothing
   but a test can stop them drifting apart. Measured on both paths rather
   than asserted against a literal — a literal would still pass with the
   two sites hard-coding different numbers. */
void test_t8_the_ack_hold_is_one_figure_shared_with_button_bs_start(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_B);
    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());
    TEST_ASSERT_GREATER_OR_EQUAL_INT(2, flow_strip_n);
    const uint32_t chore_hold = flow_strip_at[1] - flow_strip_at[0];

    /* Now the timer path: same button, no chore list, a start that lands. */
    setUp();
    mock_time_set(flow_at(15, 0));
    flow_b_result = BTN_B_STARTED;
    flow_net_open = false; /* no window: the hold is the only delay on the path */
    TEST_ASSERT_TRUE(flow_dispatch(BTN_B, flow_at(15, 0), TIMER_IDLE, true));
    const uint32_t timer_hold = mock_delay_total_ms();

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        timer_hold, chore_hold, "the chore ack and Button B's start hold the pre-press colour for different times");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(STATUS_LED_ACK_HOLD_MS, timer_hold,
                                     "Button B's start no longer holds for STATUS_LED_ACK_HOLD_MS");
}

/* Clause 4. Two chores ticked in one awake window is the case §2.5 names
   ("no homework today, and I just did the dishes"), and the second one
   must not pay the hold again — the starting state has already been
   seen, so a second pause is dead time in front of an ack.

   The break is still armed here, and WHAT IT IS FOR HAS CHANGED — say so,
   because the previous version of this comment named the break tail as
   "the one consumer a press made DURING a wake actually reaches", and that
   was true when it was written and is the defect the fix pass removed.
   Which consumer takes this particular press now depends on the quiet
   window, so the case deliberately does not say: it is the coalescing drain
   when the window reaches that far and the break tail's poll otherwise,
   and the assertions below hold either way because they are about the
   SECOND ack paying no hold rather than about who took it. The two cases
   further up pin the coalescing drain with no break armed at all. The
   break stays here because a press during a break is what design 2.6 cares
   about and this is the only case covering it — not because it is the
   mechanism. */
void test_t8_a_second_ack_in_the_same_wake_flips_with_no_hold(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_B);
    /* A break in its tail, so the poll loop runs and can take the press. */
    flow_arm_break(flow_at(15, 0) + 3, FLOW_PIANO, FLOW_SCREEN);
    flow_deferred_press_btn = BTN_C; /* ✓2 */
    flow_deferred_press_ms = 400;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(2, flow_log_count(EV_CHORE_ACK), "the second press never reached the ack");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(3, flow_strip_n, "the second ack did not repaint the strip");
    /* The frame carrying BOTH rows is the second ack's flip. Everything
       from there on is the same instant: no hold was paid for it. */
    int second_flip = -1;
    for (int i = 0; i < flow_strip_n; i++) {
        if (flow_strip_mask[i] == (uint8_t)((1u << BUTTON_CHORE_IDX_B) | (1u << BUTTON_CHORE_IDX_C))) {
            second_flip = i;
            break;
        }
    }
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, second_flip, "no frame carried both ticked rows");
    TEST_ASSERT_EQUAL_INT(2, flow_ack_n);
    /* Nothing between the apply and the flip. Measured against the apply
       and NOT against the previous frame: the press arrived through the
       break tail's 250 ms poll, so a frame-to-frame gap would read that
       poll as a hold and this case would pass on a device that paid one. */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(flow_ack_at[1], flow_strip_at[second_flip],
                                     "the second ack of the wake paid the pre-press hold again");
    /* And the first one still did, in the same run — so the case is about
       the DIFFERENCE between them and cannot pass by nobody holding. */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(STATUS_LED_ACK_HOLD_MS, flow_strip_at[1] - flow_ack_at[0],
                                     "the first ack of the wake stopped holding the pre-press state");
}

/* Clause 3, and the reason the whole feature exists: the pixels have to
   be AHEAD of the panel, not behind it. A flip written after the refresh
   would still show the right colours and would still pass every mask
   assertion above — it would just arrive a quiet window and a refresh
   late, which is precisely the latency §2.5 rejects. */
void test_t8_the_flip_reaches_the_pixels_before_the_panel_refresh(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_B);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    const int flip = flow_log_at_nth(EV_CHORE_LEDS, 2);
    const int painted = flow_log_at(EV_PARTIAL) >= 0 ? flow_log_at(EV_PARTIAL) : flow_log_at(EV_FULL_REFRESH);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(-1, flip, "the ack never flipped a pixel");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(-1, painted, "the ack never reached the panel");
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(painted, flip, "the panel repainted before the pixels acknowledged the press");
}

/* Row C17's second door. The first is pinned above for an unattended tick
   wake with nothing latched; this is the one that is easy to open by
   accident, because a press really did happen — it was latched during a
   previous wake's tail and is drained here — and an ack really is
   applied. But the wake was the RTC alarm, nobody is looking at the
   device, and lighting four LEDs on it is the battery cost §2.5's power
   discipline paragraph exists to refuse. */
void test_t8_a_latched_ack_on_a_tick_wake_leaves_the_strip_dark(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_NONE); /* no EXT1 button: this is the alarm */
    flow_reset_reason = ESP_RST_DEEPSLEEP;
    flow_press(BTN_B); /* latched before the wake, drained by the tick handler */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_CHORE_ACK), "the latched press never reached the ack");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_strip_n, "a tick wake lit the chore pixels (row C17)");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, mock_delay_total_ms(), "a tick wake paid the pre-press hold for nobody");
}

/* CHORE SLOT 2's pixel IS NP_STATE_PIXEL — status_led.c maps slot 2,
   button D's row, to pixel 0, which is the one status_led_show_timer_state()
   writes — so the two painters cannot both run on a wake. Pixel 0 held the
   GATE until M2-HW2 inverted the strip on 2026-09-22; the gate is pixel 3
   now, and this test's subject moved with it, because k_chore_pixel is a
   permutation and pixel 0 simply belongs to a different slot. What is
   guarded is unchanged: no timer colour on a claimed wake.
   status_led.h states it as a caller contract it cannot enforce; this is
   the enforcement. Every status paint in wake_flow.c goes through one
   wrapper, and on a wake the checklist has claimed, that wrapper repaints
   the checklist. */
void test_t8_a_chore_mode_wake_never_paints_a_timer_colour_over_a_chore_row(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_B);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_LED),
                                  "a chore-mode wake painted a timer colour over a chore row's pixel");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, flow_log_count(EV_CHORE_LEDS),
                                         "a chore-mode wake painted nothing on the strip at all");
}

/* And the other direction, which is the one a wrapper written too widely
   breaks: a device with no chore list — every device in the field today —
   must be completely untouched by any of this. */
void test_t8_a_timer_mode_button_wake_lights_the_timer_pixel_and_no_strip(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_mode = APP_MODE_TIMERS;
    flow_wakeup_btn = BTN_A;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, flow_log_count(EV_LED), "a timer-mode wake stopped lighting the pixel");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_strip_n, "a timer-mode wake painted the chore strip");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_led_claims, "a timer-mode wake took the sync pixel away from net_window");
}

/* THE FALSE RELEASE. net_window.c writes pixel 3 from its own task, and
   pixel 3 is THE GATE under status_led.c's mapping — it was chore slot 2's
   row until M2-HW2 inverted the strip on 2026-09-22, which moved the gate
   ONTO the sync pixel. Its NTP-success triple is (0,20,0) — byte for byte
   the checklist's "done" green — and on the gate that green means the day
   is RELEASED, the withheld time granted. So a sync landing while the
   checklist is up paints a perfectly convincing release that nobody
   earned, with nothing on the screen to contradict it.
   It is reachable on a BUTTON wake, which is the only kind that lights
   these pixels: the day rollover opens a window from inside the button
   handler's own prologue. So the claim has to be made BEFORE the
   rollover runs, not merely somewhere in the wake. */
void test_t8_the_checklist_claims_the_sync_pixel_before_the_rollover_opens_a_window(void) {
    flow_tick_clock(flow_at(0, 1));
    flow_arm_chore_wake(BTN_B);
    flow_new_day = true; /* the first press after midnight */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_led_claims, "the checklist never claimed the sync pixel");
    const int claim = flow_log_at(EV_LED_CLAIM);
    const int window = flow_log_at(EV_TRY_WINDOW);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(-1, window, "the rollover did not open a window");
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(window, claim,
                                      "a network window opened before the checklist claimed the sync pixel");
    /* And before the strip is painted, so no frame is ever exposed to it. */
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(flow_log_at(EV_CHORE_LEDS), claim,
                                      "the strip was painted before the sync pixel stood down");
}

/* The expiry alarm owns EVERY pixel while it runs and clears all four on
   its way out (neopixel.h), so anything the checklist had painted is
   gone afterwards — and the wake CONTINUES: the alert repaints the panel
   and returns. Left alone, the checklist would be on the glass with a
   dead strip under it for the rest of the wake.
   Reached the way production reaches it in chore mode: the window's
   config reconcile moves the active slot to EXPIRED under the press, so
   the post-join re-render answers EXPIRY_ALERT. */
void test_t8_an_expiry_alarm_repaints_the_strip_it_wiped(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_B);
    flow_state = TIMER_PAUSED;               /* a slot a shortened definition can expire */
    flow_state_after_finish = TIMER_EXPIRED; /* ...and the join does exactly that */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    const int alarm = flow_log_at(EV_ALERT_EXPIRY);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(-1, alarm, "the expiry alarm never fired");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(alarm, flow_log_at_nth(EV_CHORE_LEDS, flow_strip_n),
                                         "the alarm wiped the chore strip and nothing repainted it");
}

/* A refusal — a two-chore list has no ✓3 — is not an acknowledgement, so
   it owes the strip nothing: no hold, and no repaint that would flash the
   same frame twice for a press that did nothing. The strip a refused
   press leaves behind is the pre-press one it arrived to. */
void test_t8_a_refused_ack_neither_holds_nor_flips(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_D);
    flow_chore_count = 2;           /* no third row... */
    flow_ack_result = BTN_ACK_NONE; /* ...so the apply refuses */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_CHORE_ACK), "the press never reached the ack");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, mock_delay_total_ms(), "a refused ack paid the pre-press hold");
    for (int i = 0; i < flow_strip_n; i++) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, flow_strip_mask[i], "a refused ack moved a row on the strip");
    }
}

/* ---- M2-T8 fix pass: the second ack was DISCARDED, not delayed ----------

   §2.5 clause 4 ("subsequent presses in the same awake window flip
   instantly") shipped true only on the break screen. Off it, the EXT1
   handler's tail took the latch with a bare `buttons_take_pressed();`
   whose return went nowhere, and nothing else on the path consumes an ack
   — so a press during the pre-press hold was eaten there, and a press
   during the panel refresh sat in a BSS latch until deep sleep threw it
   away. Both are the box never getting ticked, with nothing on screen or
   on the strip to say so.

   THE REASON NO CASE CAUGHT IT: this suite's panel was free. The clock
   moves only inside hal_delay_ms(), which is also where the deferred press
   is delivered, so a refresh costing 0 ms meant the stretch a press
   vanishes into did not exist here and the press could not be put into it.
   The only second-press case in the file had to arm a BREAK to manufacture
   a consumer, and its own comment recorded that the EXT1 drain discards
   the latch. flow_display_ms is what closes that hole; every case below
   opts into it, and each one fails on the shipped code — the first two by
   losing the press outright, which flow_deferred_delivered proves was
   actually made. */

/* THE BLOCKER, at its plainest: a press made while the panel is busy must
   still tick its box. Armed past the first quiet window on purpose, so it
   lands inside the refresh itself and can only be answered by a take after
   it — which is the half of the fix a longer window cannot buy.

   RE-ANCHORED BY M2-T15. It used to be armed past the whole burst budget,
   which put it inside a ~1.9 s refresh; at the ~0.8 s this suite now
   charges (FLOW_PANEL_COST_MS, an estimate rather than a board figure) the
   paint is over before then and the press would never be made. Half-way through
   the first paint is the point this case is about. */
void test_t8_a_press_made_during_the_panel_refresh_is_not_discarded(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_B);
    flow_display_ms = FLOW_PANEL_COST_MS; /* the panel costs what it costs */
    flow_deferred_press_btn = BTN_C;      /* ✓2, mid-refresh */
    flow_deferred_press_ms =
        (int32_t)(STATUS_LED_ACK_HOLD_MS + CONFIG_MAGTAG_CHORE_PAINT_QUIET_MS + FLOW_PANEL_COST_MS / 2);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    /* Not vacuous: the press was really delivered. Without this the case
       passes just as happily when no second press was ever made. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_deferred_delivered, "the second press was never delivered");
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, flow_log_count(EV_CHORE_ACK),
                                  "a press made during the panel refresh was thrown away at sleep");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0, flow_latch_residue(), "the press was left in a latch deep sleep discards");
    /* And it reached the pixels, which is the only feedback §2.5 leaves. */
    bool both_rows = false;
    for (int i = 0; i < flow_strip_n; i++) {
        if (flow_strip_mask[i] == (uint8_t)((1u << BUTTON_CHORE_IDX_B) | (1u << BUTTON_CHORE_IDX_C))) {
            both_rows = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(both_rows, "the second ack never reached the strip");
}

/* Clause 4 off the break screen, and the COALESCING half: a press that
   arrives while the window is open must tick its box AHEAD of the panel
   and share the one refresh, not trigger a second one. Two chores in one
   go is the case §2.5 names by example; a refresh per ack is what it
   rejects. */
void test_t8_a_second_ack_coalesces_into_the_same_refresh_with_no_break(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_B);
    flow_display_ms = FLOW_PANEL_COST_MS;
    flow_deferred_press_btn = BTN_C;
    /* Inside the window: after the hold, before it closes. */
    flow_deferred_press_ms = (int32_t)STATUS_LED_ACK_HOLD_MS + 10;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_deferred_delivered, "the second press was never delivered");
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, flow_log_count(EV_CHORE_ACK), "the second ack never landed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_PARTIAL) + flow_log_count(EV_FULL_REFRESH),
                                  "two acks in one wake cost two panel refreshes");
    /* Both flips ahead of the single paint — the whole point of §2.5. */
    const int second_flip = flow_log_at_nth(EV_CHORE_LEDS, 3);
    const int painted = flow_log_at(EV_PARTIAL) >= 0 ? flow_log_at(EV_PARTIAL) : flow_log_at(EV_FULL_REFRESH);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(-1, second_flip, "the second ack never flipped a pixel");
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(painted, second_flip, "the second flip landed behind the panel refresh");
}

/* TWO BOXES TICKED INSIDE ONE WINDOW, which is the case the existing latch
   drains cannot serve and the reason the coalescing take asks for a MASK
   instead of asking button_latch_pick(): pick answers with one button and
   the take that feeds it clears every bit, so the other press is destroyed
   by the very call that was meant to service it (button_latch.c's priority
   list is B > C > D > A, so C is the one that would go). Here each latched
   bit gets its own apply and therefore its own flip. */
void test_t8_two_acks_latched_in_one_window_both_land(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_B);
    flow_press(BTN_C); /* ✓2 and ✓3 both latched before the take */
    flow_press(BTN_D);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(3, flow_log_count(EV_CHORE_ACK),
                                  "a second press latched alongside the first was thrown away with it");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0, flow_latch_residue(), "a latched ack was left for deep sleep to discard");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(
        (uint8_t)((1u << BUTTON_CHORE_IDX_B) | (1u << BUTTON_CHORE_IDX_C) | (1u << BUTTON_CHORE_IDX_D)),
        flow_strip_mask[flow_strip_n - 1], "the strip does not show all three rows the wake ticked");
}

/* "THE ACK DRAIN LEAVES BUTTON A ALONE" USED TO HAVE NO CASE, on the
   argument that widening the masked take to a bare buttons_take_pressed()
   survived as a mutant "because the tail's own bare take discards whatever
   the drain left a few lines later". That sentence was the defect, written
   down as a reason: the bare take discarding A is a dead mode button. Since
   M2-T15's fix pass A has its own take (wake_flow_take_gesture_toggle), so
   the mask IS observable now — a bare take in the ack drain would swallow
   the A that take exists for — and the test_t15_button_a_* cases below are
   what fail on it.

   THE LIVE MODE TERM is observable too, and the case below is it. */

/* A config edit that empties the list mid-wake reverts the mode under the
   press (make_display_state's emptied-list guard), and a press arriving
   after that must NOT be routed to an ack: button_chore_ack_apply() would
   refuse it, and a refusal here consumes the press without acting on it.
   Which is why the drain's guard reads timer_mode() LIVE rather than
   trusting the flag set at the claim.

   Driven by a hook of its own because two things have to happen at two
   different points of one wake — the list emptying inside the coalescing
   window, so the render that follows performs the revert, and the press
   landing inside the panel refresh after it. */
static void flow_empty_the_list_then_press(void) {
    const uint32_t t = mock_delay_total_ms();
    if (t >= STATUS_LED_ACK_HOLD_MS && flow_chore_count != 0) {
        flow_chore_count = 0; /* the config payload lands on the network task */
    } else if (t >= 1000) {
        flow_press(BTN_B); /* and a child presses again during the refresh */
    }
}

void test_t8_a_mode_reverted_under_the_wake_stops_routing_presses_to_acks(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_B);
    flow_display_ms = FLOW_PANEL_COST_MS;
    mock_delay_set_hook(flow_empty_the_list_then_press); /* setUp reinstalls the default */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    /* The revert really happened, or the case proves nothing. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_TIMERS, (int)flow_mode,
                                  "the emptied list never reverted the mode, so there was nothing to guard against");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_CHORE_ACK),
                                  "a press made after the mode reverted was still routed to a chore ack");
}

/* THE WINDOW IS ITS OWN KNOB, and no longer the hold (M2-T15). Until then
   one figure did both jobs and this case was
   test_t8_the_coalescing_window_is_the_ack_hold_figure; the board showed
   the two wanting opposite things — a short hold, a long window — so they
   were split. Measured as the wake's whole delay total, the hold then
   exactly one window that closes empty, because that is observable without
   reaching inside the loop.

   WRITTEN AGAINST THE CONFIG SYMBOL, NOT wake_flow.c's CHORE_PAINT_QUIET_MS,
   and that choice is the case. The collapse this guards against is a
   one-line `#define CHORE_PAINT_QUIET_MS STATUS_LED_ACK_HOLD_MS` in
   wake_flow.c, and a figure written against that macro would move with it
   and pass. The build gives the two CONFIG symbols different values (170,
   610), so the collapse fails here. And ONE figure is a bare literal, on
   the M2-T12 lesson that a suite written only against the macros under
   test cannot see those macros move: 170 + 610 = 780. */
void test_t15_a_lone_ack_waits_the_quiet_window_not_the_hold(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_B);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_NOT_EQUAL_MESSAGE(STATUS_LED_ACK_HOLD_MS, CONFIG_MAGTAG_CHORE_PAINT_QUIET_MS,
                                  "the suite builds the hold and the window at one value, so it cannot tell them "
                                  "apart");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)STATUS_LED_ACK_HOLD_MS + CONFIG_MAGTAG_CHORE_PAINT_QUIET_MS,
                                     mock_delay_total_ms(),
                                     "a lone ack did not spend exactly the hold plus one quiet window");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(780u, mock_delay_total_ms(),
                                     "170 ms hold + 610 ms quiet window, as test/CMakeLists.txt builds them");
}

/* The BOUND, and it is a battery guard rather than a feel one: the idle
   window slides, so every landed press grants another one and without a
   hard cap a pad generating edges — or a child leaning on a button — holds
   the device awake for as long as it keeps producing them.

   M2-T12 MOVED THIS FROM A COUNT TO A TIME, and the case moved with it
   because the two bounds fail differently. A count of windows ran out on a
   full list before a mis-press could be corrected, which is the field
   report this task exists for; a budget of milliseconds bounds the thing
   that actually costs power and does not shrink as the list grows. So this
   asserts the wake's DELAY total, which is the bound itself, and the ack
   count only as corroboration that presses really did keep landing.

   Driven by its own delay hook rather than the one-shot deferred press,
   which is the only way to model a press arriving in EVERY poll. The hook
   stops itself well above anything the budget can reach, so a coalescer
   that never terminated fails an assertion instead of hanging the suite.

   ON ITS OWN COUNTER, and that is a correction: it used to stop on
   flow_log_count(EV_CHORE_ACK) < 200, and flow_log is 1024 entries that
   stop recording when full. Every applied ack logs several events, so the
   log fills before 200 acks are counted, the cap never trips, and a
   coalescer with its budget removed HUNG the suite — the very outcome
   this paragraph promised it could not have (M2-T15 mutation run). */
static int flow_window_presses;

static void flow_press_in_every_window(void) {
    if (flow_window_presses < 200) {
        flow_window_presses++;
        flow_press(BTN_B);
    }
}

void test_t8_the_coalescing_window_is_bounded_per_wake(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_B);
    flow_window_presses = 0;
    mock_delay_set_hook(flow_press_in_every_window); /* setUp reinstalls the default */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    /* The pre-press hold, then coalescing until the budget is gone — and
       not one poll more, which is what makes this a bound and not a
       tendency. A sliding window with no cap never reaches this line. */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)STATUS_LED_ACK_HOLD_MS + CHORE_ACK_COALESCE_BUDGET_MS,
                                     mock_delay_total_ms(),
                                     "a press in every poll did not stop the burst at the budget");
    /* Two acks are owed before any poll happens at all — the wake press
       itself, and the press made during the pre-press hold, which the first
       take collects for free precisely because it has already happened.
       Past those it is one press per poll, all the way to the budget —
       ROUNDED UP, because the budget need not divide by the poll quantum and
       the short final step the clamp makes is still a poll. */
    const int polls =
        (int)((CHORE_ACK_COALESCE_BUDGET_MS + CHORE_ACK_COALESCE_POLL_MS - 1) / CHORE_ACK_COALESCE_POLL_MS);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2 + polls, flow_log_count(EV_CHORE_ACK),
                                  "the burst did not land exactly one ack per poll up to the budget");
}

/* ---- M2-T15: the panel waits for the gesture to settle ------------------

   The board, with every checklist partial cleaning again: press, short
   pause, press gave TWO partials — one mid-gesture, one catching up. The
   pause outlasted the 400 ms window (which was the ack hold, one knob doing
   two jobs), the panel started, the next press landed inside the paint,
   and the tail repainted the moment the first paint finished. The cases
   below pin the three halves of the fix: the window slides, a press during
   a paint re-opens it rather than repainting at once, and the whole thing
   is bounded by the burst budget. */

/* Two later presses, each a fraction of a window after the one before and
   together more than a window after the first: gaps of 400 ms and 400 ms
   against a 610 ms window. */
#define FLOW_GESTURE_GAP_MS 400u
static int flow_gesture_presses;

static void flow_press_at_a_natural_pace(void) {
    static const button_id_t seq[] = {BTN_C, BTN_D};
    if (flow_gesture_presses >= (int)(sizeof seq / sizeof seq[0])) {
        return;
    }
    const uint32_t due = STATUS_LED_ACK_HOLD_MS + FLOW_GESTURE_GAP_MS * (uint32_t)(flow_gesture_presses + 1);
    if (mock_delay_total_ms() >= due) {
        flow_press(seq[flow_gesture_presses++]);
    }
}

/* THE WINDOW SLIDES: every ack that applies grants another full one, so a
   gesture paints once however long it runs, as long as no gap outlasts the
   window. Dies on a window that stops sliding — it closes 610 ms after the
   hold, the panel starts, the third press lands inside that paint, and
   the tail owes a second refresh. */
void test_t15_presses_less_than_a_window_apart_share_one_paint(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_B);
    flow_chore_count = CHORE_MAX;
    flow_display_ms = FLOW_PANEL_COST_MS;
    flow_gesture_presses = 0;
    mock_delay_set_hook(flow_press_at_a_natural_pace); /* setUp reinstalls the default */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(2, flow_gesture_presses, "the harness never made both later presses");
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, flow_log_count(EV_CHORE_ACK), "a press of the gesture never reached the ack");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_PARTIAL) + flow_log_count(EV_FULL_REFRESH),
                                  "a gesture paced inside the quiet window cost more than one refresh");
    /* Worth making only if each gap is inside the window and the two
       together are not — otherwise a window that never slid would pass. */
    TEST_ASSERT_TRUE(FLOW_GESTURE_GAP_MS < CONFIG_MAGTAG_CHORE_PAINT_QUIET_MS);
    TEST_ASSERT_TRUE(2u * FLOW_GESTURE_GAP_MS > CONFIG_MAGTAG_CHORE_PAINT_QUIET_MS);
}

/* A PRESS DURING A PAINT RE-OPENS THE WINDOW, and the repaint waits for it
   to close. Before M2-T15 the tail repainted the moment it took the press;
   now the pixel flips at once and the panel waits a quiet window, so a
   person still pressing is coalesced into that repaint too. Pinned as the
   delay total, which is where the difference lives: the hold, a window,
   the paint, a SECOND window, the repaint. Dies on the immediate repaint
   (one window short). The literal is the M2-T12 lesson again: 170 + 610 +
   800 + 610 + 800 = 2990, as test/CMakeLists.txt and FLOW_PANEL_COST_MS
   build it. */
void test_t15_a_press_during_the_paint_waits_a_quiet_window_before_the_repaint(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_B);
    flow_display_ms = FLOW_PANEL_COST_MS;
    flow_deferred_press_btn = BTN_C;
    flow_deferred_press_ms =
        (int32_t)(STATUS_LED_ACK_HOLD_MS + CONFIG_MAGTAG_CHORE_PAINT_QUIET_MS + FLOW_PANEL_COST_MS / 2);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_deferred_delivered, "the second press was never delivered");
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, flow_log_count(EV_CHORE_ACK), "the press made during the paint was lost");
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, flow_log_count(EV_PARTIAL) + flow_log_count(EV_FULL_REFRESH),
                                  "the press made during the paint was never painted");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        (uint32_t)STATUS_LED_ACK_HOLD_MS + 2u * CONFIG_MAGTAG_CHORE_PAINT_QUIET_MS + 2u * FLOW_PANEL_COST_MS,
        mock_delay_total_ms(), "the repaint did not wait one quiet window after the press made during the paint");
    TEST_ASSERT_EQUAL_UINT32(2990u, mock_delay_total_ms());
}

/* A pad that fires once during every paint and never otherwise — the
   worst case for the tail, because every paint then owes another. Capped
   well above anything the budget allows, so a tail with no bound fails an
   assertion instead of hanging the suite. */
static int flow_paint_presses;

static void flow_press_during_every_paint(void) {
    if (flow_painting && flow_paint_presses < 50) {
        flow_paint_presses++;
        flow_press(BTN_B);
    }
}

/* THE M2-T12 RESIDUAL, retired: a press during the SECOND paint used to be
   latched and thrown away at deep sleep, because the tail took exactly
   once. Now every paint made while budget remains is followed by a take.
   Two presses, one in each of the first two paints, both land. */
static void flow_press_during_the_first_two_paints(void) {
    if (flow_painting && flow_paint_presses < 2) {
        flow_paint_presses++;
        flow_press(flow_paint_presses == 1 ? BTN_C : BTN_D);
    }
}

void test_t15_a_press_during_the_second_paint_is_not_discarded(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_B);
    flow_chore_count = CHORE_MAX;
    flow_display_ms = FLOW_PANEL_COST_MS;
    flow_paint_presses = 0;
    mock_delay_set_hook(flow_press_during_the_first_two_paints); /* setUp reinstalls the default */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(2, flow_paint_presses, "the harness never pressed during two paints");
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, flow_log_count(EV_CHORE_ACK),
                                  "a press made during the second paint was thrown away at sleep");
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, flow_log_count(EV_PARTIAL) + flow_log_count(EV_FULL_REFRESH),
                                  "each press made during a paint is owed a repaint");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0, flow_latch_residue(), "a press was left for deep sleep to discard");
}

/* THE BUDGET'S DEFINITION: the coalescer's own WAITING, summed across
   every quiet window of the wake, with the panel's time NOT counted — the
   firmware has no millisecond clock on this path to count it with. With a
   pad firing during every paint, the windows are 610, 610 and the 505
   left of the 1725 budget, each followed by a paint, and the loop stops
   after the paint that follows the budget running out: three paints. The
   press made during that last paint stays in the latch for deep sleep,
   which is the bound doing its job.

   So the delay total is the hold, the WHOLE budget, and three paints on
   top: 170 + 1725 + 3 x 800 = 4295. A budget that counted panel time would
   stop sooner; a tail with no bound would keep painting until the harness
   cap. Both fail here. */
void test_t15_the_burst_budget_does_not_count_panel_time(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_B);
    flow_display_ms = FLOW_PANEL_COST_MS;
    flow_paint_presses = 0;
    mock_delay_set_hook(flow_press_during_every_paint); /* setUp reinstalls the default */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    const int paints = flow_log_count(EV_PARTIAL) + flow_log_count(EV_FULL_REFRESH);
    const int windows = (int)((CHORE_ACK_COALESCE_BUDGET_MS + CONFIG_MAGTAG_CHORE_PAINT_QUIET_MS - 1) /
                              CONFIG_MAGTAG_CHORE_PAINT_QUIET_MS);
    TEST_ASSERT_EQUAL_INT_MESSAGE(windows, paints, "one paint per quiet window, and none past the budget's");
    TEST_ASSERT_EQUAL_INT(3, paints);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        (uint32_t)STATUS_LED_ACK_HOLD_MS + CHORE_ACK_COALESCE_BUDGET_MS + (uint32_t)paints * FLOW_PANEL_COST_MS,
        mock_delay_total_ms(), "the budget is the waiting alone: hold + budget + the paints on top");
    TEST_ASSERT_EQUAL_UINT32(4295u, mock_delay_total_ms());
    /* Every press but the last was taken and applied; the last is the one
       past the bound. */
    TEST_ASSERT_EQUAL_INT(paints, flow_paint_presses);
    TEST_ASSERT_EQUAL_INT(paints, flow_log_count(EV_CHORE_ACK)); /* the wake press + all but the last */
    TEST_ASSERT_EQUAL_HEX8_MESSAGE((uint8_t)(1u << BTN_B), flow_latch_residue(),
                                   "the press made during the post-budget paint was not left for sleep");
}

/* And a pad that fires CONTINUOUSLY — in every poll and every paint — gets
   exactly one paint past the budget: the window slides all the way to it,
   the first paint follows, the press made during that paint is owed one
   repaint with no window in front of it (none is left), and the loop
   stops.

   CAPPED ON ITS OWN COUNTER, not on flow_log_count(EV_CHORE_ACK) the way
   flow_press_in_every_window() is: flow_log is 1024 entries and stops
   recording when full, and with a paint in every iteration it fills long
   before 200 acks are logged — so a log-based cap never trips and a tail
   with no bound HANGS the suite instead of failing it (found by the
   M2-T15 mutation run). */
static int flow_pad_presses;

static void flow_press_in_every_delay(void) {
    if (flow_pad_presses < 200) {
        flow_pad_presses++;
        flow_press(BTN_B);
    }
}

void test_t15_a_continuous_pad_gets_one_paint_past_the_budget(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_B);
    flow_display_ms = FLOW_PANEL_COST_MS;
    flow_pad_presses = 0;
    mock_delay_set_hook(flow_press_in_every_delay); /* setUp reinstalls the default */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(2, flow_log_count(EV_PARTIAL) + flow_log_count(EV_FULL_REFRESH),
                                  "a continuous pad got more than one paint past the budget");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)STATUS_LED_ACK_HOLD_MS + CHORE_ACK_COALESCE_BUDGET_MS + 2u * FLOW_PANEL_COST_MS,
                             mock_delay_total_ms());
}

/* ---- M2-T15 fix pass: Button A during a chore gesture -------------------

   Review M1. Nothing on the button-wake path took A once an ack had
   landed: an A made before the first paint was eaten by the handler's bare
   stale-edge drain, and one made during a later window or paint sat in the
   latch until deep sleep discarded it. M2-T15's quiet window made that dead
   zone a second and more after every ack. The owner's scenario is the first
   case, verbatim: tick the last chore, see the gate go green, press A a
   moment later to go back to Timers — and nothing happened at all. */

/* THE OWNER'S SCENARIO. The last row is ticked (two already done, B ticks
   the third) and A is pressed well inside the quiet window — before the
   panel has painted anything. The mode must be Timers, the panel must paint
   the Timers screen, and it must be ONE paint, FULL: the toggle swaps the
   whole layout, and painting the checklist first only to replace it would
   be a wasted refresh. Dies on the shipped T15 code (A eaten by the bare
   take: mode still Chores, one partial of the checklist). */
void test_t15_button_a_after_the_last_tick_goes_back_to_timers(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_B);
    flow_chore_acked = (uint8_t)((1u << BUTTON_CHORE_IDX_C) | (1u << BUTTON_CHORE_IDX_D)); /* B's is the last */
    flow_display_ms = FLOW_PANEL_COST_MS;
    flow_deferred_press_btn = BTN_A;
    flow_deferred_press_ms = (int32_t)STATUS_LED_ACK_HOLD_MS + 300; /* inside the 610 ms window */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_deferred_delivered, "the A press was never delivered");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_CHORE_ACK), "the last tick never landed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_A_APPLY), "the A press never reached the toggle");
    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_TIMERS, (int)flow_mode, "A after the last tick left the device in Chores");
    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_TIMERS, (int)flow_painted_mode, "the panel never showed the Timers screen");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_FULL_REFRESH), "the layout swap was not a full refresh");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_PARTIAL), "the checklist was painted only to be replaced");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0, flow_latch_residue(), "a press was left for deep sleep to discard");
    /* A ENDS THE WINDOW: the paint follows the poll A landed in, not a full
       window after it. 300 is a whole number of 50 ms polls, so A lands at
       the end of the sixth and that poll's own take finds it. */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)STATUS_LED_ACK_HOLD_MS + 300u + FLOW_PANEL_COST_MS,
                                     mock_delay_total_ms(), "A did not end the quiet window");
}

/* The same press made DURING THE FIRST PAINT, which is the tail's half: the
   checklist is already on its way to the glass, so it is the tail that must
   take A and repaint — full, Timers. Dies on the shipped T15 tail, whose
   take was masked to the acks, so A sat in the latch until sleep. */
void test_t15_button_a_during_the_first_paint_goes_back_to_timers(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_B);
    flow_display_ms = FLOW_PANEL_COST_MS;
    flow_deferred_press_btn = BTN_A;
    flow_deferred_press_ms =
        (int32_t)(STATUS_LED_ACK_HOLD_MS + CONFIG_MAGTAG_CHORE_PAINT_QUIET_MS + FLOW_PANEL_COST_MS / 2);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_deferred_delivered, "the A press was never delivered");
    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_TIMERS, (int)flow_mode, "an A made during the paint was discarded");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_PARTIAL), "the checklist's own paint went missing");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_FULL_REFRESH), "A's repaint was not a full refresh");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(flow_log_at(EV_PARTIAL), flow_log_at(EV_FULL_REFRESH),
                                         "the Timers repaint did not come after the checklist");
    TEST_ASSERT_EQUAL_INT(APP_MODE_TIMERS, (int)flow_painted_mode);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0, flow_latch_residue(), "a press was left for deep sleep to discard");
    /* No quiet window in front of A's repaint: A ends the gesture. */
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)STATUS_LED_ACK_HOLD_MS + CONFIG_MAGTAG_CHORE_PAINT_QUIET_MS + 2u * FLOW_PANEL_COST_MS,
        mock_delay_total_ms());
}

/* A REFUSED A IS STILL REFUSED — button_a_toggle_allowed() says no (a RUNNING
   timer, or no list), so the press is consumed, nothing toggles, nothing is
   promoted, and the gesture carries on as if A had not been pressed: the
   window neither closes early nor slides. */
void test_t15_a_refused_button_a_during_the_gesture_changes_nothing(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_B);
    flow_a_allowed = false;
    flow_display_ms = FLOW_PANEL_COST_MS;
    flow_deferred_press_btn = BTN_A;
    flow_deferred_press_ms = (int32_t)STATUS_LED_ACK_HOLD_MS + 300;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_A_APPLY), "the refused A never reached its gate");
    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_CHORES, (int)flow_mode, "a refused A toggled the mode");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_FULL_REFRESH), "a refused A was promoted to a full refresh");
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_PARTIAL));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        (uint32_t)STATUS_LED_ACK_HOLD_MS + CONFIG_MAGTAG_CHORE_PAINT_QUIET_MS + FLOW_PANEL_COST_MS,
        mock_delay_total_ms(), "a refused A moved the quiet window");
}

/* AN ACK AND A IN THE SAME POLL: the ack first. Within one 50 ms poll the
   order of two presses is unknowable, and the two orders are not equally
   recoverable — an ack applied first is undone by pressing it again, while
   an ack routed AFTER the toggle is a Timers-mode press the handler's drain
   then drops (or, anywhere that acted on it, a timer started that nobody
   asked for). Dies on the order reversed (the ack is lost). */
static void flow_press_c_and_a_together(void) {
    if (flow_deferred_press_ms >= 0 && mock_delay_total_ms() >= (uint32_t)flow_deferred_press_ms) {
        flow_deferred_press_ms = -1;
        flow_press(BTN_C);
        flow_press(BTN_A);
    }
}

void test_t15_an_ack_and_a_in_one_poll_apply_the_ack_first(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_B);
    flow_display_ms = FLOW_PANEL_COST_MS;
    flow_deferred_press_ms = (int32_t)STATUS_LED_ACK_HOLD_MS + 300;
    mock_delay_set_hook(flow_press_c_and_a_together); /* setUp reinstalls the default */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, flow_deferred_press_ms, "the two presses were never made");
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, flow_log_count(EV_CHORE_ACK), "the ack made alongside A was lost");
    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_TIMERS, (int)flow_mode, "the A made alongside the ack was lost");
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_FULL_REFRESH));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PARTIAL));
}

/* The same pair made DURING THE FIRST PAINT, which the tail's take at the
   top of its loop collects — a different take from the window's, so it
   gets its own case (the window-only case survives that take's order
   being reversed). */
void test_t15_an_ack_and_a_made_during_a_paint_apply_the_ack_first(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_B);
    flow_display_ms = FLOW_PANEL_COST_MS;
    flow_deferred_press_ms =
        (int32_t)(STATUS_LED_ACK_HOLD_MS + CONFIG_MAGTAG_CHORE_PAINT_QUIET_MS + FLOW_PANEL_COST_MS / 2);
    mock_delay_set_hook(flow_press_c_and_a_together); /* setUp reinstalls the default */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, flow_deferred_press_ms, "the two presses were never made");
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, flow_log_count(EV_CHORE_ACK), "the ack made alongside A was lost");
    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_TIMERS, (int)flow_mode, "the A made alongside the ack was lost");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0, flow_latch_residue(), "a press was left for deep sleep to discard");
}

/* A OUTLIVES THE BUDGET, and only A. A pad that fires in every poll slides
   the window to the budget; the paint that follows owes one repaint for the
   press made during it; and an A made during THAT post-budget paint — the
   one stretch where every other press is left for sleep — still gets its
   toggle and its full repaint, because A ends the gesture and can apply
   once. Dies on a tail with no post-budget A take (A left in the latch,
   two paints). Own counters, never the log (see flow_press_in_every_delay). */
static int flow_budget_pad_presses;
static int flow_budget_paints;

static void flow_pad_then_a_during_the_post_budget_paint(void) {
    if (flow_painting) {
        flow_budget_paints++;
        if (flow_budget_paints <= 2) {
            flow_press(flow_budget_paints == 1 ? BTN_B : BTN_A); /* A's own paint gets nothing */
        }
    } else if (flow_budget_pad_presses < 200) {
        flow_budget_pad_presses++;
        flow_press(BTN_B);
    }
}

void test_t15_button_a_after_the_budget_still_goes_back_to_timers(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_B);
    flow_display_ms = FLOW_PANEL_COST_MS;
    flow_budget_pad_presses = 0;
    flow_budget_paints = 0;
    mock_delay_set_hook(flow_pad_then_a_during_the_post_budget_paint); /* setUp reinstalls the default */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(2, flow_budget_paints, "the harness never reached the post-budget paint");
    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_TIMERS, (int)flow_mode, "an A made after the budget was discarded");
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, flow_log_count(EV_PARTIAL), "the budget's own two paints changed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_FULL_REFRESH), "A past the budget was not painted full");
    TEST_ASSERT_EQUAL_INT(APP_MODE_TIMERS, (int)flow_painted_mode);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0, flow_latch_residue(), "a press was left for deep sleep to discard");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)STATUS_LED_ACK_HOLD_MS + CHORE_ACK_COALESCE_BUDGET_MS + 3u * FLOW_PANEL_COST_MS,
                             mock_delay_total_ms());
}

/* ---- M2-T15 fix pass: only the first tail repaint after a break end is full

   Review L3. s_break_ended is wake-sticky, so through the plain render
   every repaint of the chore tail after a break end was a full refresh — up
   to the whole paint bound of them in one gesture. The break ends (latched
   by timer.c) during the first paint; the ack made in that paint drains it
   and re-asserts chore mode, so its repaint is promoted — that is the one
   that shows the break end. The ack made during THAT paint is an ordinary
   tick of a box on a layout that has not moved, and must be a partial. Dies
   on the wake-sticky promotion (two fulls). */
static int flow_break_paints;

static void flow_break_ends_then_two_ticks(void) {
    if (!flow_painting || flow_break_paints >= 2) {
        return;
    }
    flow_break_paints++;
    if (flow_break_paints == 1) {
        flow_latched = true; /* the break's wall end passes during the first paint */
        flow_latched_wall = hal_time_now();
        flow_press(BTN_C);
    } else {
        flow_press(BTN_D);
    }
}

void test_t15_only_the_first_tail_repaint_after_a_break_end_is_full(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_B);
    flow_display_ms = FLOW_PANEL_COST_MS;
    flow_break_paints = 0;
    mock_delay_set_hook(flow_break_ends_then_two_ticks); /* setUp reinstalls the default */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(2, flow_break_paints, "the harness never pressed during two paints");
    TEST_ASSERT_TRUE_MESSAGE(wake_flow_break_ended_this_wake(), "the break end was never drained");
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, flow_log_count(EV_CHORE_ACK), "a tick of the gesture was lost");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_FULL_REFRESH),
                                  "the break end was not promoted on exactly the one repaint that showed it");
    TEST_ASSERT_EQUAL_INT(2, flow_log_count(EV_PARTIAL));
    /* And the full one is the repaint right after the drain, not the last. */
    TEST_ASSERT_LESS_THAN_INT(flow_log_at_nth(EV_PARTIAL, 2), flow_log_at(EV_FULL_REFRESH));
}

/* ---- M2-T15 fix pass: the post-join re-render across a screen change -----

   Review M2. finish_action_and_render()'s re-render built force_full with
   no screen-kind term, so a config edit applied during the network window
   that cancels a timer RUNNING in chore mode (MAIN -> checklist, since
   display_screen_for() suppresses CHORES while RUNNING) was painted as a
   PARTIAL — a whole-layout swap as a diff. Negative control inline: the
   policy still calls the pair partial, so the full refresh can only be the
   new term. Dies on the term removed. */
void test_m2_a_join_that_moves_the_screen_kind_repaints_full(void) {
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_state = TIMER_RUNNING; /* the timer screen, in chore mode */
    mock_time_set(flow_at(16, 0));
    flow_net_finish = NET_FINISH_CHANGED;
    flow_state_after_finish = TIMER_IDLE; /* the config edit cancelled it */

    TEST_ASSERT_EQUAL_INT_MESSAGE(
        WAKE_RENDER_PARTIAL, wake_policy_render(TIMER_RUNNING, TIMER_IDLE, true, false, false),
        "the policy no longer calls this pair partial - this test's negative control has gone vacuous");
    TEST_ASSERT_NOT_EQUAL(display_screen_for(TIMER_RUNNING, APP_MODE_CHORES, 3),
                          display_screen_for(TIMER_IDLE, APP_MODE_CHORES, 3));

    finish_action_and_render(BTN_B, TIMER_RUNNING, flow_at(16, 0), false);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_PARTIAL), "the first render was not the ordinary partial");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_FULL_REFRESH),
                                  "the post-join repaint across a screen change went partial");
    TEST_ASSERT_GREATER_THAN_INT(flow_log_at(EV_PARTIAL), flow_log_at(EV_FULL_REFRESH));
}

/* The other way the join moves the screen, and the reason the term compares
   against the screen the first render BUILT rather than asking
   display_screen_for() about `painted` with today's mode: a config payload
   that EMPTIES the list, with no toggle this wake, makes the re-render's
   emptied-list guard revert the mode and paint the timer screen over the
   checklist. The timer state never moves, so a state-only term asked with
   the current (reverted) mode sees MAIN on both sides and calls it no
   change. Dies on that form of the term. */
void test_m2_a_join_that_empties_the_list_repaints_full(void) {
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_state = TIMER_IDLE; /* the checklist */
    mock_time_set(flow_at(16, 0));
    flow_net_finish = NET_FINISH_CHANGED;
    flow_chores_after_finish = 0; /* the config edit emptied the list */

    finish_action_and_render(BTN_B, TIMER_IDLE, flow_at(16, 0), false);

    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_TIMERS, (int)flow_painted_mode,
                                  "the emptied list never reverted the screen, so there was nothing to promote");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_PARTIAL), "the first render was not the ordinary partial");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_FULL_REFRESH),
                                  "the checklist -> timer screen repaint after the join went partial");
}

/* And the half that keeps the term from being "any join change is full": a
   join that moves the state but not the screen stays partial. */
void test_m2_a_join_that_keeps_the_screen_stays_partial(void) {
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_state = TIMER_IDLE; /* the checklist */
    mock_time_set(flow_at(16, 0));
    flow_net_finish = NET_FINISH_CHANGED;
    flow_state_after_finish = TIMER_PAUSED; /* still the checklist */

    TEST_ASSERT_EQUAL_INT(WAKE_RENDER_PARTIAL, wake_policy_render(TIMER_IDLE, TIMER_PAUSED, true, false, false));

    finish_action_and_render(BTN_B, TIMER_IDLE, flow_at(16, 0), false);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_FULL_REFRESH), "a same-screen post-join repaint went full");
    TEST_ASSERT_EQUAL_INT(2, flow_log_count(EV_PARTIAL));
}

/* THE POLL QUANTUM, pinned to BUTTON_LATCH_DEBOUNCE_US and not to the
   coalescer's own macro — which is the difference between a case and a
   tautology. Every other figure here is written against
   CHORE_ACK_COALESCE_POLL_MS so it moves with the knob, and review found
   that a poll quantum mutated up to the whole idle window therefore failed
   NOTHING: the ack-per-poll arithmetic in the bound case moved with it.

   WHY THE DEBOUNCE IS THE RIGHT BOUND and not a feel figure: the latch is a
   BITMASK, so two presses of one button waiting in the same take collapse
   into one toggle. What stops that is not the mask — it is that two accepted
   edges on one button need a release observation between them
   (button_latch.h) and a release is only observed when somebody takes. Poll
   further apart than the debounce window and two presses can accumulate
   between takes; poll at it or under it and they cannot. So this asserts the
   floor: the idle window is covered by takes no further apart than that.

   A LOWER BOUND rather than an equality, because the entry take and any
   other masked take on the path are not this case's business — a coalescer
   that polled FINER than required is not the defect. */
void test_t12_the_coalescing_loop_polls_at_least_as_often_as_the_debounce(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_B);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    const uint32_t debounce_ms = BUTTON_LATCH_DEBOUNCE_US / 1000;
    /* The idle window is the QUIET window since M2-T15, not the hold. */
    const uint32_t floor_takes = ((uint32_t)CONFIG_MAGTAG_CHORE_PAINT_QUIET_MS + debounce_ms - 1) / debounce_ms;
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(floor_takes, (uint32_t)flow_mask_takes,
                                                "the coalescing loop polls further apart than the debounce window, so "
                                                "two presses of one button can collapse into one latched bit");
}

/* THE FIELD REPORT, and the one case that fails on everything shipped
   before M2-T12: "if I click a button, then click it again that should
   work. That'll happen if I misclick and want to put the state back."
   A toggle is its own inverse, so two presses of one button must leave the
   row exactly as it was found — and on a FULL list, where the old bound of
   CHORE_MAX windows was already spent by the three ticks.

   Two separate defects had to be fixed for this to pass and it fails on
   either alone: the old bound ran out before the fourth press could be
   granted a window, and one press latched TWICE (its release bounce) so the
   correction could not be told from the phantom it was competing with.

   WHAT THIS CASE DOES NOT PIN, said here because for a while it was believed
   to: the release gate's TIMING. Every touch of the latch clock here jumps
   ten debounce windows (flow_edge_step), so each press sits 500 ms clear of
   the level sample before it — a gap no polling device produces. A third
   defect therefore hid underneath a passing case, and
   test_t12_a_correction_press_is_accepted_at_a_real_poll_cadence below is
   the case that fails on it. */
static int flow_correcting_presses;

static void flow_tick_then_correct(void) {
    /* One press per poll: ✓1, ✓2, ✓3, then ✓3 again to put it back. */
    static const button_id_t seq[] = {BTN_C, BTN_D, BTN_D};
    if (flow_correcting_presses < (int)(sizeof seq / sizeof seq[0])) {
        flow_press(seq[flow_correcting_presses++]);
    }
}

void test_t12_a_mis_press_on_a_full_list_can_be_corrected_in_the_same_wake(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_B); /* ✓1 is the wake press */
    flow_chore_count = CHORE_MAX;
    flow_correcting_presses = 0;
    mock_delay_set_hook(flow_tick_then_correct); /* setUp reinstalls the default */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(3, flow_correcting_presses, "the harness never made all three later presses");
    /* FOUR presses, four applies: the fourth is the correction, and on the
       old window count there was no window left to collect it in. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(4, flow_log_count(EV_CHORE_ACK),
                                  "a repeat press on a full list never reached the ack");
    /* And they all shared the one refresh, which is what coalescing is for. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_PARTIAL) + flow_log_count(EV_FULL_REFRESH),
                                  "correcting a mis-press cost a second panel refresh");
}

/* THE SAME GESTURE AT THE CADENCE A DEVICE ACTUALLY PRODUCES, and the case
   that fails on the release anchor M2-T12 shipped first.
   On hardware the level sample that reopens the gate is taken by the same
   take that then consumes presses, so a press made while the panel waits
   lands 0-50 ms after a sample — never the 500 ms the default clock step
   above manufactures. The anchor on that sample was BUTTON_LATCH_DEBOUNCE_US
   wide to begin with, so every one of those presses was rejected as bounce
   and then lost outright at the next poll: the coarse step is the reason no
   case in this file could see it (see button_latch.h for the arithmetic).
   The gap below is seven tenths of a chatter window, which is the one place a
   press can sit and be unambiguous: past the settle anchor so the gate must
   accept it, inside the chatter window so the first anchor rejected it, and
   far enough apart that two presses of one button do not collapse into one
   latched bit on the press-edge window instead. All three are asserted,
   because a case that silently stops distinguishing them is how this defect
   got here. */
#define FLOW_EDGE_GAP_TIGHT_US ((int64_t)30000) /* 30 ms */

void test_t12_a_correction_press_is_accepted_at_a_real_poll_cadence(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_B); /* ✓1 is the wake press */
    flow_chore_count = CHORE_MAX;
    flow_correcting_presses = 0;
    flow_edge_gap_us = FLOW_EDGE_GAP_TIGHT_US;
    mock_delay_set_hook(flow_tick_then_correct); /* setUp reinstalls the default */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(3, flow_correcting_presses, "the harness never made all three later presses");
    TEST_ASSERT_EQUAL_INT_MESSAGE(4, flow_log_count(EV_CHORE_ACK),
                                  "a press made within a debounce window of the level sample that reopened the "
                                  "gate was rejected as bounce — the lost correction press of M2-T12");
    /* AFTER the assertion, because these only say whether it was worth
       making: a probe gap outside the band proves nothing either way. */
    TEST_ASSERT_TRUE_MESSAGE(FLOW_EDGE_GAP_TIGHT_US > BUTTON_LATCH_RELEASE_SETTLE_US,
                             "the probe gap no longer clears the settle anchor, so a failure here would be the "
                             "one rejection the gate is RIGHT to make");
    TEST_ASSERT_TRUE_MESSAGE(FLOW_EDGE_GAP_TIGHT_US < BUTTON_LATCH_DEBOUNCE_US,
                             "the probe gap no longer lands inside the chatter window, so this case passes "
                             "without distinguishing the two anchors");
    TEST_ASSERT_TRUE_MESSAGE(2 * FLOW_EDGE_GAP_TIGHT_US > BUTTON_LATCH_DEBOUNCE_US,
                             "two presses of one button are now closer together than the chatter window, so "
                             "they would collapse in the latch for a reason that is not the gate");
}

/* THE PHANTOM, on its own, because the case above would also pass if the
   fourth press were a bounce artefact rather than the press the harness
   made. One physical press is one falling edge when it goes down and, on a
   bouncing contact, another when it comes back UP — and that second edge
   arrives a whole hold duration after the first, so no debounce window
   measured from the press can reject it. Before M2-T12 the coalescer read
   it as a second press and toggled the row back, which is field finding 3:
   "toggling 3 flips 2 back to green and the screen renders as this".

   Modelled exactly as the hardware produces it: an edge on a button that
   has not been observed released since its last accepted press. The button
   is held throughout (flow_held_now), so no take can observe a release.

   ON ITS OWN COUNTER, like every other self-limiting hook in this file
   since M2-T15: capping on flow_log_count() is the pattern that hung the
   suite there, because flow_log holds 1024 entries and silently stops
   recording. This one could not saturate the log today, but a cap that is
   only safe because of how few events a passing run logs is not a cap. */
static int flow_bounce_edges;

static void flow_bounce_the_held_button(void) {
    if (flow_bounce_edges < 20) {
        flow_bounce_edges++;
        flow_press(BTN_B); /* release-bounce edges, never a new press */
    }
}

void test_t12_release_bounce_on_a_held_button_is_not_a_second_press(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_B);
    flow_held_now = 1u << BTN_B; /* still down: nobody can see it come up */
    flow_bounce_edges = 0;
    mock_delay_set_hook(flow_bounce_the_held_button);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_CHORE_ACK),
                                  "release bounce on the wake press was applied as a second ack");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)STATUS_LED_ACK_HOLD_MS + CONFIG_MAGTAG_CHORE_PAINT_QUIET_MS,
                                     mock_delay_total_ms(), "bounce edges granted the burst more windows");
}

/* The guard's three terms, from the side that costs battery: a wake that
   never acked must not spend a window waiting for a press nobody is
   making. A press of A in chore mode is the reachable case — it toggles
   OUT, acks nothing, and has no gesture to continue. */
void test_t8_a_wake_with_no_ack_spends_no_coalescing_window(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_A); /* the mode toggle, not an ack */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_CHORE_ACK), "Button A reached the ack");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, mock_delay_total_ms(), "a wake that acked nothing paid a coalescing window");
}

/* ---- M2-T8 fix pass: the strip claim is the PAINTER's answer ------------

   The claim used to key on the mode byte alone. The mode and the timer
   state are independent — button_actions.c gates an ack on the mode and
   the row count with no timer-state term — while display_screen_for()
   answers MAIN for a RUNNING timer whatever the mode says. So a press made
   during a running timer looked like nothing on the panel, and the NEXT
   button wake painted four chore colours over a TIMER screen with the
   RUNNING green on pixel 0 replaced by chore slot 2's. */
void test_t8_a_running_timer_keeps_its_pixel_even_in_chore_mode(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_B);
    flow_state = TIMER_RUNNING; /* chore mode, but the checklist is not the screen */
    flow_expiry_wall = (int64_t)flow_at(15, 30);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_strip_n, "the checklist painted its strip over a timer screen");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, flow_log_count(EV_LED),
                                         "a running timer lost its state pixel to the chore claim");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_led_claims, "the sync pixel was stood down on a timer screen");
}

/* The count term arrives with it, and is worth having on its own: chore
   mode with an emptied list is not the checklist either, so claiming there
   stood the sync pixel down and painted four dark pixels for nobody. */
void test_t8_an_empty_chore_list_claims_nothing(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_arm_chore_wake(BTN_B);
    flow_chore_count = 0; /* the shipped default: no list configured */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_strip_n, "an empty list painted the strip");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_led_claims, "an empty list stood the sync pixel down");
}

/* ---- M2-T3: Button A reaches the toggle, on every path (row C3) ---------

   Four separate things had to change before a press of A could do
   anything at all, and wake_flow.c owned three of them. Each has a case
   below, because each fails silently and identically in the field — the
   button does nothing — while every other test in this suite still
   passes:

     the EXT1 switch     wake_flow_handle_button_wake's decode had no
                         BTN_A case, so a wake caused by A fell to the
                         default arm and only repainted.
     the two pick masks  wake_flow_poll_break_buttons (the break tail) and
                         the tick-wake latch drain both passed an ALLOWED
                         mask of B|C, so button_latch_pick could never
                         return A whatever was latched.
     the dispatch arm    the arm itself, which logged at DEBUG and
                         returned false.

   The fourth is the wake mask in buttons_policy.c, which is where A is
   armed at sleep entry; test_buttons_policy owns that one. */

/* --- the dispatch arm --- */

void test_c3_button_a_toggles_the_mode_in_the_dispatch(void) {
    flow_mode = APP_MODE_TIMERS;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_A, flow_at(16, 0), TIMER_IDLE, true));

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_A_APPLY));
    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_CHORES, (int)flow_mode, "Button A did not reach the mode toggle");
    /* The out-param contract, which every arm owes: cleared, and the
       clock handed back untouched. A is a render selector — it starts no
       timer, so it opens no network window and steps no clock. */
    TEST_ASSERT_FALSE(flow_swapped_io);
    TEST_ASSERT_EQUAL_INT64(flow_at(16, 0), flow_now_io);
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_NET_OPEN));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_B_APPLY));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_SELECT_NEXT));
}

void test_c3_button_a_toggles_back_out_of_chore_mode(void) {
    flow_mode = APP_MODE_CHORES;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_A, flow_at(16, 0), TIMER_IDLE, true));

    TEST_ASSERT_EQUAL_INT(APP_MODE_TIMERS, (int)flow_mode);
}

/* A REFUSED press reports false and stores nothing — the same shape as a
   refused B or C. False is what stops the caller spending a paint on a
   press that did nothing; the mode assertion is what stops a refusal that
   toggled anyway. */
void test_c3_a_refused_button_a_reports_false_and_stores_nothing(void) {
    flow_a_allowed = false; /* a RUNNING timer, or no chores configured */
    flow_mode = APP_MODE_TIMERS;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_FALSE(flow_dispatch(BTN_A, flow_at(16, 0), TIMER_RUNNING, true));

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_A_APPLY)); /* the map WAS asked */
    TEST_ASSERT_EQUAL_INT(APP_MODE_TIMERS, (int)flow_mode);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_SET_MODE), "a refused Button A still wrote the mode");
}

/* --- HAZARD: the break-end / toggle collision --- */

/* THE CASE THIS TASK MOST NEEDS. On the button path the action runs
   first, and render_action_result() drains the break end BEFORE the
   paint — where M2-T2's revert stores APP_MODE_TIMERS UNCONDITIONALLY
   (wake_flow.c, and deliberately so: see the comment there). So the
   obvious A arm produces:

       press A  -> toggle writes APP_MODE_CHORES
       drain    -> break end writes APP_MODE_TIMERS
       paint    -> Timers

   The press is silently undone and the panel shows exactly what it showed
   before — the "mode button does nothing" failure, arriving from the
   opposite direction to the one M2-T2's emptied-list guard was written to
   prevent.

   Draining first instead only ROTATES that failure onto the other
   direction (the toggle would flip from the reverted value, so a press
   made in chore mode lands back in chores), which is why the arm applies,
   drains, and then re-asserts what the press chose. This case and the one
   below it are the two directions, and BOTH have to pass — either pure
   ordering passes one and fails the other.

   Driven through the break tail poll rather than through flow_dispatch,
   because the collision is only reachable where render_action_result()
   runs — the dispatch alone would pass with either order. */
void test_c3_a_break_end_in_the_same_wake_does_not_undo_the_toggle(void) {
    mock_time_set(flow_at(15, 0));
    flow_mode = APP_MODE_TIMERS;
    flow_chore_count = 3;
    flow_state = TIMER_PAUSED;
    flow_arm_break(flow_at(14, 59), FLOW_PIANO, FLOW_SCREEN); /* an end nothing has drained */
    flow_press(BTN_A);

    TEST_ASSERT_TRUE(wake_flow_poll_break_buttons());

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_CHIME), "the premise: the break end really was drained here");
    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_CHORES, (int)flow_mode,
                                  "the break-end revert landed on top of the toggle and undid the press");
    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_CHORES, (int)flow_painted_mode,
                                  "the panel painted Timers over a press of the mode button");
}

/* THE OTHER DIRECTION, and the case that rejects "just drain first". The
   panel is showing the chore screen, a break ends in the same wake, and
   the kid presses A to go back to Timers. Drain-first would revert to
   Timers and then toggle INTO chores — the press appears dead again, from
   the third side. Apply-drain-reassert lands on Timers, which is both what
   the press asked for and what the break end wanted. */
void test_c3_a_break_end_still_reverts_when_the_press_leaves_chore_mode(void) {
    mock_time_set(flow_at(15, 0));
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_state = TIMER_PAUSED;
    flow_arm_break(flow_at(14, 59), FLOW_PIANO, FLOW_SCREEN);
    flow_press(BTN_A);

    TEST_ASSERT_TRUE(wake_flow_poll_break_buttons());

    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_TIMERS, (int)flow_mode,
                                  "a break end plus a press of A left the device in chore mode");
}

/* THE STRANDING GUARD. The arm returns BEFORE it drains when the toggle
   is refused, so a refused A leaves the break-end edge exactly where a
   refused B leaves it — latched, for a later consumer or the sleep safety
   net. Draining and then reporting false would eat the edge and paint
   nothing: `take` is a consuming read, so the chime, the snap back and
   the wake-sticky full refresh would be gone together. That is the
   stranded-latch failure this codebase has shipped once already, and it
   is why the refusal is tested first rather than after the drain. */
void test_c3_a_refused_button_a_leaves_the_break_end_latched(void) {
    mock_time_set(flow_at(15, 0));
    flow_a_allowed = false; /* a timer is running behind the break */
    flow_mode = APP_MODE_TIMERS;
    flow_state = TIMER_PAUSED;
    flow_arm_break(flow_at(14, 59), FLOW_PIANO, FLOW_SCREEN);
    flow_press(BTN_A);

    TEST_ASSERT_FALSE(wake_flow_poll_break_buttons());

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_CHIME), "a refused Button A drained the break end and ate it");
    TEST_ASSERT_FALSE(wake_flow_break_ended_this_wake());
    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_TIMERS, (int)flow_mode, "a refused press moved the mode after all");
    /* THE assertion, and it has to be the owner rather than the sleep
       safety net: the latch is set by timer_break_tick(), which the owner
       runs and the raw take at sleep does not, so a net that saw nothing
       would prove only that nothing had ticked yet. Asking the owner
       afterwards is the direct question — is the edge still there? — and
       it comes back with the chime and the snap back intact. */
    TEST_ASSERT_TRUE_MESSAGE(wake_flow_break_end(),
                             "the refused press consumed the break end a later consumer was owed");
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_CHIME));
}

/* --- the two pick masks --- */

/* The break tail's ALLOWED mask. 2.6 wants chore mode reachable
   THROUGHOUT a break, and this poll is the only consumer of a press
   latched during one — so A being outside the mask here is the difference
   between "the checklist is where you do chores while the screen is
   locked" and a dead button for the whole break. */
void test_c3_the_break_tail_poll_acts_on_a_lone_button_a_press(void) {
    flow_state = TIMER_BREAK;
    flow_mode = APP_MODE_TIMERS;
    flow_chore_count = 3;
    flow_press(BTN_A);
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(wake_flow_poll_break_buttons());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_A_APPLY));
    TEST_ASSERT_EQUAL_INT(APP_MODE_CHORES, (int)flow_mode);
    TEST_ASSERT_EQUAL_INT(1, FLOW_RENDERS());
    TEST_ASSERT_EQUAL_HEX8(0, flow_latch_residue());
}

/* PRIORITY, which admitting A to the mask does not change: button_latch_pick
   runs B > C > D > A, so a B press latched alongside an A press still
   wins. This is the property that made adding A to the mask safe at all —
   the old comment at both sites said A was excluded so it could not
   swallow a real press, and that reason died when the priority table
   moved A to last. Pinned here so the mask and the table cannot drift. */
void test_c3_a_button_b_press_still_beats_a_button_a_press(void) {
    flow_state = TIMER_PAUSED;
    flow_b_result = BTN_B_STARTED;
    flow_press(BTN_A);
    flow_press(BTN_B);
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(wake_flow_poll_break_buttons());

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_B_APPLY), "Button A swallowed a Button B press");
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_A_APPLY));
    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_TIMERS, (int)flow_mode, "the swallowed press toggled the mode anyway");
}

/* The tick-wake latch drain: a press made while the wake was already
   awake (a sync, a grid wait, an e-ink flush). Without A in this mask the
   press evaporates at deep sleep — which is the same nothing as before
   the binding existed, on the path a kid is most likely to use it from
   (the panel is visibly busy, so they press again). */
void test_c3_the_tick_wake_latch_drain_acts_on_a_latched_button_a_press(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_mode = APP_MODE_TIMERS;
    flow_chore_count = 3;
    flow_press(BTN_A);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_A_APPLY),
                                  "a Button A press latched during the wake never reached the toggle");
    TEST_ASSERT_EQUAL_INT(APP_MODE_CHORES, (int)flow_mode);
    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_CHORES, (int)flow_painted_mode,
                                  "the toggle landed but the wake painted the old screen");
}

/* --- the EXT1 decode --- */

/* A wake CAUSED by A. The decode has to name it or the press only ever
   repaints the screen it was pressed to leave. */
void test_c3_a_button_a_ext1_wake_toggles_the_mode(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_mode = APP_MODE_TIMERS;
    flow_chore_count = 3;
    flow_wakeup_btn = BTN_A;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_A_APPLY), "the EXT1 decode has no Button A case");
    TEST_ASSERT_EQUAL_INT(APP_MODE_CHORES, (int)flow_mode);
    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_CHORES, (int)flow_painted_mode,
                                  "a Button A wake repainted the timer screen it was pressed to leave");
}

/* THE RESIDUAL, pinned so it is recorded behaviour rather than an
   accident waiting to be "fixed" into something worse. A caller that
   drains the break end in its prologue, ABOVE the dispatch, leaves the
   arm's re-assert nothing to rescue: the mode is already Timers by the
   time button_a_apply() reads it, so the press toggles INTO chores rather
   than out of them.

   IT IS NOT ONE PATH. An earlier revision of this comment said "the two
   latch-drain paths do not have that prologue and get the press they were
   given", and that is false for one of them:
   wake_flow_handle_timer_tick calls wake_flow_break_end() twice
   UNCONDITIONALLY (once after rollover + bedtime, once after the grid
   wait) and only then drains its latch, so it has exactly the prologue
   this one does. The next test is its twin and exists because the diff
   that added this one asserted the tick path was clean. Only
   wake_flow_poll_break_buttons has no prologue, which is why the break
   tail is the one caller the arm's ordering fully protects.

   Left alone deliberately, on both paths. Moving those drains below the
   dispatch would change what `before` is for Button B and Button C as
   well — their position is what lets a snap back ride the wake-sticky
   promotion instead of confusing the state diff — and the cost is one
   extra press in a wake where the break ended and the kid pressed A
   before the scheduled break-end wake could repaint. */
void test_c3_an_ext1_wake_that_also_drains_a_break_end_toggles_from_timers(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_wakeup_btn = BTN_A;
    flow_arm_break(flow_at(14, 59), FLOW_PIANO, FLOW_SCREEN);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_CHIME), "the premise: the prologue really drained the edge");
    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_CHORES, (int)flow_mode,
                                  "the EXT1 residual changed: the press now toggles from the pre-revert mode");
}

/* THE SECOND OCCURRENCE of the residual above, on the tick-wake latch
   drain. Same shape, same cost, same reason for being left alone — the
   only difference is that the prologue here is two unconditional
   wake_flow_break_end() calls rather than one.

   This test exists because the change that introduced the A arm asserted
   the opposite in two places at once (the arm's own comment and the EXT1
   call site both said the latch-drain callers had no prologue), which
   would have sent the next reader looking for a bug that is not there —
   or, worse, "fixing" the tick drain's ordering, which moves `before` for
   Button B and Button C too. Pinning it is the cheap half of that. */
void test_c3_a_tick_latch_drain_after_a_break_end_toggles_from_timers(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_arm_break(flow_at(14, 59), FLOW_PIANO, FLOW_SCREEN);
    flow_press(BTN_A);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_CHIME),
                                  "the premise: the tick prologue really drained the edge before the latch drain");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_A_APPLY), "the latched Button A press never reached the toggle");
    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_CHORES, (int)flow_mode,
                                  "the tick-drain residual changed: the press now toggles from the pre-revert mode");
}

/* A refused A wake still paints — the handler's tail runs regardless of
   what the dispatch reported, exactly as it does for a refused B. The
   press cost a wake here because the device was already awake enough to
   decode it; what stops that costing a wake in the ordinary case is the
   wake mask, one layer down (test_buttons_policy). */
void test_c3_a_refused_button_a_wake_repaints_the_current_screen(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_a_allowed = false;
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_wakeup_btn = BTN_A;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_CHORES, (int)flow_mode, "a refused Button A wake moved the mode");
    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_CHORES, (int)flow_painted_mode,
                                  "a refused Button A wake painted the other screen");
}

/* --- the emptied-list guard, from the other side (hazard 2) --- */

/* M2-T2's guard runs at PAINT time and STORES: if the list reads back
   empty it writes APP_MODE_TIMERS. So it can, in principle, clobber a
   toggle made earlier in the same wake — the second way round to the same
   "does nothing" failure.

   It cannot in practice, and this is where that is written down: the
   predicate behind button_a_toggle_allowed() refuses a toggle INTO chore
   mode when no chores are configured, so the guard never meets a toggle
   it would have to undo. What remains is the genuinely racy case — a list
   emptied by an MQTT config edit that lands AFTER the press, in the same
   wake — where the guard is correct to win: there is nothing to paint a
   checklist from. That is this case, and the guard winning is the
   assertion, not the bug. */
void test_c3_a_config_edit_that_empties_the_list_after_the_press_still_wins(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_mode = APP_MODE_TIMERS;
    flow_chore_count = 3; /* a list, at the moment of the press */
    flow_press(BTN_A);

    /* The edit lands between the action and the paint, exactly as
       config_apply's apply_chores does on the network task. */
    flow_chore_count = 0;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_A_APPLY)); /* the press landed */
    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_TIMERS, (int)flow_painted_mode,
                                  "a chore screen with no chores on it reached the panel");
    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_TIMERS, (int)flow_mode,
                                  "the emptied-list guard rendered the fallback but left the stored mode in chores");
}

/* --- M2-T4: a mode toggle must not be painted as a PARTIAL --- */

/* THE hazard M2-T4 inherited. The A arm returns true with
   *selection_changed false and moves no timer, so before == after with no
   break end — and wake_policy_render's button leg answers PARTIAL. That
   was harmless while both modes painted the same screen. It stopped being
   harmless the moment display_screen_for() gave chore mode a layout of its
   own: a partial diff across a whole-screen layout change ghosts the
   panel, which is the very hazard wake_policy_render's own break-chip
   comment names for the other full-screen layout.

   The negative control is inline and is the point of the test: the shipped
   policy, asked about this exact state pair, still says PARTIAL. So the
   full refresh below can only be coming from the force_full promotion. */
void test_a_mode_toggle_is_painted_full_not_partial(void) {
    flow_mode = APP_MODE_TIMERS;
    flow_chore_count = 3;
    flow_state = TIMER_IDLE;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_EQUAL_INT_MESSAGE(
        WAKE_RENDER_PARTIAL, wake_policy_render(TIMER_IDLE, TIMER_IDLE, true, false, false),
        "the policy no longer calls this pair partial - this test's negative control has gone vacuous");

    TEST_ASSERT_TRUE(flow_dispatch(BTN_A, flow_at(16, 0), TIMER_IDLE, true));
    render_action_result(BTN_A, TIMER_IDLE, flow_at(16, 0), false);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_FULL_REFRESH), "a mode toggle was painted as a partial diff");
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PARTIAL));
    /* and it really was the chore screen that got painted */
    TEST_ASSERT_EQUAL_INT(APP_MODE_CHORES, (int)flow_painted_mode);
}

/* The other direction across the same boundary — chores back to timers —
   because both crossings ghost, exactly as both crossings of the break
   screen do (test_row4_a_swap_back_onto_the_break_screen_renders_full). */
void test_a_toggle_back_out_of_chore_mode_is_also_full(void) {
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_state = TIMER_IDLE;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_A, flow_at(16, 0), TIMER_IDLE, true));
    render_action_result(BTN_A, TIMER_IDLE, flow_at(16, 0), false);

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_FULL_REFRESH));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PARTIAL));
    TEST_ASSERT_EQUAL_INT(APP_MODE_TIMERS, (int)flow_painted_mode);
}

/* A REFUSED press must NOT be promoted. It leaves the panel showing the
   screen it already showed, and the tail paints it anyway
   (test_c3_a_refused_button_a_wake_repaints_the_current_screen), so a full
   refresh there would spend two seconds of panel time on an unchanged
   frame. This is what stops the promotion being written as
   `btn == BTN_A`. */
void test_a_refused_button_a_is_not_promoted_to_a_full_refresh(void) {
    flow_a_allowed = false; /* a RUNNING timer, or no chore list */
    flow_mode = APP_MODE_TIMERS;
    flow_state = TIMER_IDLE;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_FALSE(flow_dispatch(BTN_A, flow_at(16, 0), TIMER_IDLE, true));
    render_action_result(BTN_A, TIMER_IDLE, flow_at(16, 0), false);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_FULL_REFRESH),
                                  "a refused mode toggle burned a full panel refresh");
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_PARTIAL));
}

/* The promotion is wake-sticky, so it also covers the RE-render after the
   network join: a config edit that empties the list mid-wake sends
   make_display_state's guard back to the timer screen, which is a second
   screen-kind change in the same wake and would ghost the same way. */
void test_the_toggle_promotion_survives_into_the_post_join_repaint(void) {
    flow_mode = APP_MODE_TIMERS;
    flow_chore_count = 3;
    flow_state = TIMER_IDLE;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_A, flow_at(16, 0), TIMER_IDLE, true));
    flow_net_finish = NET_FINISH_CHANGED; /* the join changed what the panel shows */
    flow_chore_count = 0;                 /* ...by emptying the list */

    finish_action_and_render(BTN_A, TIMER_IDLE, flow_at(16, 0), false);

    TEST_ASSERT_EQUAL_INT_MESSAGE(2, flow_log_count(EV_FULL_REFRESH),
                                  "the post-join repaint across the same boundary went partial");
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PARTIAL));
    TEST_ASSERT_EQUAL_INT(APP_MODE_TIMERS, (int)flow_painted_mode);
}

/* --- the general rule: ANY change of screen kind is a full refresh --- */

/* The two toggle cases above are one instance of a larger rule, and
   leaving only them fixed leaves the rule resting on an emergent
   invariant: today no press can reach a chore-mode RUNNING (B's start is
   rebound, C no longer swaps, the join poll is guarded, HA cannot start a
   timer, and a break end restores slot 0 PAUSED), so the timer state
   cannot move the screen kind on its own. Every one of those is a fact
   about a DIFFERENT file, and nothing stops a later task changing one.

   So the promotion asks display_screen_for() — the painter's own choice —
   instead of enumerating the presses that can reach it, which is what
   design 2.5's "the transition out of chore mode stays a full refresh"
   actually means. These cases drive render_action_result() at its own
   seam with a state pair whose screen kind moves, deliberately WITHOUT
   claiming a press that produces it: the seam is where the rule lives,
   and a test routed through the bindings would be re-asserting their
   current shape instead of the rule.

   Negative control inline, as the toggle cases have: the shipped policy
   still calls this pair PARTIAL, so the full refresh can only be the
   promotion. */
void test_a_screen_kind_change_from_the_timer_state_alone_is_painted_full(void) {
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_state = TIMER_PAUSED; /* after: the checklist */
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_EQUAL_INT_MESSAGE(DISPLAY_SCREEN_MAIN, (int)display_screen_for(TIMER_RUNNING, APP_MODE_CHORES, 3),
                                  "the fixture's `before` is not the timer screen");
    TEST_ASSERT_EQUAL_INT_MESSAGE(DISPLAY_SCREEN_CHORES, (int)display_screen_for(TIMER_PAUSED, APP_MODE_CHORES, 3),
                                  "the fixture's `after` is not the chore screen");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        WAKE_RENDER_PARTIAL, wake_policy_render(TIMER_RUNNING, TIMER_PAUSED, true, false, false),
        "the policy no longer calls this pair partial - this test's negative control has gone vacuous");

    render_action_result(BTN_B, TIMER_RUNNING, flow_at(16, 0), false);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_FULL_REFRESH),
                                  "a whole-screen layout change was painted as a partial diff");
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PARTIAL));
}

/* The other direction across the same boundary, because both crossings
   ghost — the same reason the two mode-toggle directions are both pinned. */
void test_the_other_direction_of_a_screen_kind_change_is_full_too(void) {
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_state = TIMER_RUNNING; /* after: back to the timer screen */
    mock_time_set(flow_at(16, 0));

    render_action_result(BTN_B, TIMER_PAUSED, flow_at(16, 0), false);

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_FULL_REFRESH));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PARTIAL));
}

/* And the half that stops the promotion being "any state change at all".
   A pause on the timer screen moves the state and keeps the layout, so it
   must still go partial — otherwise every ordinary press costs a full
   panel refresh and design 2.5's whole budget is gone. */
void test_a_state_change_that_keeps_the_same_screen_stays_partial(void) {
    flow_mode = APP_MODE_TIMERS;
    flow_chore_count = 3;
    flow_state = TIMER_PAUSED;
    mock_time_set(flow_at(16, 0));

    render_action_result(BTN_B, TIMER_RUNNING, flow_at(16, 0), false);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_FULL_REFRESH), "an ordinary pause burned a full panel refresh");
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_PARTIAL));
}

/* --- C16: a break that STARTS in chore mode has to announce itself --- */

/* display_screen_for() puts CHORES above BREAK unconditionally and
   paint_break_started() renders through that same choice, so a break
   starting while the stored mode is CHORES painted the CHECKLIST: no
   SCREEN BREAK title, no countdown, no bar. The alarm sounds and the
   panel never says why.
   The revert is the break-END revert's argument applied to the other edge
   of the same event — the kid did not press anything, and the repaint is
   a full refresh with an alarm behind it whose whole job is to show the
   new situation. Nothing is lost: 2.6's break screen carries its own
   "A → Chores" prompt, so the way back in is the press 2.6 asks for. */
void test_c16_a_break_that_starts_in_chore_mode_reverts_to_timers(void) {
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_break_due_ret = true;

    TEST_ASSERT_EQUAL_INT(FLOW_GATE_STARTED, flow_run_break_gate(flow_at(14, 0)));

    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_TIMERS, (int)flow_mode,
                                  "a break started and left the device painting the chore screen");
    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_TIMERS, (int)flow_painted_mode,
                                  "the break screen paint still went out in chore mode");
    TEST_ASSERT_EQUAL_INT_MESSAGE(DISPLAY_SCREEN_BREAK,
                                  (int)display_screen_for(TIMER_BREAK, flow_painted_mode, flow_chore_count),
                                  "the painter would still have chosen the checklist over the break screen");
}

/* The revert has to land AFTER the chore-ack arm's re-assert, not merely
   after a mode toggle: M2-T4a made the ack arm store APP_MODE_CHORES
   again once the break-end drain has run, which is what widened this. The
   dispatch has returned by the time finish_or_break() reaches the break
   gate, so the ordering holds — asserted rather than reasoned about. */
void test_c16_the_break_start_revert_survives_an_ack_earlier_in_the_wake(void) {
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_ack_result = BTN_ACK_TOGGLED;
    flow_state = TIMER_IDLE;
    mock_time_set(flow_at(14, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_B, flow_at(14, 0), TIMER_IDLE, true));
    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_CHORES, (int)flow_mode, "the ack arm did not re-assert chore mode");

    flow_break_due_ret = true;
    TEST_ASSERT_EQUAL_INT(FLOW_GATE_STARTED, flow_run_break_gate(flow_at(14, 0)));

    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_TIMERS, (int)flow_mode,
                                  "the ack's re-assert outlived the break start and hid the break screen");
}

/* ---- the OTA call sites -------------------------------------------------

   Four things, and only the first two of the four are decisions this
   module makes: WHICH triggers arm a check, WHAT facts they are armed
   with, WHERE in the pre-sleep tail the download sits, and that it is
   handed facts sampled at that point rather than at arming.

   The negative cases matter as much as the positive ones. "Only arm on a
   trigger that can check" is a deliberate choice over "always arm, with
   OTA_TRIGGER_NONE where appropriate", and the difference is invisible in
   the ordinary case: ota_flow_arm's buffer clear only fires when the arm
   actually arms, so an arm(NONE) is a no-op for everything except one
   state — a wake whose FIRST window never ran the check (no WiFi, or a
   window that failed to spawn), where the arm is still live and the
   second window of the same wake would pick it up. An arm(NONE) at that
   second window silently throws that away. Button B and the tick sync are
   therefore left alone, and these two cases are what stop a later tidy-up
   from "completing the set". */

void test_a_day_rollover_arms_an_update_check_before_it_opens_the_window(void) {
    time_t now = flow_at(0, 5);
    flow_new_day = true;

    wake_flow_handle_day_rollover(&now);

    TEST_ASSERT_EQUAL_INT(1, flow_ota_arms);
    TEST_ASSERT_EQUAL_INT(OTA_TRIGGER_ROLLOVER, (int)flow_ota_trigger);
    /* The order is the load-bearing half: the check runs on the network
       task and reads what the arm sampled, so an arm after the spawn is
       a race rather than a style question. */
    int armed = flow_log_at(EV_OTA_ARM);
    TEST_ASSERT_TRUE(armed >= 0);
    TEST_ASSERT_TRUE(armed < flow_log_at(EV_TRY_WINDOW));
}

/* A wake that is not a new day opens no window and arms nothing. */
void test_a_day_that_has_not_rolled_over_arms_nothing(void) {
    time_t now = flow_at(9, 0);
    flow_new_day = false;

    wake_flow_handle_day_rollover(&now);

    TEST_ASSERT_EQUAL_INT(0, flow_ota_arms);
}

void test_the_rollover_arm_carries_the_battery_and_the_charge_lock(void) {
    time_t now = flow_at(0, 5);
    flow_new_day = true;
    flow_batt_pct = 41;
    flow_charge_locked = true;

    wake_flow_handle_day_rollover(&now);

    TEST_ASSERT_EQUAL_INT(41, flow_ota_arm_batt);
    TEST_ASSERT_TRUE(flow_ota_arm_locked);
}

/* The failed ADC read, kept distinct from a flat cell. ota_gate_in_t says
   batt_pct < 0 is "unreadable" and does NOT gate; the SoC curve clamps a
   dead read to 0 %, which WOULD gate — permanently, and silently, on a
   device whose ADC broke. */
void test_an_unreadable_battery_arms_as_unknown_rather_than_as_flat(void) {
    time_t now = flow_at(0, 5);
    flow_new_day = true;
    flow_batt_unreadable = true;

    wake_flow_handle_day_rollover(&now);

    TEST_ASSERT_EQUAL_INT(-1, flow_ota_arm_batt);
}

void test_button_d_arms_a_sync_check_before_it_opens_its_window(void) {
    mock_time_set(flow_at(15, 0));
    flow_wakeup_btn = BTN_D;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT(1, flow_ota_arms);
    TEST_ASSERT_EQUAL_INT(OTA_TRIGGER_SYNC, (int)flow_ota_trigger);
    int armed = flow_log_at(EV_OTA_ARM);
    TEST_ASSERT_TRUE(armed >= 0);
    TEST_ASSERT_TRUE(armed < flow_log_at(EV_NET_OPEN));
}

/* Button B opens a window and must NOT arm: the user is waiting on the
   panel, and a manifest GET would sit between the press and the render.
   The window is asserted so the case cannot pass by the press being
   dropped. */
void test_button_b_opens_a_window_without_arming_a_check(void) {
    mock_time_set(flow_at(15, 0));
    flow_wakeup_btn = BTN_B;
    flow_b_result = BTN_B_STARTED;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_NET_OPEN));
    TEST_ASSERT_EQUAL_INT(0, flow_ota_arms);
}

/* The periodic clock sync is the third window this firmware opens, and it
   does not arm either — it is a tick wake, which the trigger enum names
   as never checking. */
void test_the_periodic_sync_window_arms_no_check(void) {
    mock_time_set(flow_at(15, 0));
    flow_state = TIMER_IDLE;
    flow_last_ntp = flow_at(15, 0) - IDLE_SYNC_INTERVAL_SEC; /* sync is due */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_TRY_WINDOW));
    TEST_ASSERT_EQUAL_INT(0, flow_ota_arms);
}

/* The `repaint` seam app_main hands to ota_flow_ops_t. A failed download
   leaves the update screen on the panel, and this is the only thing that
   takes it off — so an entry point that compiled but did nothing would be
   invisible until a real download failed in the field. The composition
   root's half (that the ops table actually points HERE) has no host home;
   this pins the other half, that what it points at is the repaint. */
void test_the_ota_repaint_seam_paints_the_normal_screen(void) {
    mock_time_set(flow_at(15, 0));

    wake_flow_repaint_current_state();

    TEST_ASSERT_EQUAL_INT(1, flow_repaint_count());
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PARTIAL));
}

/* ---- the apply point ---------------------------------------------------- */

/* The whole reason the apply is at the tail of the wake handlers rather
   than inside enter_deep_sleep(): it has to be after the join (ota_flow.h
   makes the pending flag readable only there) and before the sleep, and
   it has to be somewhere the awake failsafe's esp_timer context cannot
   reach. The first two halves are assertable here; the third is
   structural and is asserted by the absence of a call in main.c. */
void test_a_pending_update_is_applied_after_the_join_and_before_the_sleep(void) {
    mock_time_set(flow_at(0, 5));
    flow_new_day = true;
    flow_ota_pending = true;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(1, flow_ota_applies);
    int applied = flow_log_at(EV_OTA_APPLY);
    TEST_ASSERT_TRUE(applied >= 0);
    TEST_ASSERT_TRUE(flow_log_at(EV_TRY_WINDOW) < applied);
    TEST_ASSERT_TRUE(applied < flow_log_at(EV_SLEEP));
}

/* The common path. Nothing pending means no second window, no ADC read
   for it and no 16 KB task allocation — which is why the test lives at
   this call site and not inside ota_flow_apply's own early-out. */
void test_a_wake_with_nothing_pending_opens_no_second_window(void) {
    mock_time_set(flow_at(0, 5));
    flow_new_day = true;
    flow_ota_pending = false;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(0, flow_ota_applies);
}

/* A spawn that fails is the one outcome ota_flow.c can never report,
   because ota_flow_apply does not run at all: xTaskCreate refused the
   16 KB stack (the largest single allocation this firmware makes) or the
   semaphore could not be allocated. s_pending had been true, so an update
   WAS found and announced by the check — and before this the device then
   said nothing about it, on this wake and on every wake after it. */
void test_a_download_task_that_cannot_be_spawned_is_reported(void) {
    mock_time_set(flow_at(0, 5));
    flow_new_day = true;
    flow_ota_pending = true;
    flow_ota_spawn_ok = false;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(1, flow_ota_applies);
    TEST_ASSERT_EQUAL_INT(1, flow_ota_spawn_failures_noted);
}

/* And the ordinary path stays silent: a spawn that worked hands the
   reporting to ota_flow_apply, which knows what actually happened. Two
   notes for one wake would overwrite a real download failure with a
   heap excuse. */
void test_a_download_task_that_spawns_reports_nothing_from_here(void) {
    mock_time_set(flow_at(0, 5));
    flow_new_day = true;
    flow_ota_pending = true;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(1, flow_ota_applies);
    TEST_ASSERT_EQUAL_INT(0, flow_ota_spawn_failures_noted);
}

/* The button handler's tail is the second call site, and it is a separate
   statement: deleting either one leaves the other's cases green. */
void test_the_button_handler_applies_a_pending_update_too(void) {
    mock_time_set(flow_at(15, 0));
    flow_wakeup_btn = BTN_D;
    flow_ota_pending = true;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT(1, flow_ota_applies);
    int applied = flow_log_at(EV_OTA_APPLY);
    TEST_ASSERT_TRUE(applied >= 0);
    TEST_ASSERT_TRUE(flow_log_at(EV_NET_FINISH) < applied);
    TEST_ASSERT_TRUE(applied < flow_log_at(EV_SLEEP));
}

/* The facts are RE-SAMPLED at the apply, not carried over from the arm.
   The two windows are minutes and a full-panel repaint apart, and a cell
   that has crossed into the charge lock in between is exactly the one
   that must not be asked for a sustained radio burst followed by a flash
   write. The stub moves the cell at the arm to make the difference
   visible; carrying the arm's value over would report 41 twice. */
void test_the_apply_samples_the_battery_again_instead_of_reusing_the_arms(void) {
    mock_time_set(flow_at(0, 5));
    flow_new_day = true;
    flow_ota_pending = true;
    flow_batt_pct = 41;
    flow_ota_batt_after_arm = 88;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(41, flow_ota_arm_batt);
    TEST_ASSERT_EQUAL_INT(88, flow_ota_apply_batt);
}

/* The apply is the LAST thing the wake does. Not a stylistic preference:
   the pre-sleep event watch can hold the CPU for the whole final minute
   and repaints when it is done, so an apply hoisted above it would paint
   the update screen and then have the watch paint over it — and, worse,
   a reboot mid-watch would swallow the expiry alert the watch exists to
   fire. A RUNNING wake with an expiry inside the watch window is what
   makes the watch produce an event at all; with an empty watch the two
   orderings are indistinguishable. */
void test_the_apply_runs_after_the_pre_sleep_event_watch(void) {
    time_t now = flow_at(15, 0);
    mock_time_set(now);
    flow_state = TIMER_RUNNING;
    flow_expiry_wall = (int64_t)now + 30; /* inside the final-minute watch */
    flow_last_ntp = now;                  /* no sync window to muddy the trace */
    flow_needs_sync = false;
    flow_ota_pending = true;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    int alerted = flow_log_at(EV_ALERT_EXPIRY);
    int applied = flow_log_at(EV_OTA_APPLY);
    int slept = flow_log_at(EV_SLEEP);
    TEST_ASSERT_TRUE(alerted >= 0); /* the watch really did run */
    TEST_ASSERT_TRUE(alerted < applied);
    /* Nothing at all between the download and the sleep. */
    TEST_ASSERT_EQUAL_INT(slept - 1, applied);
}

/* The charge lock rides the same path, and it is the fact with teeth: it
   is the one the download gate refuses on. */
void test_the_apply_carries_the_charge_lock_it_sampled(void) {
    mock_time_set(flow_at(0, 5));
    flow_new_day = true;
    flow_ota_pending = true;
    flow_charge_locked = false;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_FALSE(flow_ota_apply_locked);
}

/* ======================================================================
   M2-T4a — B, C and D BOUND TO THE CHORE ACKS (design 2.4, 2.5)
   ======================================================================

   The routing half of the ack. What a toggle does to the mask, to flash
   and to the Screen timer is test_button_actions' subject; what lives
   here is which button reaches the ack with which row, what happens to
   the timer action it displaced, and what the wake does around it. */

/* Chore mode rebinds B to the first checkbox. The timer job it displaced
   must not ALSO run: a press that both ticked a chore and started the
   screen timer would be the worst of both, and design 4.2 only lets the
   device into chore mode because nothing is running. */
void test_c4a_b_in_chore_mode_acks_row_1_instead_of_starting(void) {
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_ack_result = BTN_ACK_TOGGLED;
    flow_b_result = BTN_B_STARTED; /* armed, so a fall-through would be loud */
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_B, flow_at(16, 0), TIMER_IDLE, true));

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_CHORE_ACK), "B did not reach the ack in chore mode");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_ack_idx, "B ticked the wrong row");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_B_APPLY), "the ack fell through to the timer action");
    TEST_ASSERT_EQUAL_INT(TIMER_IDLE, flow_state);
    TEST_ASSERT_EQUAL_INT64(flow_at(16, 0), flow_ack_now);
}

/* C is the middle checkbox, and the swap it displaced must not happen —
   a timer selection changing invisibly behind the checklist is exactly
   the ambiguity the three fixed rows exist to remove (design 2.4). */
void test_c4a_c_in_chore_mode_acks_row_2_instead_of_swapping(void) {
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_ack_result = BTN_ACK_TOGGLED;
    flow_select_ok = true; /* armed */
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_C, flow_at(16, 0), TIMER_IDLE, true));

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_CHORE_ACK));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_ack_idx, "C ticked the wrong row");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_SELECT_NEXT), "the ack fell through to the swap");
    TEST_ASSERT_FALSE_MESSAGE(flow_swapped_io, "an ack reported a selection change");
}

/* A REFUSED ack (a row that is not configured — two chores means no row
   3) must not fall back to the timer action either. In chore mode B, C
   and D ARE the ack buttons; a C press that silently swapped the selected
   timer because row 2 happened not to exist would be the same ambiguity
   arriving by a side door. */
void test_c4a_a_refused_ack_does_not_fall_through_to_the_timer_action(void) {
    const button_id_t btns[] = {BTN_B, BTN_C};
    for (int i = 0; i < 2; i++) {
        setUp();
        flow_mode = APP_MODE_CHORES;
        flow_chore_count = 2;
        flow_ack_result = BTN_ACK_NONE; /* the row is not configured */
        flow_b_result = BTN_B_STARTED;
        flow_select_ok = true;
        mock_time_set(flow_at(16, 0));

        TEST_ASSERT_FALSE(flow_dispatch(btns[i], flow_at(16, 0), TIMER_IDLE, true));

        TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_CHORE_ACK));
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_B_APPLY), "a refused ack started a timer");
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_SELECT_NEXT), "a refused ack swapped the timer");
    }
}

/* The other side of the same switch, and the one that matters to every
   device in the field: outside chore mode nothing is rebound at all. */
void test_c4a_outside_chore_mode_b_and_c_keep_their_timer_jobs(void) {
    flow_mode = APP_MODE_TIMERS;
    flow_ack_result = BTN_ACK_TOGGLED; /* armed, so a wrong route would be loud */
    flow_b_result = BTN_B_STARTED;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_B, flow_at(16, 0), TIMER_IDLE, true));

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_CHORE_ACK), "a timer-mode press reached the ack");
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_B_APPLY));
    TEST_ASSERT_EQUAL_INT(TIMER_RUNNING, flow_state);
}

/* Design 2.6 wants the checklist reachable THROUGHOUT a screen break, so
   the ack has to sit ahead of B's break guard. That guard refuses a start
   during a break, which is right for a start and wrong for a tick. */
void test_c4a_an_ack_is_not_refused_during_a_screen_break(void) {
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_ack_result = BTN_ACK_TOGGLED;
    flow_state = TIMER_BREAK;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(flow_dispatch(BTN_B, flow_at(16, 0), TIMER_BREAK, true));

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_CHORE_ACK), "B's break guard swallowed a chore ack");
}

/* M2-T3's apply -> drain -> RE-ASSERT, inherited. render_action_result()
   drains a pending break end before it paints and that drain stores
   APP_MODE_TIMERS unconditionally, so an ack that left the drain to the
   tail would tick the box and then paint the timer screen — the press
   visibly undone. The arm therefore drains here and re-asserts the mode
   the press was made in, exactly as the Button A arm does, and the
   drain's other effects (the chime, the snap back, the wake-sticky full
   refresh) all stand.

   Driven through the break-tail poll because that is the ONE caller with
   no drain in its prologue, and so the only one where the ordering is
   observable at all. */
void test_c4a_an_ack_drains_a_pending_break_end_and_keeps_chore_mode(void) {
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_ack_result = BTN_ACK_TOGGLED;
    flow_state = TIMER_BREAK;
    flow_arm_break(flow_at(16, 0), FLOW_PIANO, FLOW_SCREEN);
    mock_time_set(flow_at(16, 0) + 3);
    flow_press(BTN_B);

    TEST_ASSERT_TRUE(wake_flow_poll_break_buttons());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_CHORE_ACK));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_CHIME), "the ack arm stranded the break-end latch");
    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_CHORES, (int)flow_mode, "the drain's unconditional revert undid the press");
    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_CHORES, (int)flow_painted_mode,
                                  "the panel painted the screen the press was made to leave");
}

/* And a REFUSED ack returns before the drain, so the edge stays latched
   for a later consumer or the pre-sleep safety net. A refusal must never
   consume an edge it then cannot get painted — the same rule the refused
   Button A arm follows, and the shape that stranded this latch once
   already. */
void test_c4a_a_refused_ack_leaves_the_break_end_latched(void) {
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 2;
    flow_ack_result = BTN_ACK_NONE;
    flow_state = TIMER_BREAK;
    flow_arm_break(flow_at(16, 0), FLOW_PIANO, FLOW_SCREEN);
    mock_time_set(flow_at(16, 0) + 3);
    flow_press(BTN_B);

    TEST_ASSERT_FALSE(wake_flow_poll_break_buttons());

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_CHIME), "a refused ack consumed the break end");
    /* The edge is not merely un-chimed, it is still THERE: the refusal
       returned above the drain, so nothing has ticked the elapsed break
       yet and the next consumer still finds it. Asserted by draining it
       here, which is the only way to tell "left for later" apart from
       "silently swallowed". */
    TEST_ASSERT_TRUE_MESSAGE(flow_break_running, "a refused ack ticked the break");
    TEST_ASSERT_TRUE_MESSAGE(wake_flow_break_end(), "the break-end edge was eaten by a press that did nothing");
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_CHIME));
}

/* D is the third checkbox, and in chore mode it is NOT the sync button:
   an update check and a network window are not what the row promises. */
void test_c4a_d_in_chore_mode_acks_row_3_instead_of_checking_for_an_update(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_ack_result = BTN_ACK_TOGGLED;
    flow_wakeup_btn = BTN_D;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_CHORE_ACK), "the D wake arm has no ack case");
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, flow_ack_idx, "D ticked the wrong row");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_OTA_ARM), "the ack also armed an update check");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_NET_OPEN), "the ack also opened a network window");
}

/* Outside chore mode D is the sync button exactly as it was. */
void test_c4a_d_outside_chore_mode_still_checks_for_an_update(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_mode = APP_MODE_TIMERS;
    flow_ack_result = BTN_ACK_TOGGLED; /* armed */
    flow_wakeup_btn = BTN_D;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_CHORE_ACK), "a timer-mode D press reached the ack");
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_OTA_ARM));
}

/* ---- M2-T4b: ✓3 survives the latch drains too ---------------------------

   A DELIBERATE FLIP of what T4a shipped. The case that stood here,
   test_c4a_a_latched_d_press_in_chore_mode_still_never_acks, asserted
   that a latched D was taken and dropped in chore mode as well as out of
   it, and it was an accurate reading of T4a's code. It was also the
   defect: ✓1 and ✓2 ARE honoured from both latch drains, so the checklist
   lost its third row in exactly the window design §2.6 cares about most —
   wake_flow_watch_break_end's ~250 ms tail poll, which is the break, and
   the tick-wake drain, which is any press that lands while the wake is
   already awake.

   WHAT DID NOT CHANGE, and is pinned below: the dispatch still has no
   BTN_D arm (test_button_d_and_button_none_are_inert_in_the_dispatch),
   so D's TIMER action — the update check and the network window — is
   still reachable only from the EXT1 decode. D joins the pick only while
   the mode byte says CHORES, where it is ✓3 and has no network leg at
   all, which is the entire reason it was kept out. Neither pick mask
   became load bearing on the way: a D that leaked into the candidates in
   Timers mode routes to button_chore_ack_apply(), whose own first line
   refuses on the mode (button_actions.c) — inert, exactly as the
   dispatch's default arm was. */

/* ✓3 from the break tail: the window §2.6 names, and the one caller with
   no break-end drain in its prologue. */
void test_c4b_a_latched_d_press_in_the_break_tail_acks_row_3(void) {
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_ack_result = BTN_ACK_TOGGLED;
    flow_state = TIMER_BREAK;
    mock_time_set(flow_at(16, 0));
    flow_press(BTN_D);

    /* ASCII in the assertion MESSAGES, unlike the comments around them:
       Unity prints them byte-escaped, so a "check 3" that reads as
       \xE2\x9C\x933 tells the next reader nothing. */
    TEST_ASSERT_TRUE_MESSAGE(wake_flow_poll_break_buttons(), "the break tail dropped a check-3 press");

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_CHORE_ACK),
                                  "check 3 is discarded where checks 1 and 2 are honoured");
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, flow_ack_idx, "the break tail ticked the wrong row");
    TEST_ASSERT_EQUAL_HEX8(0, flow_latch_residue());
}

/* ✓3 from the tick-wake drain: a press that landed during a sync, a grid
   wait or an e-ink flush. Same binding, the other drain. */
void test_c4b_a_latched_d_press_in_chore_mode_acks_row_3(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_ack_result = BTN_ACK_TOGGLED;
    flow_state = TIMER_IDLE;
    flow_press(BTN_D);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_CHORE_ACK), "the tick-wake drain dropped a check-3 press");
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, flow_ack_idx, "the tick drain ticked the wrong row");
    TEST_ASSERT_EQUAL_HEX8(0, flow_latch_residue());
}

/* THE NEGATIVE CONTROL, and the reason the widening is mode-gated rather
   than unconditional: outside chore mode a latched D is still taken and
   thrown away. D's timer action opens a network window, and a D that
   merely rode in on somebody else's wake must not buy one — that is why
   it was kept out of both masks in the first place, and that reason is
   untouched here. An implementation that added (1u << BTN_D)
   unconditionally would pass both cases above and fail this one. */
void test_c4b_a_latched_d_press_outside_chore_mode_is_still_dropped(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_mode = APP_MODE_TIMERS;
    flow_ack_result = BTN_ACK_TOGGLED; /* armed, so a wrong route would be loud */
    flow_state = TIMER_IDLE;
    flow_press(BTN_D);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_CHORE_ACK), "a timer-mode latched D reached the ack");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_OTA_ARM), "a latched D bought an update check");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_NET_OPEN), "a latched D bought a network window");
    TEST_ASSERT_EQUAL_HEX8(0, flow_latch_residue()); /* taken, then dropped */
}

/* PRIORITY IS UNCHANGED, which is what keeps this one action per drain.
   button_latch_pick runs B > C > D > A, so D joining the candidates can
   only ever displace A — never a press that moves a timer, and never a
   second ack in the same drain. Asserted on the ROW, because an
   implementation that acked D alongside the B it lost to would still log
   an ack and still return true. */
void test_c4b_a_latched_b_press_outranks_a_latched_d_in_the_break_tail(void) {
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_ack_result = BTN_ACK_TOGGLED;
    flow_state = TIMER_BREAK;
    mock_time_set(flow_at(16, 0));
    flow_press(BTN_B);
    flow_press(BTN_D);

    TEST_ASSERT_TRUE(wake_flow_poll_break_buttons());

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_CHORE_ACK), "one drain ticked two boxes");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_ack_idx, "check 3 outranked check 1 - the pick's priority was bypassed");
    TEST_ASSERT_EQUAL_HEX8(0, flow_latch_residue());
}

/* Design 2.5: "an ack is a partial refresh", and the transition OUT of
   chore mode stays full. D is the one ack button that would otherwise
   have forced a full one — it is the user-facing "refresh everything"
   button and rides the force_full channel — so a D ack has to suppress
   that. Three acks at ~3 s each is the ~9 s the design rejects. */
void test_c4a_an_ack_renders_a_partial_not_a_full(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_ack_result = BTN_ACK_TOGGLED;
    flow_wakeup_btn = BTN_D;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, FLOW_RENDER_FLUSHES(),
                                  "an ack spent a full refresh - design 2.5 makes it a partial");
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_PARTIAL));
}

/* The negative control for the case above: with no ack in the wake, D is
   still the refresh button and still forces a full one. A suppression
   written as an unconditional drop would pass the partial case and fail
   this one. */
void test_c4a_d_outside_chore_mode_still_forces_a_full_refresh(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_mode = APP_MODE_TIMERS;
    flow_wakeup_btn = BTN_D;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, FLOW_RENDER_FLUSHES(), "Button D stopped forcing a full refresh");
}

/* The suppression is wake-sticky, so it also covers the RE-render after
   the network join — the same layout, painted a second time, and
   promoting THAT one would spend the seconds design 2.5 is saving a few
   lines later. Mirror of test_the_toggle_promotion_survives_into_the_post_
   join_repaint, which asserts the opposite for the opposite press. */
void test_c4a_the_ack_suppression_survives_into_the_post_join_repaint(void) {
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_ack_result = BTN_ACK_TOGGLED;
    flow_state = TIMER_IDLE;
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(wake_flow_apply_chore_ack(BUTTON_CHORE_IDX_D, flow_at(16, 0)));
    flow_net_finish = NET_FINISH_CHANGED; /* the join changed what the panel shows */

    finish_action_and_render(BTN_D, TIMER_IDLE, flow_at(16, 0), false);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_FULL_REFRESH),
                                  "the post-join repaint after an ack spent a full refresh");
    TEST_ASSERT_EQUAL_INT(2, flow_log_count(EV_PARTIAL));
}

/* The awake join poll (main.c calls it from inside the MQTT join) must
   not apply B's TIMER action from the chore screen: that would start the
   screen timer with the checklist on the panel, the one outcome design
   4.2 rules out by only admitting chore mode while nothing is running.
   It does not ack either — it runs after the render and could not paint
   one — so the press is LEFT IN THE LATCH rather than eaten, which is
   what the residue assertion is for. */
void test_c4a_the_join_poll_neither_starts_nor_acks_in_chore_mode(void) {
    flow_mode = APP_MODE_CHORES;
    flow_b_result = BTN_B_STARTED; /* armed, so a fall-through would be loud */
    flow_ack_result = BTN_ACK_TOGGLED;
    flow_state = TIMER_IDLE;
    flow_press(BTN_B);
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_FALSE(wake_flow_poll_button_b_action());

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_B_APPLY), "the join poll started a timer from the chore screen");
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_CHORE_ACK));
    TEST_ASSERT_EQUAL_INT(TIMER_IDLE, flow_state);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE((uint8_t)(1u << BTN_B), flow_latch_residue(),
                                   "the press was eaten by a poll that did nothing with it");
}

/* The negative control: outside chore mode the join poll is exactly what
   it was. A guard written as an unconditional early return would pass the
   case above and fail this one. */
void test_c4a_the_join_poll_outside_chore_mode_still_applies_b(void) {
    flow_mode = APP_MODE_TIMERS;
    flow_b_result = BTN_B_STARTED;
    flow_state = TIMER_IDLE;
    flow_press(BTN_B);
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_TRUE(wake_flow_poll_button_b_action());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_B_APPLY));
    TEST_ASSERT_EQUAL_INT(TIMER_RUNNING, flow_state);
}

/* ---- M3-T4: a press made to a locked device belongs to the lock ---------

   The config-error lock arms Button D alone, and the documented exit is
   "fix it in HA, then press D": D reaches the gate's network window early.
   When the gate's re-check then finds the pair fixed it lets go and falls
   through into the normal wake — and until M3-T4 the press fell through
   with it, into the switch that runs D's own action. In chore mode that
   ticked ✓3 (and could grant the withheld Screen time); on the timer
   screen it bought a second window and an update check.

   The gate now SAYS it released (lock_gate_check_bedtime returns true —
   which paths make it say so is test_lock_gate's subject, including the
   early release before any window), and these cases pin what the wake
   does with that: no action, a drained latch, and a full repaint of the
   normal screen. The stub models the release as the real gate leaves it —
   its window already run, the lock flag already clear — and can latch a
   press "made during the window", while Config Error was on the glass. */

/* THE DEFECT, in the mode it did the damage in. The window press is C
   (✓2), because before the fix the D ack armed the chore gesture window
   and the coalescer would have ticked C as well. */
void test_m3t4_a_release_by_d_in_chore_mode_ticks_nothing(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_chore_outstanding = 3;
    flow_ack_result = BTN_ACK_TOGGLED; /* armed, so a fall-through would be loud */
    flow_wakeup_btn = BTN_D;
    flow_sleep_mode_answer = WAKE_SLEEP_CONFIG_ERR;
    flow_gate_releases = true;
    flow_gate_window_press = BTN_C;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_CHORE_ACK), "the D that released the lock also ticked a box");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0, flow_chore_acked, "a checkbox moved on the release wake");
    TEST_ASSERT_FALSE_MESSAGE(flow_chore_released, "the release wake granted the withheld Screen time");
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_OTA_ARM));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_NET_OPEN));
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0, flow_latch_residue(), "a press made under Config Error survived the release");
    /* And the wake still does what the gate's fall-through promised: the
       normal screen, repainted in full over the lock screen. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_CHORES, (int)flow_painted_mode, "the release repainted the wrong screen");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, FLOW_RENDER_FLUSHES(), "Config Error was cleared with a partial");
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PARTIAL));
    TEST_ASSERT_EQUAL_INT(WAKE_SLEEP_NORMAL, flow_slept_mode);
}

/* The same release on the timer screen: no wrong state before the fix,
   but a second full network window and an update check the press never
   asked for — the lock's own window has just run. */
void test_m3t4_a_release_by_d_on_the_timer_screen_opens_no_second_window(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_mode = APP_MODE_TIMERS;
    flow_state = TIMER_PAUSED; /* where the lock left a timer it found running */
    flow_wakeup_btn = BTN_D;
    flow_sleep_mode_answer = WAKE_SLEEP_CONFIG_ERR;
    flow_gate_releases = true;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_OTA_ARM), "the release wake armed an update check");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_NET_OPEN), "the release wake opened a second window");
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_WAIT_NTP));
    TEST_ASSERT_EQUAL_INT(TIMER_PAUSED, flow_state); /* the release does not resume anything */
    TEST_ASSERT_EQUAL_INT_MESSAGE(APP_MODE_TIMERS, (int)flow_painted_mode, "the release repainted the wrong screen");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, FLOW_RENDER_FLUSHES(), "Config Error was cleared with a partial");
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PARTIAL));
}

/* THE REPAINT IS OWED BY THE RELEASE, not by the button. A B press on
   the timer screen normally renders a partial when it moves nothing, so
   this is the case where only the lock can be what makes it full — D's
   own force_full is out of the picture. (B cannot wake a config-locked
   device; the bed-time and charge sleeps arm nothing. The wake handler
   does not rely on which button it was, and neither does this.) */
void test_m3t4_the_release_repaint_is_full_whatever_the_button(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_mode = APP_MODE_TIMERS;
    flow_b_result = BTN_B_STARTED; /* armed, so a fall-through would be loud */
    flow_wakeup_btn = BTN_B;
    flow_sleep_mode_answer = WAKE_SLEEP_CONFIG_ERR;
    flow_gate_releases = true;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_B_APPLY));
    TEST_ASSERT_EQUAL_INT(TIMER_IDLE, flow_state);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, FLOW_RENDER_FLUSHES(), "the release render was left to the render policy");
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PARTIAL));
}

/* The post-join re-render paints over the normal screen the first render
   left, not over the lock screen, so it goes back to the render policy.
   That re-render has its own force_full (finish_action_and_render) and
   never reads s_lock_screen_on_glass, so what this pins is that the tail
   no longer sees a D: a D left in `btn` would force it full a second
   time. The one-shot itself is pinned by the case below. */
void test_m3t4_the_release_promotes_only_the_first_render(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_mode = APP_MODE_TIMERS;
    flow_wakeup_btn = BTN_D;
    flow_sleep_mode_answer = WAKE_SLEEP_CONFIG_ERR;
    flow_gate_releases = true;
    flow_net_finish = NET_FINISH_CHANGED; /* the join changed what the panel shows */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_FULL_REFRESH));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_PARTIAL), "the release promoted the post-join repaint too");
}

/* And the same one-shot seen from the other renderer that shares it: a
   press taken by the break tail's poll after the release paints through
   render_action_result() too, over the normal screen — so an ack there
   keeps design 2.5's partial. The post-join re-render above has its own
   force_full and cannot tell a one-shot from a wake-sticky flag; this can. */
void test_m3t4_a_later_render_in_the_release_wake_is_not_promoted(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_ack_result = BTN_ACK_TOGGLED;
    flow_wakeup_btn = BTN_D;
    flow_sleep_mode_answer = WAKE_SLEEP_CONFIG_ERR;
    flow_gate_releases = true;
    /* A break in its tail, so the watch polls and takes a press after
       the release's own render. */
    flow_arm_break(flow_at(15, 0) + 3, FLOW_PIANO, FLOW_SCREEN);
    flow_deferred_press_btn = BTN_C; /* ✓2, made on the repainted checklist */
    flow_deferred_press_ms = 400;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_CHORE_ACK), "a press made after the repaint was not honoured");
    TEST_ASSERT_EQUAL_INT(1, flow_ack_idx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, FLOW_RENDER_FLUSHES(), "the release promoted a later render too");
    TEST_ASSERT_GREATER_OR_EQUAL_INT(1, flow_log_count(EV_PARTIAL));
}

/* THE LOCK HOLDS: the gate ends the wake, and the press does nothing but
   the lock's own path. Existing behaviour, pinned so the consume above can
   never be mistaken for the only thing standing between D and ✓3. */
void test_m3t4_a_d_press_that_does_not_release_the_lock_does_nothing_else(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_ack_result = BTN_ACK_TOGGLED;
    flow_wakeup_btn = BTN_D;
    flow_sleep_mode_answer = WAKE_SLEEP_CONFIG_ERR;
    flow_bedtime_locks = true; /* the gate paints its screen and sleeps */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_BEDTIME, flow_run_button());

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_CHORE_ACK));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_OTA_ARM));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_NET_OPEN));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_FULL_REFRESH));
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PARTIAL));
}

/* THE NEGATIVE CONTROLS: a gate that released nothing leaves D its action
   in both modes. A consume written as unconditional would pass every case
   above and fail these. */
void test_m3t4_with_no_release_d_still_ticks_row_3_in_chore_mode(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_ack_result = BTN_ACK_TOGGLED;
    flow_wakeup_btn = BTN_D;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_CHORE_ACK));
    TEST_ASSERT_EQUAL_INT(2, flow_ack_idx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, FLOW_RENDER_FLUSHES(), "an unlocked ack wake was promoted to full");
}

void test_m3t4_with_no_release_d_still_opens_its_window_on_the_timer_screen(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_mode = APP_MODE_TIMERS;
    flow_wakeup_btn = BTN_D;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_OTA_ARM));
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_NET_OPEN));
}

/* THE TICK WAKE: a release there has no wake press to consume, but the
   latch can hold one made during the lock's window, and the tick drain
   before sleep would tick ✓3 with it. Drained once the release's repaint
   has landed (the grid-wait cases below pin why not earlier). */
void test_m3t4_a_tick_release_drops_a_press_made_under_the_lock(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_ack_result = BTN_ACK_TOGGLED;
    flow_state = TIMER_IDLE;
    flow_sleep_mode_answer = WAKE_SLEEP_CONFIG_ERR;
    flow_gate_releases = true;
    flow_gate_window_press = BTN_D;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_CHORE_ACK), "a press made under Config Error ticked a box");
    TEST_ASSERT_EQUAL_HEX8(0, flow_latch_residue());
}

/* And its control: a press latched on a tick wake with no release is a
   normal press, exactly as test_c4b_a_latched_d_press_in_chore_mode_acks_-
   row_3 has it — pinned here against a drain written unconditionally. */
void test_m3t4_a_tick_wake_with_no_release_keeps_its_latched_press(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_ack_result = BTN_ACK_TOGGLED;
    flow_state = TIMER_IDLE;
    flow_press(BTN_D);

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_CHORE_ACK));
}

/* THE JOIN POLL UNDER A LOCK. Every lock runs its window with its flag
   set, and that window's join polls B: acting there would resume the
   timer the lock just paused, behind the lock screen. Taken and dropped,
   under each of the three lock sleeps. The no-clock lock (BUG-14) sleeps
   the config-error one, so WAKE_SLEEP_CONFIG_ERR covers it too. */
void test_m3t4_the_join_poll_drops_b_under_every_lock(void) {
    const wake_sleep_mode_t locks[] = {WAKE_SLEEP_CHARGE_LOCK, WAKE_SLEEP_BEDTIME, WAKE_SLEEP_CONFIG_ERR};
    for (unsigned i = 0; i < sizeof locks / sizeof locks[0]; i++) {
        setUp();
        flow_mode = APP_MODE_TIMERS;
        flow_b_result = BTN_B_RESUMED; /* armed: what B would do to a paused timer */
        flow_state = TIMER_PAUSED;
        flow_sleep_mode_answer = locks[i];
        flow_press(BTN_B);
        mock_time_set(flow_at(16, 0));

        TEST_ASSERT_FALSE(wake_flow_poll_button_b_action());

        TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_B_APPLY), "the join poll acted on B behind a lock screen");
        TEST_ASSERT_EQUAL_INT(TIMER_PAUSED, flow_state);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0, flow_latch_residue(), "a B press made under a lock was left for later");
    }
}

/* Chore mode under a lock takes it too — the lock test runs first, so the
   chore-mode "leave it latched" refusal never gets to keep it. */
void test_m3t4_the_join_poll_drops_b_under_a_lock_in_chore_mode_too(void) {
    flow_mode = APP_MODE_CHORES;
    flow_ack_result = BTN_ACK_TOGGLED;
    flow_sleep_mode_answer = WAKE_SLEEP_CONFIG_ERR;
    flow_press(BTN_B);
    mock_time_set(flow_at(16, 0));

    TEST_ASSERT_FALSE(wake_flow_poll_button_b_action());

    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_CHORE_ACK));
    TEST_ASSERT_EQUAL_HEX8(0, flow_latch_residue());
}

/* ---- M3-T4 fix pass: the lock owns presses until its screen leaves ------

   A tick wake that releases the lock does not repaint at once: the
   regular sync may run, then the grid wait (up to 25 s), and only then
   the render. Config Error is on the glass all that time, so a press made
   there is made to the lock — the person fixed the pair in HA and pressed
   D, exactly as the screen told them to. Until the fix pass the tick
   handler drained at the gate and treated everything after as a normal
   press, so that D reached the latch pick as ✓3. The clock sits ten
   seconds off the minute, so the grid wait is real and the deferred press
   lands inside it. */
void test_m3t4_a_d_pressed_in_the_grid_wait_after_a_tick_release_ticks_nothing(void) {
    flow_tick_clock(flow_at(15, 0) + 50); /* 10 s to the wall minute */
    flow_reset_reason = ESP_RST_DEEPSLEEP;
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_chore_outstanding = 3;
    flow_ack_result = BTN_ACK_TOGGLED; /* armed, so an ack would be loud */
    flow_state = TIMER_PAUSED;         /* where the lock left the timer */
    flow_sleep_mode_answer = WAKE_SLEEP_CONFIG_ERR;
    flow_gate_releases = true;
    flow_promote_out = WAKE_RENDER_FULL; /* the real gate's promotion on a release */
    flow_deferred_press_btn = BTN_D;
    flow_deferred_press_ms = 1; /* the first poll of the grid wait */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(1, flow_deferred_delivered); /* a real press, by wall time */
    TEST_ASSERT_TRUE_MESSAGE(mock_delay_total_ms() >= 1000, "the grid wait did not happen");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_CHORE_ACK), "a D pressed under Config Error ticked ✓3");
    TEST_ASSERT_EQUAL_HEX8(0, flow_chore_acked);
    TEST_ASSERT_EQUAL_HEX8(0, flow_latch_residue());
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, FLOW_RENDER_FLUSHES(), "Config Error was cleared with a partial");
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_PARTIAL));
    TEST_ASSERT_FALSE_MESSAGE(s_lock_screen_on_glass, "the repaint left the lock's claim standing");
}

/* B in the same wait is dropped too, and the timer the lock paused stays
   paused. What drops it here is the pause poll's own take — it acts on a
   RUNNING timer only, and no release leaves one running, since every lock
   pauses at engage and only B restarts one — and the post-render drain
   behind it. The flag's part is the join poll, pinned below. */
void test_m3t4_a_b_pressed_in_the_grid_wait_after_a_tick_release_is_dropped(void) {
    flow_tick_clock(flow_at(15, 0) + 50);
    flow_reset_reason = ESP_RST_DEEPSLEEP;
    flow_mode = APP_MODE_TIMERS;
    flow_state = TIMER_PAUSED;
    flow_b_result = BTN_B_RESUMED; /* armed: what B would do to it */
    flow_sleep_mode_answer = WAKE_SLEEP_CONFIG_ERR;
    flow_gate_releases = true;
    flow_promote_out = WAKE_RENDER_FULL;
    flow_deferred_press_btn = BTN_B;
    flow_deferred_press_ms = 1;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(1, flow_deferred_delivered);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_B_APPLY), "a B pressed under Config Error resumed the timer");
    TEST_ASSERT_EQUAL_INT(TIMER_PAUSED, flow_state);
    TEST_ASSERT_EQUAL_HEX8(0, flow_latch_residue());
    TEST_ASSERT_EQUAL_INT(1, FLOW_RENDER_FLUSHES());
}

/* THE REGULAR SYNC AFTER A RELEASE. The gate has let go, so the lock flag
   is clear and lock_gate_sleep_mode() answers NORMAL — but the sync window
   runs before the repaint, with Config Error still showing, and its join
   polls B. s_lock_screen_on_glass is what keeps the poll dropping it. */
void test_m3t4_the_sync_after_a_tick_release_drops_b_in_its_join(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_last_ntp = flow_at(15, 0) - IDLE_SYNC_INTERVAL_SEC; /* the regular sync is due */
    flow_mode = APP_MODE_TIMERS;
    flow_state = TIMER_PAUSED;
    flow_b_result = BTN_B_RESUMED;
    flow_sleep_mode_answer = WAKE_SLEEP_CONFIG_ERR;
    flow_gate_releases = true;
    flow_promote_out = WAKE_RENDER_FULL;
    flow_window_join_press = BTN_B;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_TRY_WINDOW)); /* the sync ran */
    TEST_ASSERT_FALSE(flow_window_join_polled);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_B_APPLY), "the post-release sync resumed the timer");
    TEST_ASSERT_EQUAL_INT(TIMER_PAUSED, flow_state);
    TEST_ASSERT_EQUAL_HEX8(0, flow_latch_residue());
}

/* Its control: the same sync on a wake that released nothing applies B,
   so the case above is not passing on a window that never polls. */
void test_m3t4_the_sync_on_an_unlocked_tick_wake_still_applies_b(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_last_ntp = flow_at(15, 0) - IDLE_SYNC_INTERVAL_SEC;
    flow_mode = APP_MODE_TIMERS;
    flow_state = TIMER_PAUSED;
    flow_b_result = BTN_B_RESUMED;
    flow_window_join_press = BTN_B;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_TRUE(flow_window_join_polled);
    TEST_ASSERT_EQUAL_INT(1, flow_log_count(EV_B_APPLY));
}

/* THE CLAIM ENDS AT THE REPAINT. A press made after it — here a ✓2 taken
   by the break tail's poll on the repainted checklist — is a normal
   press, and its render goes back to the render policy: a flag left
   standing past the tick render would promote it (the button path's
   one-shot reader, render_action_result_as, is the break tail's too).
   The break ends 30 s out: past the grid wait's cap, so the render is not
   held, and inside the watch, so the tail stays awake and polls. */
void test_m3t4_a_press_after_the_tick_release_repaint_is_a_normal_press(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_reset_reason = ESP_RST_DEEPSLEEP;
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_ack_result = BTN_ACK_TOGGLED;
    flow_sleep_mode_answer = WAKE_SLEEP_CONFIG_ERR;
    flow_gate_releases = true;
    flow_promote_out = WAKE_RENDER_FULL;
    flow_arm_break(flow_at(15, 0) + 30, FLOW_PIANO, FLOW_SCREEN);
    flow_deferred_press_btn = BTN_C; /* ✓2, made on the repainted checklist */
    flow_deferred_press_ms = 400;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_tick());

    TEST_ASSERT_EQUAL_INT(1, flow_deferred_delivered);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flow_log_count(EV_CHORE_ACK), "a press made after the repaint was not honoured");
    TEST_ASSERT_EQUAL_INT(1, flow_ack_idx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, FLOW_RENDER_FLUSHES(), "the release promoted a render after its own");
    TEST_ASSERT_GREATER_OR_EQUAL_INT(1, flow_log_count(EV_PARTIAL));
}

/* LOW-4: A LOCKED WAKE DOES NOT CLAIM THE STRIP. A config-locked device in
   chore mode wakes on D with Config Error on the glass and the gate holds
   it in its window; claimed, the strip would show the checklist's colours
   over the lock screen and stand the sync pixel down for all of it. */
void test_m3t4_a_config_locked_d_wake_in_chore_mode_does_not_claim_the_strip(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_ack_result = BTN_ACK_TOGGLED;
    flow_wakeup_btn = BTN_D;
    flow_sleep_mode_answer = WAKE_SLEEP_CONFIG_ERR;
    flow_bedtime_locks = true; /* the lock holds: its screen, then sleep */

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_BEDTIME, flow_run_button());

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_led_claims, "a locked wake took the sync pixel away from its window");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, flow_log_count(EV_CHORE_LEDS), "chore colours were painted over Config Error");
    TEST_ASSERT_FALSE(s_chore_strip_lit);
}

/* And after a release in that wake the strip stays unclaimed: the
   repainted checklist gets the status paint a tick wake's checklist gets,
   because the claim has one write site and it is above the gate. */
void test_m3t4_a_release_by_d_in_chore_mode_leaves_the_strip_unclaimed(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_ack_result = BTN_ACK_TOGGLED;
    flow_wakeup_btn = BTN_D;
    flow_sleep_mode_answer = WAKE_SLEEP_CONFIG_ERR;
    flow_gate_releases = true;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT(0, flow_led_claims);
    TEST_ASSERT_EQUAL_INT(0, flow_log_count(EV_CHORE_LEDS));
    TEST_ASSERT_TRUE_MESSAGE(flow_log_count(EV_LED) >= 1, "the release wake painted no status at all");
}

/* The control: the same D wake on an unlocked device still claims. */
void test_m3t4_an_unlocked_d_wake_in_chore_mode_still_claims_the_strip(void) {
    flow_tick_clock(flow_at(15, 0));
    flow_mode = APP_MODE_CHORES;
    flow_chore_count = 3;
    flow_ack_result = BTN_ACK_TOGGLED;
    flow_wakeup_btn = BTN_D;

    TEST_ASSERT_EQUAL_INT(FLOW_WAKE_SLEPT, flow_run_button());

    TEST_ASSERT_EQUAL_INT(1, flow_led_claims);
    TEST_ASSERT_TRUE(flow_log_count(EV_CHORE_LEDS) >= 1);
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
    RUN_TEST(test_the_state_assembly_hands_the_battery_read_to_app_state);
    RUN_TEST(test_the_state_assembly_carries_the_firmware_version);
    RUN_TEST(test_paint_carries_the_firmware_version);
    RUN_TEST(test_the_state_assembly_neither_ticks_nor_paints);
    RUN_TEST(test_the_full_repaint_ticks_then_assembles_then_flushes);
    RUN_TEST(test_the_full_repaint_never_goes_partial);
    RUN_TEST(test_the_stat_gather_reads_both_adcs_into_their_own_fields);
    RUN_TEST(test_the_stat_gather_carries_the_charge_lock_either_way);
    RUN_TEST(test_the_stat_gather_publishes_the_image_version);
    RUN_TEST(test_the_stat_gather_publishes_the_decoded_reset_reason);
    RUN_TEST(test_the_stat_gather_reads_the_injected_clock_not_the_wall_clock);
    RUN_TEST(test_the_stat_gather_neither_ticks_nor_paints);
    RUN_TEST(test_the_flag_is_not_set_without_an_edge);
    RUN_TEST(test_any_edge_sets_the_flag_including_a_silent_one);
    RUN_TEST(test_the_flag_survives_a_later_drain_that_found_nothing);
    RUN_TEST(test_row4_a_break_end_forces_every_later_render_full);
    RUN_TEST(test_a_swap_across_the_break_screen_is_full_by_the_boundary_rule);
    /* ---- the guard matrix ---- */
    RUN_TEST(test_row5_the_pause_poll_consumes_only_the_b_bit);
    RUN_TEST(test_row5_a_refused_pause_poll_still_leaves_the_others_latched);
    RUN_TEST(test_row5_the_join_poll_consumes_only_the_b_bit);
    RUN_TEST(test_row5_a_join_poll_with_no_b_press_leaves_the_others_latched);
    RUN_TEST(test_the_pause_poll_consumes_the_b_press_even_when_it_refuses);
    RUN_TEST(test_the_pause_poll_pauses_only_a_running_timer);
    RUN_TEST(test_the_pause_poll_does_nothing_without_a_b_press);
    RUN_TEST(test_the_pause_poll_stamps_the_clock_it_reads_now);
    RUN_TEST(test_the_pause_poll_pauses_at_most_once_per_press);
    RUN_TEST(test_the_join_poll_is_inert_without_a_latched_b);
    RUN_TEST(test_the_join_poll_reports_nothing_when_the_state_map_refuses);
    RUN_TEST(test_the_join_poll_acks_on_the_leds_for_every_accepted_action);
    RUN_TEST(test_the_join_poll_neither_paints_nor_syncs);
    RUN_TEST(test_the_join_poll_applies_the_map_at_the_current_clock);
    RUN_TEST(test_row18_button_b_during_a_break_is_refused_with_no_side_effects);
    RUN_TEST(test_the_break_guard_does_not_block_a_reload_of_an_expired_extra);
    RUN_TEST(test_a_break_press_on_slot_zero_is_still_refused_before_the_map);
    RUN_TEST(test_row18_the_refusal_reads_the_before_parameter_not_timer_get_state);
    RUN_TEST(test_button_b_on_a_non_reloadable_expired_slot_is_refused_by_the_map);
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
    RUN_TEST(test_row19_button_a_touches_no_timer_machinery);
    RUN_TEST(test_button_a_touches_no_slot_in_any_state);
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
    RUN_TEST(test_the_break_tail_poll_drains_the_whole_latch_including_a_and_d);
    RUN_TEST(test_a_lone_d_press_in_the_break_tail_is_consumed_and_ignored);
    RUN_TEST(test_the_break_tail_acts_on_a_lone_button_c_press);
    RUN_TEST(test_the_break_tail_treats_a_map_reload_as_a_real_action);
    RUN_TEST(test_the_break_tail_acts_on_the_highest_priority_latched_press);
    RUN_TEST(test_the_break_tail_never_lets_a_latched_a_press_swallow_a_b_press);
    RUN_TEST(test_a_lone_refused_a_press_in_the_break_tail_is_consumed_and_ignored);
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
    RUN_TEST(test_the_break_screen_lights_the_led_before_the_flush_not_after);
    RUN_TEST(test_the_break_screen_paints_the_state_this_tick_returned);
    RUN_TEST(test_row21_a_break_that_would_cross_bed_time_goes_to_bed_instead);
    RUN_TEST(test_row21_the_bed_time_engage_is_always_audible);
    RUN_TEST(test_row21_the_bed_time_engage_gets_the_gates_own_clock);
    RUN_TEST(test_row21_on_an_unset_clock_the_break_starts_and_bed_time_waits);
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
    RUN_TEST(test_the_summary_counts_the_days_acked_chores);
    RUN_TEST(test_the_summary_chores_are_read_before_the_window_and_the_reset);
    RUN_TEST(test_the_summary_ignores_ack_bits_above_the_configured_count);
    RUN_TEST(test_the_summary_reports_no_chores_without_a_list);
    RUN_TEST(test_the_summary_marks_the_chores_unknown_when_the_list_cannot_be_read);
    RUN_TEST(test_a_cold_boot_with_no_stored_date_queues_no_summary);
    RUN_TEST(test_a_day_an_unset_clock_opened_queues_no_summary_and_still_restores);
    RUN_TEST(test_bug14_a_clock_locked_stand_in_day_does_not_roll_over);
    RUN_TEST(test_a_cold_boot_rollover_still_clears_the_bonus_and_resets);
    RUN_TEST(test_bug14_a_late_synced_power_on_rollover_requeues_the_clear);
    RUN_TEST(test_bug14_a_late_synced_power_on_restore_does_not_requeue_the_clear);
    RUN_TEST(test_bug14_a_power_on_rollover_that_never_syncs_does_not_requeue_the_clear);
    RUN_TEST(test_the_retained_bonus_target_is_cleared_on_every_rollover);
    RUN_TEST(test_the_days_usage_is_read_before_the_window_steps_the_clock);
    RUN_TEST(test_the_days_usage_is_sampled_before_the_completions_loop);
    RUN_TEST(test_the_rollover_effect_order_is_pinned_end_to_end);
    /* ---- the render-grid wait ---- */
    RUN_TEST(test_the_grid_wait_absorbs_the_residue_of_a_running_countdown);
    RUN_TEST(test_the_grid_wait_uses_the_break_countdown_when_a_break_is_selected);
    RUN_TEST(test_the_grid_wait_lands_a_clock_only_state_on_the_wall_minute);
    RUN_TEST(test_a_residue_exactly_at_the_cap_is_waited_out);
    RUN_TEST(test_a_residue_one_second_past_the_cap_renders_in_place);
    RUN_TEST(test_the_cap_the_caller_passes_is_the_one_that_applies);
    RUN_TEST(test_a_state_already_on_its_grid_never_polls_at_all);
    RUN_TEST(test_an_expiry_that_has_already_passed_is_not_waited_on);
    RUN_TEST(test_the_grid_wait_polls_ten_times_a_second);
    RUN_TEST(test_row6_b_press_during_a_long_grid_wait_pauses_at_that_moment);
    RUN_TEST(test_row6_b_press_while_not_running_is_eaten_KNOWN_BUG);
    RUN_TEST(test_a_press_latched_before_the_grid_wait_pauses_before_the_first_delay);
    RUN_TEST(test_the_grid_wait_leaves_the_other_buttons_latched_for_the_tick_drain);
    /* ---- the break tail ---- */
    RUN_TEST(test_the_break_watch_is_inert_when_no_break_is_running);
    RUN_TEST(test_row2_a_running_extra_leaves_the_break_tail_alone);
    RUN_TEST(test_a_break_end_beyond_the_watch_window_is_left_to_the_planner);
    RUN_TEST(test_a_break_end_exactly_at_the_watch_window_is_watched);
    RUN_TEST(test_the_break_watch_lights_the_state_before_it_starts_waiting);
    RUN_TEST(test_a_single_second_of_break_tail_is_still_watched);
    RUN_TEST(test_the_break_watch_waits_out_the_tail_then_chimes_and_repaints);
    RUN_TEST(test_a_break_whose_end_has_already_passed_repaints_without_waiting);
    RUN_TEST(test_the_break_watch_polls_four_times_a_second);
    RUN_TEST(test_row1_button_c_in_the_break_tail_moves_the_selection_and_ends_the_watch);
    RUN_TEST(test_row2_button_b_on_a_break_eligible_extra_starts_it_and_suppresses_the_end);
    RUN_TEST(test_a_press_latched_before_the_break_tail_is_serviced_before_the_first_delay);
    RUN_TEST(test_a_refused_press_in_the_break_tail_does_not_end_the_watch);
    /* ---- the final minute ---- */
    RUN_TEST(test_the_final_minute_watch_is_inert_once_the_expiry_has_passed);
    RUN_TEST(test_one_second_of_countdown_is_still_the_final_minute);
    RUN_TEST(test_a_countdown_beyond_the_watch_window_is_left_to_the_planner);
    RUN_TEST(test_a_countdown_exactly_at_the_watch_window_is_watched);
    RUN_TEST(test_the_final_minute_watch_discards_presses_made_before_it_started);
    RUN_TEST(test_a_stale_clock_with_room_to_spare_sharpens_the_expiry_first);
    RUN_TEST(test_fifteen_seconds_left_is_too_little_room_to_sync);
    RUN_TEST(test_a_fresh_clock_needs_no_sync_however_much_room_there_is);
    RUN_TEST(test_a_sync_that_moved_the_timer_out_of_running_ends_the_watch);
    RUN_TEST(test_the_final_minute_watch_lights_the_state_before_it_counts_down);
    RUN_TEST(test_the_countdown_steps_at_the_quarter_minute_marks);
    RUN_TEST(test_a_late_entry_skips_the_marks_it_has_already_passed);
    RUN_TEST(test_each_countdown_mark_is_painted_against_a_freshly_read_clock);
    RUN_TEST(test_a_wake_one_second_late_does_not_repaint_the_sixty_second_mark);
    RUN_TEST(test_the_last_fifteen_seconds_ride_the_pixels_as_a_binary_count);
    RUN_TEST(test_the_binary_count_is_written_once_per_second_not_once_per_poll);
    RUN_TEST(test_the_expiry_alert_fires_when_the_countdown_reaches_zero);
    RUN_TEST(test_a_pause_press_in_the_final_minute_repaints_and_cancels_the_expiry);
    RUN_TEST(test_the_final_minute_watch_polls_four_times_a_second);
    RUN_TEST(test_row9_a_break_due_inside_the_final_minute_paints_it_and_ends_the_watch);
    RUN_TEST(test_row9_the_balance_is_asked_on_every_poll_not_only_at_entry);
    RUN_TEST(test_row9_the_break_interval_is_read_once_before_the_loop);
    RUN_TEST(test_row9_a_zero_break_interval_is_never_asked_of_the_balance);
    RUN_TEST(test_the_watch_falls_back_to_the_compile_time_break_interval);
    RUN_TEST(test_the_post_action_render_drains_the_break_before_the_tick);
    RUN_TEST(test_the_post_action_render_ticks_at_the_clock_it_was_handed);
    RUN_TEST(test_button_d_forces_a_full_refresh_the_policy_made_partial);
    RUN_TEST(test_the_same_state_pair_under_any_other_button_stays_partial);
    RUN_TEST(test_a_transition_into_expired_alerts_instead_of_painting);
    RUN_TEST(test_a_reported_swap_onto_an_expired_slot_paints_and_never_alerts);
    RUN_TEST(test_button_d_still_yields_to_the_expiry_alert);
    RUN_TEST(test_the_post_action_render_lights_the_state_before_it_flushes);
    RUN_TEST(test_a_break_that_ended_this_wake_forces_the_post_action_render_full);
    RUN_TEST(test_row11_the_stats_are_posted_only_after_the_paint_completes);
    RUN_TEST(test_row11_a_partial_paint_also_completes_before_the_stats_go_out);
    RUN_TEST(test_row11_the_whole_expiry_alert_completes_before_the_stats_go_out);
    RUN_TEST(test_row11_the_snapshot_is_collected_before_it_is_posted);
    RUN_TEST(test_row11_the_join_runs_after_the_mqtt_phase_is_released);
    RUN_TEST(test_a_tail_with_no_window_open_collects_and_posts_nothing);
    RUN_TEST(test_a_quiet_join_renders_exactly_once);
    RUN_TEST(test_a_join_that_reports_a_change_renders_again);
    RUN_TEST(test_a_quiet_join_that_moved_the_timer_renders_again);
    RUN_TEST(test_an_alerted_join_never_re_renders_even_when_the_state_moved);
    RUN_TEST(test_the_re_render_compares_against_what_was_painted);
    RUN_TEST(test_the_re_render_reads_the_wall_clock_afresh);
    RUN_TEST(test_the_re_render_drains_the_break_before_its_own_tick);
    RUN_TEST(test_button_d_forces_the_re_render_full_as_well);
    RUN_TEST(test_a_break_due_at_the_tail_takes_the_wake_before_any_render);
    RUN_TEST(test_the_break_screen_releases_the_mqtt_phase_then_joins_then_sleeps);
    RUN_TEST(test_no_break_due_falls_straight_through_to_the_render);
    RUN_TEST(test_the_tail_asks_the_break_gate_with_the_clock_it_was_handed);
    RUN_TEST(test_a_running_timer_gets_the_final_minute_watch);
    RUN_TEST(test_every_state_but_running_gets_the_break_watch);
    RUN_TEST(test_the_drain_after_the_final_minute_watch_repaints_a_missed_edge);
    RUN_TEST(test_the_drain_after_the_break_watch_repaints_a_missed_edge);
    RUN_TEST(test_the_drain_is_reached_even_when_neither_watch_acts);
    RUN_TEST(test_row8_a_power_on_wake_paints_immediately);
    RUN_TEST(test_row8_a_deep_sleep_wake_with_the_same_residue_waits_it_out);
    RUN_TEST(test_row8_every_reason_but_deep_sleep_skips_the_grid_wait);
    RUN_TEST(test_row8_a_cold_boot_lights_the_state_pixel_before_the_paint);
    RUN_TEST(test_row8_a_deep_sleep_tick_wake_stays_dark);
    RUN_TEST(test_row8_the_reset_reason_is_asked_at_both_gates);
    RUN_TEST(test_row10_an_expiry_and_a_break_in_the_same_tick_alert_then_break);
    RUN_TEST(test_row10_the_post_render_gate_is_asked_with_a_freshly_read_clock);
    RUN_TEST(test_row10_a_break_already_due_on_arrival_wins_before_the_paint);
    RUN_TEST(test_row10_an_ordinary_tick_asks_the_break_gate_twice);
    RUN_TEST(test_the_tick_wake_rolls_the_day_then_checks_bed_time_then_drains);
    RUN_TEST(test_the_bed_time_gate_can_end_a_tick_wake);
    RUN_TEST(test_the_bed_time_gate_sees_the_rollover_corrected_clock);
    RUN_TEST(test_an_idle_wake_past_the_sync_cadence_opens_one_window);
    RUN_TEST(test_an_idle_wake_inside_the_sync_cadence_opens_none);
    RUN_TEST(test_a_sync_that_stepped_the_clock_is_what_the_paint_uses);
    RUN_TEST(test_a_running_countdown_is_snapped_to_the_minute_before_it_is_painted);
    RUN_TEST(test_a_non_running_state_is_painted_with_the_raw_remaining);
    RUN_TEST(test_the_break_countdown_on_the_panel_is_snapped_too);
    RUN_TEST(test_the_break_screens_own_countdown_is_snapped_too);
    RUN_TEST(test_a_panel_with_no_break_leaves_the_break_countdown_alone);
    RUN_TEST(test_the_lock_gate_may_promote_the_render_to_full);
    RUN_TEST(test_an_unpromoted_render_stays_partial);
    RUN_TEST(test_the_promoted_value_is_what_the_render_switch_reads);
    RUN_TEST(test_a_press_latched_during_the_wake_is_dispatched_before_sleep);
    RUN_TEST(test_a_latched_a_press_never_swallows_the_b_press_beside_it);
    RUN_TEST(test_a_latched_a_press_alone_reaches_its_own_arm_and_no_slot);
    RUN_TEST(test_a_latched_d_press_in_timers_mode_is_never_dispatched_by_the_tick_drain);
    RUN_TEST(test_a_synced_wake_denies_the_latched_press_a_second_window);
    RUN_TEST(test_an_unsynced_wake_lets_the_latched_press_open_its_own_window);
    RUN_TEST(test_the_latch_drain_runs_before_the_event_watch);
    RUN_TEST(test_a_refused_latched_press_never_reaches_the_tail);
    RUN_TEST(test_a_tick_wake_always_ends_in_deep_sleep_under_the_gates_mode);
    RUN_TEST(test_row7_a_button_held_through_a_two_second_sleep_is_ignored);
    RUN_TEST(test_row7_the_ignore_window_is_two_seconds_inclusive);
    RUN_TEST(test_row7_a_different_button_inside_the_window_is_a_new_press);
    RUN_TEST(test_row7_an_empty_held_mask_never_ignores_anything);
    RUN_TEST(test_row7_an_undecoded_wake_is_never_treated_as_a_continuation);
    RUN_TEST(test_row7_every_button_is_guarded_on_its_own_bit);
    RUN_TEST(test_the_sleep_entry_note_records_the_pads_and_the_clock);
    RUN_TEST(test_row7_the_guard_reads_back_what_the_sleep_entry_recorded);
    RUN_TEST(test_row7_a_sleep_that_recorded_no_hold_lets_the_next_press_through);
    RUN_TEST(test_the_button_wake_acks_on_the_pixels_before_any_other_work);
    RUN_TEST(test_the_button_wake_rolls_over_then_beds_then_drains_the_break);
    RUN_TEST(test_the_bed_time_gate_can_end_a_button_wake);
    RUN_TEST(test_a_wake_press_is_dispatched_through_the_shared_guard_matrix);
    RUN_TEST(test_a_wake_press_on_button_b_runs_the_state_map);
    RUN_TEST(test_a_wake_press_on_button_a_moves_no_timer);
    RUN_TEST(test_a_wake_press_is_always_allowed_its_network_window);
    RUN_TEST(test_button_d_syncs_before_the_paint_and_re_reads_the_clock);
    RUN_TEST(test_button_d_with_no_window_available_skips_the_ntp_wait);
    RUN_TEST(test_an_undecoded_button_wake_still_renders_and_sleeps);
    RUN_TEST(test_the_wake_press_stale_edge_is_drained_before_the_tail);
    RUN_TEST(test_a_resume_into_the_final_minute_is_not_re_paused_by_its_own_stale_edge);
    RUN_TEST(test_a_button_wake_always_ends_in_deep_sleep_under_the_gates_mode);
    RUN_TEST(test_a_button_wake_whose_action_pushed_the_balance_over_breaks);
    RUN_TEST(test_bug1_an_idle_sync_wake_does_drain_the_latch_before_sleep);
    RUN_TEST(test_row6_the_bug2_eat_also_happens_on_the_idle_sync_path_KNOWN_BUG);
    RUN_TEST(test_row8_a_residue_one_second_past_the_cap_renders_in_place);
    RUN_TEST(test_row8_a_pause_inside_the_grid_wait_is_a_change_the_render_must_see);
    RUN_TEST(test_the_guaranteed_drain_catches_an_edge_that_appeared_during_the_watch);
    RUN_TEST(test_a_sync_that_stepped_the_clock_is_what_the_fast_path_gate_sees);
    RUN_TEST(test_an_off_grid_countdown_is_painted_exactly_as_the_tick_reported_it);
    RUN_TEST(test_an_off_grid_break_countdown_reaches_the_panel_untouched);
    RUN_TEST(test_a_break_that_ended_this_wake_forces_the_tick_render_full);
    RUN_TEST(test_the_latch_drain_runs_before_the_event_watch_can_discard_it);
    RUN_TEST(test_row10_the_post_render_break_takes_the_wake_before_the_latched_press);
    RUN_TEST(test_the_button_wake_captures_the_state_the_break_drain_left_behind);
    RUN_TEST(test_a_break_at_the_tail_still_drains_the_stale_edge_first);
    RUN_TEST(test_the_button_wake_reaches_the_event_watch_before_it_sleeps);
    RUN_TEST(test_row13_an_ext1_wake_goes_to_the_button_handler);
    RUN_TEST(test_row13_a_timer_wake_goes_to_the_tick_handler);
    RUN_TEST(test_row13_a_cold_boot_reports_no_cause_and_still_ticks);
    RUN_TEST(test_row13_ext1_alongside_the_timer_is_still_a_button_wake);
    RUN_TEST(test_row13_a_panic_reset_stays_quiet_before_the_boot_prints);
    RUN_TEST(test_row13_no_other_reset_reason_delays_the_boot);
    RUN_TEST(test_row13_an_undrained_break_end_is_taken_at_sleep);
    RUN_TEST(test_row13_the_undrained_take_actually_consumes_the_latch);
    RUN_TEST(test_row13_a_wake_that_drained_its_break_reports_nothing_at_sleep);
    RUN_TEST(test_row13_the_sleep_drain_has_no_side_effects_beyond_the_take);
    /* M2-T2: the painted mode and its reverts (C16, C17) */
    RUN_TEST(test_c16_a_break_end_takes_the_mode_back_to_timers);
    RUN_TEST(test_c16_a_silently_ended_break_reverts_the_mode_too);
    RUN_TEST(test_c16_a_wake_with_no_break_edge_leaves_the_mode_alone);
    RUN_TEST(test_c16_an_undrained_break_end_at_sleep_still_reverts);
    RUN_TEST(test_c16_a_sleep_with_no_undrained_edge_leaves_the_mode_alone);
    RUN_TEST(test_c16_a_day_rollover_reverts_the_mode_through_timer_reset);
    RUN_TEST(test_c16_a_same_day_restore_is_not_a_rollover_and_keeps_chore_mode);
    RUN_TEST(test_c16_an_emptied_chore_list_reverts_the_mode_at_paint_time);
    RUN_TEST(test_c16_the_emptied_list_guard_stores_once_not_on_every_paint);
    RUN_TEST(test_c16_the_shipped_default_paints_timers_and_stores_nothing);
    RUN_TEST(test_c16_the_final_ack_does_not_revert_the_mode);
    RUN_TEST(test_c16_a_chore_mode_tick_wake_runs_exactly_the_same_effects);
    RUN_TEST(test_c16_a_chore_mode_tick_wake_still_drains_the_break_end);
    RUN_TEST(test_c16_a_chore_mode_button_wake_still_drains_the_break_end);
    RUN_TEST(test_c17_an_unattended_chore_mode_wake_repaints_with_the_pixels_dark);

    /* M2-T8: the NeoPixel ack sequencing (design §2.5) */
    RUN_TEST(test_t8_a_chore_mode_button_wake_paints_the_pre_press_state_first);
    RUN_TEST(test_t8_the_flip_carries_the_row_the_press_ticked);
    RUN_TEST(test_t8_the_first_ack_of_a_wake_holds_the_pre_press_state_before_it_flips);
    RUN_TEST(test_t8_the_ack_hold_is_one_figure_shared_with_button_bs_start);
    RUN_TEST(test_t8_a_second_ack_in_the_same_wake_flips_with_no_hold);
    RUN_TEST(test_t8_the_flip_reaches_the_pixels_before_the_panel_refresh);
    RUN_TEST(test_t8_a_latched_ack_on_a_tick_wake_leaves_the_strip_dark);
    RUN_TEST(test_t8_a_chore_mode_wake_never_paints_a_timer_colour_over_a_chore_row);
    RUN_TEST(test_t8_a_timer_mode_button_wake_lights_the_timer_pixel_and_no_strip);
    RUN_TEST(test_t8_the_checklist_claims_the_sync_pixel_before_the_rollover_opens_a_window);
    RUN_TEST(test_t8_an_expiry_alarm_repaints_the_strip_it_wiped);
    RUN_TEST(test_t8_a_refused_ack_neither_holds_nor_flips);
    RUN_TEST(test_t8_a_press_made_during_the_panel_refresh_is_not_discarded);
    RUN_TEST(test_t8_a_second_ack_coalesces_into_the_same_refresh_with_no_break);
    RUN_TEST(test_t8_two_acks_latched_in_one_window_both_land);
    RUN_TEST(test_t8_a_mode_reverted_under_the_wake_stops_routing_presses_to_acks);
    RUN_TEST(test_t15_a_lone_ack_waits_the_quiet_window_not_the_hold);
    RUN_TEST(test_t8_the_coalescing_window_is_bounded_per_wake);
    RUN_TEST(test_t15_presses_less_than_a_window_apart_share_one_paint);
    RUN_TEST(test_t15_a_press_during_the_paint_waits_a_quiet_window_before_the_repaint);
    RUN_TEST(test_t15_a_press_during_the_second_paint_is_not_discarded);
    RUN_TEST(test_t15_the_burst_budget_does_not_count_panel_time);
    RUN_TEST(test_t15_a_continuous_pad_gets_one_paint_past_the_budget);
    RUN_TEST(test_t15_button_a_after_the_last_tick_goes_back_to_timers);
    RUN_TEST(test_t15_button_a_during_the_first_paint_goes_back_to_timers);
    RUN_TEST(test_t15_a_refused_button_a_during_the_gesture_changes_nothing);
    RUN_TEST(test_t15_an_ack_and_a_in_one_poll_apply_the_ack_first);
    RUN_TEST(test_t15_an_ack_and_a_made_during_a_paint_apply_the_ack_first);
    RUN_TEST(test_t15_button_a_after_the_budget_still_goes_back_to_timers);
    RUN_TEST(test_t15_only_the_first_tail_repaint_after_a_break_end_is_full);
    RUN_TEST(test_m2_a_join_that_moves_the_screen_kind_repaints_full);
    RUN_TEST(test_m2_a_join_that_empties_the_list_repaints_full);
    RUN_TEST(test_m2_a_join_that_keeps_the_screen_stays_partial);
    RUN_TEST(test_t12_the_coalescing_loop_polls_at_least_as_often_as_the_debounce);
    RUN_TEST(test_t12_a_mis_press_on_a_full_list_can_be_corrected_in_the_same_wake);
    RUN_TEST(test_t12_a_correction_press_is_accepted_at_a_real_poll_cadence);
    RUN_TEST(test_t12_release_bounce_on_a_held_button_is_not_a_second_press);
    RUN_TEST(test_t8_a_wake_with_no_ack_spends_no_coalescing_window);
    RUN_TEST(test_t8_a_running_timer_keeps_its_pixel_even_in_chore_mode);
    RUN_TEST(test_t8_an_empty_chore_list_claims_nothing);

    /* M2-T3: Button A reaches the toggle, on every path (row C3) */
    RUN_TEST(test_c3_button_a_toggles_the_mode_in_the_dispatch);
    RUN_TEST(test_c3_button_a_toggles_back_out_of_chore_mode);
    RUN_TEST(test_c3_a_refused_button_a_reports_false_and_stores_nothing);
    RUN_TEST(test_c3_a_break_end_in_the_same_wake_does_not_undo_the_toggle);
    RUN_TEST(test_c3_a_break_end_still_reverts_when_the_press_leaves_chore_mode);
    RUN_TEST(test_c3_a_refused_button_a_leaves_the_break_end_latched);
    RUN_TEST(test_c3_the_break_tail_poll_acts_on_a_lone_button_a_press);
    RUN_TEST(test_c3_a_button_b_press_still_beats_a_button_a_press);
    RUN_TEST(test_c3_the_tick_wake_latch_drain_acts_on_a_latched_button_a_press);
    RUN_TEST(test_c3_a_button_a_ext1_wake_toggles_the_mode);
    RUN_TEST(test_c3_an_ext1_wake_that_also_drains_a_break_end_toggles_from_timers);
    RUN_TEST(test_c3_a_tick_latch_drain_after_a_break_end_toggles_from_timers);
    RUN_TEST(test_c3_a_refused_button_a_wake_repaints_the_current_screen);
    RUN_TEST(test_c3_a_config_edit_that_empties_the_list_after_the_press_still_wins);
    RUN_TEST(test_a_mode_toggle_is_painted_full_not_partial);
    RUN_TEST(test_a_toggle_back_out_of_chore_mode_is_also_full);
    RUN_TEST(test_a_refused_button_a_is_not_promoted_to_a_full_refresh);
    RUN_TEST(test_the_toggle_promotion_survives_into_the_post_join_repaint);
    RUN_TEST(test_a_screen_kind_change_from_the_timer_state_alone_is_painted_full);
    RUN_TEST(test_the_other_direction_of_a_screen_kind_change_is_full_too);
    RUN_TEST(test_a_state_change_that_keeps_the_same_screen_stays_partial);
    RUN_TEST(test_c16_a_break_that_starts_in_chore_mode_reverts_to_timers);
    RUN_TEST(test_c16_the_break_start_revert_survives_an_ack_earlier_in_the_wake);
    RUN_TEST(test_a_day_rollover_arms_an_update_check_before_it_opens_the_window);
    RUN_TEST(test_a_day_that_has_not_rolled_over_arms_nothing);
    RUN_TEST(test_the_rollover_arm_carries_the_battery_and_the_charge_lock);
    RUN_TEST(test_an_unreadable_battery_arms_as_unknown_rather_than_as_flat);
    RUN_TEST(test_button_d_arms_a_sync_check_before_it_opens_its_window);
    RUN_TEST(test_button_b_opens_a_window_without_arming_a_check);
    RUN_TEST(test_the_periodic_sync_window_arms_no_check);
    RUN_TEST(test_the_ota_repaint_seam_paints_the_normal_screen);
    RUN_TEST(test_a_pending_update_is_applied_after_the_join_and_before_the_sleep);
    RUN_TEST(test_a_wake_with_nothing_pending_opens_no_second_window);
    RUN_TEST(test_a_download_task_that_cannot_be_spawned_is_reported);
    RUN_TEST(test_a_download_task_that_spawns_reports_nothing_from_here);
    RUN_TEST(test_the_button_handler_applies_a_pending_update_too);
    RUN_TEST(test_the_apply_samples_the_battery_again_instead_of_reusing_the_arms);
    RUN_TEST(test_the_apply_runs_after_the_pre_sleep_event_watch);
    RUN_TEST(test_the_apply_carries_the_charge_lock_it_sampled);
    /* M2-T4a — B, C and D bound to the chore acks */
    RUN_TEST(test_c4a_b_in_chore_mode_acks_row_1_instead_of_starting);
    RUN_TEST(test_c4a_c_in_chore_mode_acks_row_2_instead_of_swapping);
    RUN_TEST(test_c4a_a_refused_ack_does_not_fall_through_to_the_timer_action);
    RUN_TEST(test_c4a_outside_chore_mode_b_and_c_keep_their_timer_jobs);
    RUN_TEST(test_c4a_an_ack_is_not_refused_during_a_screen_break);
    RUN_TEST(test_c4a_an_ack_drains_a_pending_break_end_and_keeps_chore_mode);
    RUN_TEST(test_c4a_a_refused_ack_leaves_the_break_end_latched);
    RUN_TEST(test_c4a_d_in_chore_mode_acks_row_3_instead_of_checking_for_an_update);
    RUN_TEST(test_c4a_d_outside_chore_mode_still_checks_for_an_update);
    RUN_TEST(test_c4b_a_latched_d_press_in_the_break_tail_acks_row_3);
    RUN_TEST(test_c4b_a_latched_d_press_in_chore_mode_acks_row_3);
    RUN_TEST(test_c4b_a_latched_d_press_outside_chore_mode_is_still_dropped);
    RUN_TEST(test_c4b_a_latched_b_press_outranks_a_latched_d_in_the_break_tail);
    RUN_TEST(test_c4a_an_ack_renders_a_partial_not_a_full);
    RUN_TEST(test_c4a_d_outside_chore_mode_still_forces_a_full_refresh);
    RUN_TEST(test_c4a_the_ack_suppression_survives_into_the_post_join_repaint);
    RUN_TEST(test_c4a_the_join_poll_neither_starts_nor_acks_in_chore_mode);
    RUN_TEST(test_c4a_the_join_poll_outside_chore_mode_still_applies_b);
    RUN_TEST(test_m3t4_a_release_by_d_in_chore_mode_ticks_nothing);
    RUN_TEST(test_m3t4_a_release_by_d_on_the_timer_screen_opens_no_second_window);
    RUN_TEST(test_m3t4_the_release_repaint_is_full_whatever_the_button);
    RUN_TEST(test_m3t4_the_release_promotes_only_the_first_render);
    RUN_TEST(test_m3t4_a_later_render_in_the_release_wake_is_not_promoted);
    RUN_TEST(test_m3t4_a_d_press_that_does_not_release_the_lock_does_nothing_else);
    RUN_TEST(test_m3t4_with_no_release_d_still_ticks_row_3_in_chore_mode);
    RUN_TEST(test_m3t4_with_no_release_d_still_opens_its_window_on_the_timer_screen);
    RUN_TEST(test_m3t4_a_tick_release_drops_a_press_made_under_the_lock);
    RUN_TEST(test_m3t4_a_tick_wake_with_no_release_keeps_its_latched_press);
    RUN_TEST(test_m3t4_the_join_poll_drops_b_under_every_lock);
    RUN_TEST(test_m3t4_the_join_poll_drops_b_under_a_lock_in_chore_mode_too);
    RUN_TEST(test_m3t4_a_d_pressed_in_the_grid_wait_after_a_tick_release_ticks_nothing);
    RUN_TEST(test_m3t4_a_b_pressed_in_the_grid_wait_after_a_tick_release_is_dropped);
    RUN_TEST(test_m3t4_the_sync_after_a_tick_release_drops_b_in_its_join);
    RUN_TEST(test_m3t4_the_sync_on_an_unlocked_tick_wake_still_applies_b);
    RUN_TEST(test_m3t4_a_press_after_the_tick_release_repaint_is_a_normal_press);
    RUN_TEST(test_m3t4_a_config_locked_d_wake_in_chore_mode_does_not_claim_the_strip);
    RUN_TEST(test_m3t4_a_release_by_d_in_chore_mode_leaves_the_strip_unclaimed);
    RUN_TEST(test_m3t4_an_unlocked_d_wake_in_chore_mode_still_claims_the_strip);
    return UNITY_END();
}
