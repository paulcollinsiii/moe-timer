#include <string.h>
#include <unity.h>

/* Single-TU: the real policies the break-end owner consults — the chime
   grace window and the render choice — are compiled in alongside the
   module under test, so the edges below are pinned against the shipping
   rules rather than a restatement of them. The mock clock comes along
   because every decision here is a wall-time comparison. Everything with
   a device behind it gets a link-time spy stub in the preamble below. */
// clang-format off
#include "../../main/wake_policy.c"
#include "mock_hal_time.c"
// clang-format on

#include "audio.h"
#include "sleep_plan.h" /* BREAK_CHIME_GRACE_SEC */
#include "timer.h"
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
} flow_event_t;

static flow_event_t flow_log[16];
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

void setUp(void) {
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
   `before`. It is kept as the executable statement of the precondition
   this module's flag is combined with at all three render sites — that
   `before` is the state which was actually PAINTED, so a swap across the
   break screen's full-screen inversion is caught by the boundary rule.
   The pure-policy half is already pinned by test_wake_policy
   (test_render_selection_change_across_the_break_screen_is_full).

   The row-4 defect proper (1520486 defect 1 — dispatch_button_action
   rewriting *before on a successful swap, so four presses in five
   ghosted) lives in dispatch_button_action, which is still in main.c.
   NOTHING pins it, here or anywhere, until cycle 11 moves that function. */
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
    return UNITY_END();
}
