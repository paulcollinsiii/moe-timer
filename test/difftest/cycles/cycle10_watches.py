#!/usr/bin/env python3
"""Generate a differential harness: OLD (a7050d4 main.c) vs NEW (wake_flow.c)
for the three awake watches, driven over a cartesian product, comparing FULL
ORDERED EFFECT TRACES.

Functions are extracted MECHANICALLY (brace matching by name) from the two
sources so no body is hand-transcribed. Only these substitutions are made,
each of them a change this refactor explicitly claims is device-identical:
  * time(NULL)                    -> hal_time_now()
    (hal_time.c: `return time(NULL);`)
  * vTaskDelay(pdMS_TO_TICKS(n))  -> hal_delay_ms(n)
    (hal_time.c: `void hal_delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }`
     - the delay seam added in the first cycle of this refactor)
  * symbol renaming to old_/new_ namespaces
Both are TEXTUAL substitutions applied to the OLD body before comparison, so
they make the two sides agree by construction on the IDENTITY of the clock
read and of the delay. What is still pinned - and what actually matters for a
polling loop - is their COUNT, their ARGUMENTS and their POSITION in the
trace.

The NEW side routes its two renders through the state-assembly seam
(make_display_state), which is itself extracted mechanically from the CURRENT
wake_flow.c - it lived in main.c until the residency audit's review moved it,
"the battery ADC read has no host answer" not being one of the four residency
reasons and lock_gate.c refuting it anyway. The OLD side calls main.c's
make_state directly, which the harness models. The extracted seam and that
model are deliberately trace-equivalent - same ADC read, same log line, same
E_MAKE_STATE - so both sides reach the same modelled read + assembly.

wake_policy.c is compiled in for real rather than stubbed: a stub would be a
second implementation of the wait length and the countdown schedule to keep in
sync, and the sweep is comparing OLD against NEW, not against a model.
"""
import re
import subprocess
import sys
import os

# Derived from this script's own location, never hardcoded. A fixed absolute
# path sweeps whatever tree it names rather than the one you are working in, so
# a run from another checkout reports a green control about code that is not in
# front of you - and the sweep stops working outright the day that tree is
# deleted. run.sh derives its own copy the same way, from the same file layout.
OUT = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(OUT, os.pardir, os.pardir, os.pardir))

# Pinned to an annotated tag, not a bare short SHA: the tag keeps the baseline
# commit reachable even if the branch it was made on is deleted or rewritten,
# and cannot go ambiguous as history grows. The full SHA is recorded so the tag
# can be recreated if it is ever lost - preflight() prints the command.
BASE = "difftest-base/cycle10"
BASE_SHA = "a7050d4724c799b092226aac3dfdae0d89e2ef53"


def preflight():
    """Refuse to sweep the wrong tree, or a baseline that no longer resolves."""
    try:
        top = subprocess.check_output(
            ["git", "-C", REPO, "rev-parse", "--show-toplevel"],
            text=True, stderr=subprocess.DEVNULL).strip()
    except (subprocess.CalledProcessError, OSError):
        sys.exit(f"difftest: {REPO} is not a git working tree")
    if not os.path.samefile(top, REPO):
        sys.exit(f"difftest: {REPO} is not the root of its working tree ({top}) - "
                 "this generator must sit three levels below the repo root")
    if subprocess.call(["git", "-C", REPO, "rev-parse", "--verify", "--quiet",
                        BASE + "^{commit}"], stdout=subprocess.DEVNULL) != 0:
        sys.exit(f"difftest: baseline {BASE} is missing from {REPO}. Recreate it:\n"
                 f"  git -C {REPO} tag -a {BASE} -m 'difftest baseline' {BASE_SHA}")


def read_git(rev, path):
    return subprocess.check_output(["git", "-C", REPO, "show", f"{rev}:{path}"], text=True)


def read_file(path):
    with open(os.path.join(REPO, path)) as f:
        return f.read()


preflight()


def extract(src, name):
    """Pull a whole function definition out by name via brace matching."""
    m = re.search(r"^[A-Za-z_][A-Za-z0-9_ \*]*\b" + re.escape(name) + r"\s*\([^;]*?\)\s*\{", src, re.M | re.S)
    if not m:
        sys.exit(f"could not find definition of {name}")
    start = m.start()
    i = src.index("{", m.start())
    depth = 0
    while True:
        c = src[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                break
        i += 1
    return src[start:i + 1]


def namespace(body, prefix, names):
    for n in names:
        body = re.sub(r"\b" + re.escape(n) + r"\b", prefix + n, body)
    return body


old_main = read_git(BASE, "main/main.c")
new_flow = read_file("main/wake_flow.c")

OLD_NAMES = ["wait_for_render_grid", "watch_break_end", "watch_final_minute"]
NEW_NAMES = ["wake_flow_wait_for_render_grid", "wake_flow_watch_break_end", "wake_flow_watch_final_minute"]

old_bodies = "\n\n".join(extract(old_main, n) for n in OLD_NAMES)
new_bodies = "\n\n".join(extract(new_flow, n) for n in NEW_NAMES)
seams = extract(new_flow, "make_display_state")

# --- the only permitted rewrites -------------------------------------------
old_bodies = old_bodies.replace("time(NULL)", "hal_time_now()")
old_bodies = re.sub(r"vTaskDelay\(pdMS_TO_TICKS\((\d+)\)\)", r"hal_delay_ms(\1)", old_bodies)
if "vTaskDelay" in old_bodies or "pdMS_TO_TICKS" in old_bodies:
    sys.exit("a vTaskDelay survived the delay-seam rewrite - the sweep would not build")
old_bodies = namespace(old_bodies, "old_", OLD_NAMES)
new_bodies = namespace(new_bodies, "new_", NEW_NAMES)
seams = re.sub(r"\bstatic\b\s+", "", seams)  # make_state is ours; the seam stays non-static

# Both sides are file-scope here, so drop the `static` the OLD definitions
# carried inside main.c.
old_bodies = re.sub(r"^static\s+", "", old_bodies, flags=re.M)

MUTANTS = {
    "control": [],
    # ---- ROW 1 / ROW 2: the break tail must service buttons (1f954da) ----
    "row1_delete_break_tail_poll": [
        ("            if (wake_flow_poll_break_buttons())\n                return; /* repainted; the planner owns the end from here */",
         "            if (false)\n                return; /* repainted; the planner owns the end from here */")],
    "row2_delete_extra_running_suppression": [
        ("    if (timer_any_extra_running()) {", "    if (false) {")],
    # ---- ROW 6: the pause poll inside the grid wait (2d9d61f) ----
    "row6_delete_grid_wait_pause_poll": [
        ("        if (wake_flow_poll_pause_button())\n            return;",
         "        if (false)\n            return;")],
    "row6_pause_poll_after_the_delay": [
        ("        if (wake_flow_poll_pause_button())\n            return;\n        hal_delay_ms(100);",
         "        hal_delay_ms(100);\n        if (wake_flow_poll_pause_button())\n            return;")],
    # ---- ROW 9: the break check must stay INSIDE the loop (f2008da) ----
    "row9_break_check_removed_from_the_loop": [
        ("        if (break_interval_min != 0 && timer_break_due(hal_time_now(), (int32_t)break_interval_min * 60)) {",
         "        if (false && timer_break_due(hal_time_now(), (int32_t)break_interval_min * 60)) {")],
    "row9_break_check_before_the_pause_poll": [
        ("""        if (wake_flow_poll_pause_button()) {
            neopixel_stop(); /* clear the binary-countdown pixels */
            time_t pnow = hal_time_now();
            display_state_t st = make_display_state(timer_tick(pnow), pnow);
            status_led_show_timer_state(); /* amber through the refresh until sleep */
            display_full_refresh(&st);
            return;
        }
        if (break_interval_min != 0 && timer_break_due(hal_time_now(), (int32_t)break_interval_min * 60)) {
            neopixel_stop(); /* clear the binary-countdown pixels */""",
         """        if (break_interval_min != 0 && timer_break_due(hal_time_now(), (int32_t)break_interval_min * 60)) {
            neopixel_stop(); /* clear the binary-countdown pixels */
            if (wake_flow_maybe_start_break(hal_time_now())) {
                return;
            }
        }
        if (wake_flow_poll_pause_button()) {
            neopixel_stop(); /* clear the binary-countdown pixels */
            time_t pnow = hal_time_now();
            display_state_t st = make_display_state(timer_tick(pnow), pnow);
            status_led_show_timer_state(); /* amber through the refresh until sleep */
            display_full_refresh(&st);
            return;
        }
        if (false) {
            neopixel_stop();""")],
    "row9_interval_never_read_from_nvs": [
        ("    uint16_t break_interval_min = NVS_DEFAULT_BREAK_INTERVAL_MIN;\n"
         "    nvs_config_get_break_interval_min(&break_interval_min);",
         "    uint16_t break_interval_min = NVS_DEFAULT_BREAK_INTERVAL_MIN;")],
    # ---- +-1 on every boundary the watches own ----
    "boundary_watch_window_plus1": [
        ("if (remaining <= 0 || remaining > SLEEP_PLAN_WATCH_SEC)",
         "if (remaining <= 0 || remaining >= SLEEP_PLAN_WATCH_SEC)")],
    "boundary_expiry_passed_minus1": [
        ("if (remaining <= 0 || remaining > SLEEP_PLAN_WATCH_SEC)",
         "if (remaining < 0 || remaining > SLEEP_PLAN_WATCH_SEC)")],
    "boundary_break_tail_window_plus1": [
        ("    if (brem > SLEEP_PLAN_WATCH_SEC)", "    if (brem >= SLEEP_PLAN_WATCH_SEC)")],
    "boundary_brem_zero_plus1": [("    if (brem > 0) {", "    if (brem >= 0) {")],
    "boundary_sync_room_minus1": [("&& remaining > 15) {", "&& remaining >= 15) {")],
    "boundary_binary_threshold_plus1": [
        ("        if (rem <= 15 && (int32_t)rem != leds_shown) {",
         "        if (rem <= 16 && (int32_t)rem != leds_shown) {")],
    "boundary_grid_poll_count_minus1": [
        ("    for (int32_t i = 0; i < to * 10; i++) {", "    for (int32_t i = 0; i < to * 10 - 1; i++) {")],
    "boundary_countdown_step_index_plus1": [
        ("if (next_step < WAKE_COUNTDOWN_STEPS && rem <= (int64_t)wake_policy_countdown_step(next_step)) {",
         "if (next_step < WAKE_COUNTDOWN_STEPS && rem <= (int64_t)wake_policy_countdown_step(next_step + 1)) {")],
    # ---- deletion ----
    "delete_pre_watch_press_discard": [
        ("    buttons_take_pressed(); /* only presses made DURING the watch may pause */", "    ;")],
    "delete_leds_dedupe": [
        ("        if (rem <= 15 && (int32_t)rem != leds_shown) {", "        if (rem <= 15) {")],
    "delete_break_tail_led": [
        ("        status_led_show_timer_state();\n        while (timer_break_remaining(hal_time_now()) > 0) {",
         "        while (timer_break_remaining(hal_time_now()) > 0) {")],
    # ---- reordering ----
    "reorder_led_after_the_flush": [
        ("            status_led_show_timer_state(); /* amber through the refresh until sleep */\n"
         "            display_full_refresh(&st);",
         "            display_full_refresh(&st);\n"
         "            status_led_show_timer_state(); /* amber through the refresh until sleep */")],
    "reorder_tick_after_the_alert": [
        ("    timer_tick(hal_time_now()); /* RUNNING -> EXPIRED */\n    wake_flow_fire_expiry_alert();",
         "    wake_flow_fire_expiry_alert();\n    timer_tick(hal_time_now()); /* RUNNING -> EXPIRED */")],
    "reorder_pixels_off_after_the_paint": [
        ("            neopixel_stop(); /* clear the binary-countdown pixels */\n"
         "            time_t pnow = hal_time_now();",
         "            time_t pnow = hal_time_now();"),
        ("            display_full_refresh(&st);\n            return;",
         "            display_full_refresh(&st);\n            neopixel_stop();\n            return;")],
    # ---- clock-read count and position ----
    "move_clock_sample_reuse_entry_clock": [
        ("            time_t pnow = hal_time_now();", "            time_t pnow = now;")],
    "move_clock_sample_break_due_reuses_rem": [
        ("timer_break_due(hal_time_now(), (int32_t)break_interval_min * 60)",
         "timer_break_due((time_t)(timer_expiry_wall() - rem), (int32_t)break_interval_min * 60)")],
    "grid_wait_reads_the_clock_twice": [
        ("    int32_t to = wake_policy_grid_wait_sec(st, event_remaining, (int)(now % 60), max_wait_sec);",
         "    int32_t to = wake_policy_grid_wait_sec(st, event_remaining, (int)(hal_time_now() % 60), max_wait_sec);")],
    # ---- cadence ----
    "cadence_final_minute_plus1ms": [
        ("        hal_delay_ms(250);\n    }\n    timer_tick(hal_time_now());",
         "        hal_delay_ms(251);\n    }\n    timer_tick(hal_time_now());")],
    "cadence_grid_wait_plus1ms": [("        hal_delay_ms(100);", "        hal_delay_ms(101);")],
}


def apply_mutant(bodies, seamsrc, edits):
    """Apply each edit to whichever of the two NEW-side sources holds it.
    Missing anchors are fatal: a mutant that silently did nothing would
    read as an equivalence."""
    for a, b in edits:
        if a in bodies:
            bodies = bodies.replace(a, b, 1)
        elif a in seamsrc:
            seamsrc = seamsrc.replace(a, b, 1)
        else:
            sys.exit(f"mutant anchor not found:\n{a}")
    return bodies, seamsrc


PRE = r"""
/* Differential sweep harness - generated, never committed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "app_state.h"
#include "buttons.h"
#include "display.h"
#include "hal_time.h"
#include "neopixel.h"
#include "net_apply.h"
#include "nvs_config.h"
#include "nvs_defaults.h"
#include "sleep_plan.h"
#include "status_led.h"
#include "timer.h"
#include "wake_flow.h"
#include "wake_policy.h"

/* The real policy, not a model: the wait length, the countdown schedule and
   the late-wake skip are all decisions both sides delegate to it. */
#include "wake_policy.c"

/* Both sides get log macros that EVALUATE their arguments, exactly as the
   real ESP_LOG* do on device. wake_flow.c's NATIVE block discards them, so
   anything smuggled into a log vararg is invisible to the committed host
   suite - here it is not. (Neither watch has a call inside a log argument
   today; this keeps that true rather than assuming it.) */
static void diff_logv(const char *tag, const char *fmt, ...);
#define ESP_LOGI(tag, ...) diff_logv(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) diff_logv(tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) diff_logv(tag, __VA_ARGS__)
static const char *TAG = "diff";

/* The extracted state-assembly seam reads it; wake_flow.c derives it from
   the Kconfig bool and only the function body is extracted. false is the
   shipping configuration, and the flag reaches nothing this sweep
   compares - it is one field of the app_state_in_t both sides fill. */
#define PARENT_TESTING false

/* ---- the ordered effect trace ------------------------------------------ */
enum {
    E_CLOCK = 1, E_DELAY, E_GET_STATE, E_EXPIRY_WALL, E_BREAK_REM, E_BREAK_ACTIVE,
    E_EXTRA_RUNNING, E_NEEDS_SYNC, E_TRY_WINDOW, E_CFG_INT, E_BREAK_DUE,
    E_MAYBE_BREAK, E_EXPIRY_ALERT, E_POLL_PAUSE, E_POLL_BREAK, E_BREAK_REPAINT,
    E_LED, E_PIXELS_OFF, E_BINARY, E_PARTIAL, E_FULL_REFRESH, E_TICK,
    E_TAKE_PRESSED, E_BATT, E_MAKE_STATE, E_LOGLINE,
};

#define TRACE_MAX 8192
typedef struct { int ev; long long a, b, c; } tev_t;
static tev_t g_tr[TRACE_MAX];
static int g_tr_n;
static long g_overflow; /* a truncated trace could hide a divergence */
static void tr(int ev, long long a, long long b, long long c) {
    if (g_tr_n < TRACE_MAX) { g_tr[g_tr_n].ev = ev; g_tr[g_tr_n].a = a;
        g_tr[g_tr_n].b = b; g_tr[g_tr_n].c = c; g_tr_n++; }
    else { g_overflow++; }
}
static void diff_logv(const char *tag, const char *fmt, ...) {
    /* Evaluates the varargs (that is the point) but records only that a
       line happened - the log TEXT is not part of the device contract we
       are pinning, the CALLS inside the argument list are. */
    va_list ap; va_start(ap, fmt); (void)tag; (void)fmt; va_end(ap);
    tr(E_LOGLINE, 0, 0, 0);
}

/* ---- the fake clock ----------------------------------------------------- */
/* Reads ADVANCE it (so an extra or a missing read is visible even when
   nothing else moved), and hal_delay_ms advances it exactly the way the
   committed host mock does - carrying the sub-second remainder, so four
   250 ms polls are one second and a loop waiting on wall time terminates. */
static time_t g_clock;
static int g_clock_step;
static uint32_t g_delay_carry;
time_t hal_time_now(void) {
    g_clock += g_clock_step;
    tr(E_CLOCK, (long long)g_clock, 0, 0);
    return g_clock;
}
void hal_delay_ms(uint32_t ms) {
    tr(E_DELAY, (long long)ms, 0, 0);
    g_delay_carry += ms;
    g_clock += (time_t)(g_delay_carry / 1000u);
    g_delay_carry %= 1000u;
}

/* ---- the injected model -------------------------------------------------- */
typedef struct {
    int state;                /* timer_state_t */
    long long expiry_off;     /* expiry wall, relative to the clock seed */
    int break_active, extra_running;
    long long break_off;      /* break wall end, relative to the clock seed */
    int needs_sync;
    int state_after_window;   /* -1 = the window leaves the state alone */
    int cfg_writes;
    int interval;
    long long due_from_off;   /* -1 = never due */
    int break_starts;
    int pause_poll_at;        /* poll index that pauses; -1 = never */
    int bpoll_at;             /* break-tail poll index that acts; -1 = never */
    long tick_ret;
    int batt_mv;
    int clock_step;
    time_t clock_seed;
} params_t;
static params_t P;

static int m_state;
static int64_t m_expiry_wall;
static int m_break_active, m_extra_running;
static time_t m_break_wall_end, m_due_from;
static int g_pause_polls, g_bpolls;

static void reset_run(void) {
    g_clock = P.clock_seed;
    g_clock_step = P.clock_step;
    g_delay_carry = 0;
    g_tr_n = 0;
    g_pause_polls = 0;
    g_bpolls = 0;
    m_state = P.state;
    m_expiry_wall = (int64_t)P.clock_seed + P.expiry_off;
    m_break_active = P.break_active;
    m_extra_running = P.extra_running;
    m_break_wall_end = P.clock_seed + (time_t)P.break_off;
    m_due_from = (P.due_from_off < 0) ? 0 : P.clock_seed + (time_t)P.due_from_off;
}

timer_state_t timer_get_state(void) {
    tr(E_GET_STATE, m_state, 0, 0);
    return (timer_state_t)m_state;
}
int64_t timer_expiry_wall(void) {
    tr(E_EXPIRY_WALL, (long long)m_expiry_wall, 0, 0);
    return m_expiry_wall;
}
/* timer.c's own arithmetic, clamp included. */
int32_t timer_break_remaining(time_t now) {
    int32_t r = 0;
    if (m_break_active) {
        int64_t left = (int64_t)m_break_wall_end - (int64_t)now;
        r = left > 0 ? (int32_t)left : 0;
    }
    tr(E_BREAK_REM, (long long)now, r, 0);
    return r;
}
bool timer_break_active(void) { tr(E_BREAK_ACTIVE, m_break_active, 0, 0); return m_break_active != 0; }
bool timer_any_extra_running(void) { tr(E_EXTRA_RUNNING, m_extra_running, 0, 0); return m_extra_running != 0; }
bool timer_needs_ntp_sync(time_t now) { tr(E_NEEDS_SYNC, (long long)now, P.needs_sync, 0); return P.needs_sync != 0; }
esp_err_t net_apply_try_window(void) {
    tr(E_TRY_WINDOW, 0, 0, 0);
    if (P.state_after_window >= 0) m_state = P.state_after_window;
    return ESP_OK;
}
esp_err_t nvs_config_get_break_interval_min(uint16_t *out) {
    tr(E_CFG_INT, (long long)*out, 0, 0);
    if (!P.cfg_writes) return ESP_FAIL;
    *out = (uint16_t)P.interval;
    return ESP_OK;
}
bool timer_break_due(time_t now, int32_t interval_sec) {
    int due = (m_due_from != 0 && now >= m_due_from);
    tr(E_BREAK_DUE, (long long)now, (long long)interval_sec, due);
    return due != 0;
}
bool wake_flow_maybe_start_break(time_t now) {
    tr(E_MAYBE_BREAK, (long long)now, P.break_starts, 0);
    return P.break_starts != 0;
}
void wake_flow_fire_expiry_alert(void) { tr(E_EXPIRY_ALERT, 0, 0, 0); }
bool wake_flow_poll_pause_button(void) {
    int hit = (P.pause_poll_at >= 0 && g_pause_polls == P.pause_poll_at);
    tr(E_POLL_PAUSE, g_pause_polls, hit, 0);
    g_pause_polls++;
    if (hit) m_state = TIMER_PAUSED; /* timer_pause() on device */
    return hit != 0;
}
bool wake_flow_poll_break_buttons(void) {
    int hit = (P.bpoll_at >= 0 && g_bpolls == P.bpoll_at);
    tr(E_POLL_BREAK, g_bpolls, hit, 0);
    g_bpolls++;
    return hit != 0;
}
bool wake_flow_break_end_repaint(void) { tr(E_BREAK_REPAINT, 0, 0, 0); return true; }
void status_led_show_timer_state(void) { tr(E_LED, 0, 0, 0); }
void neopixel_stop(void) { tr(E_PIXELS_OFF, 0, 0, 0); }
void neopixel_status_binary4(uint8_t value, uint8_t r, uint8_t g, uint8_t b) {
    tr(E_BINARY, value, ((long long)r << 16) | ((long long)g << 8) | b, 0);
}
void display_update(const display_state_t *st) {
    tr(E_PARTIAL, (long long)st->remaining_sec, (long long)st->wall_time, 0);
}
void display_full_refresh(const display_state_t *st) {
    tr(E_FULL_REFRESH, (long long)st->remaining_sec, (long long)st->wall_time, 0);
}
int32_t timer_tick(time_t now) { tr(E_TICK, (long long)now, 0, 0); return (int32_t)P.tick_ret; }
uint8_t buttons_take_pressed(void) { tr(E_TAKE_PRESSED, 0, 0, 0); return 0; }
int battery_read_mv(void) { tr(E_BATT, P.batt_mv, 0, 0); return P.batt_mv; }
int battery_percent_from_mv(int mv) { return mv / 40; }

/* The OLD side's make_state, modelled: the ADC read then the assembly. Two
   display_state_t fields carry the pair so the paint stubs can report
   exactly what was painted. The NEW side no longer needs this - its
   make_display_state is the real body, extracted from wake_flow.c and
   compiled in below - so the model has to stay TRACE-EQUIVALENT to it:
   same ADC read, same log line, same E_MAKE_STATE. app_state_display()
   under it is what closes the pair. */
static display_state_t make_state(int32_t remaining, time_t now) {
    int mv = battery_read_mv();
    ESP_LOGD(TAG, "battery: %d mV (%d%%)", mv, battery_percent_from_mv(mv));
    display_state_t st;
    memset(&st, 0, sizeof st);
    st.remaining_sec = remaining;
    st.wall_time = now;
    tr(E_MAKE_STATE, (long long)remaining, (long long)now, 0);
    return st;
}

/* The assembly half, for the extracted make_display_state to land on. The
   ADC read and the log line are in the extracted body itself, so this
   records only what make_state records at the same point. `in` is read so
   a seam that stopped filling it would not pass silently. */
display_state_t app_state_display(const app_state_in_t *in, int32_t remaining, time_t now) {
    display_state_t st;
    memset(&st, 0, sizeof st);
    st.remaining_sec = remaining;
    st.wall_time = now;
    (void)in->batt_mv;
    (void)in->parent_testing;
    tr(E_MAKE_STATE, (long long)remaining, (long long)now, 0);
    return st;
}
"""

DRIVER = r"""
/* ---- the driver -------------------------------------------------------- */

#define DAY_BASE 1785283200 /* 2026-07-29 00:00:00 UTC, exactly on a minute */

typedef struct { tev_t *ev; int n; int cap; } run_t;
static run_t o, n;

static void ensure(run_t *r) {
    if (r->cap == 0) { r->cap = TRACE_MAX; r->ev = malloc(sizeof(tev_t) * (size_t)r->cap); }
}
static void snap(run_t *r) {
    ensure(r);
    r->n = g_tr_n;
    memcpy(r->ev, g_tr, sizeof(tev_t) * (size_t)g_tr_n);
}

static int same(const run_t *a, const run_t *b) {
    if (a->n != b->n) return 0;
    for (int i = 0; i < a->n; i++) {
        if (a->ev[i].ev != b->ev[i].ev || a->ev[i].a != b->ev[i].a ||
            a->ev[i].b != b->ev[i].b || a->ev[i].c != b->ev[i].c) return 0;
    }
    return 1;
}

static long g_cases, g_diverge;
static char g_first[512];

static void report(const char *what) {
    g_diverge++;
    if (g_first[0]) return;
    int k = -1;
    int lim = o.n < n.n ? o.n : n.n;
    for (int i = 0; i < lim; i++)
        if (o.ev[i].ev != n.ev[i].ev || o.ev[i].a != n.ev[i].a ||
            o.ev[i].b != n.ev[i].b || o.ev[i].c != n.ev[i].c) { k = i; break; }
    if (k < 0) k = lim;
    snprintf(g_first, sizeof g_first,
             "%s: n_old=%d n_new=%d first_diff@%d old=(%d,%lld,%lld,%lld) new=(%d,%lld,%lld,%lld)",
             what, o.n, n.n, k,
             k < o.n ? o.ev[k].ev : -1, k < o.n ? o.ev[k].a : -1,
             k < o.n ? o.ev[k].b : -1, k < o.n ? o.ev[k].c : -1,
             k < n.n ? n.ev[k].ev : -1, k < n.n ? n.ev[k].a : -1,
             k < n.n ? n.ev[k].b : -1, k < n.n ? n.ev[k].c : -1);
}

static void defaults(void) {
    memset(&P, 0, sizeof P);
    P.state = TIMER_IDLE;
    P.state_after_window = -1;
    P.due_from_off = -1;
    P.pause_poll_at = -1;
    P.bpoll_at = -1;
    P.cfg_writes = 1;
    P.interval = 30;
    P.tick_ret = 777;
    P.batt_mv = 3900;
    P.clock_seed = (time_t)DAY_BASE;
}

int main(void) {
    setenv("TZ", "UTC0", 1);
    tzset();

    static const int STATES[] = {TIMER_IDLE, TIMER_RUNNING, TIMER_PAUSED, TIMER_BREAK, TIMER_EXPIRED};
    static const int SECS[] = {0, 1, 34, 35, 59};
    static const long long OFFS[] = {-5, 0, 1, 25, 26, 60, 3625};
    static const int MAXW[] = {0, 1, 24, 25, 60};
    static const int PAUSE_AT[] = {-1, 0, 5, 120};
    static const int STEPS[] = {0, 1};

    /* ---- wait_for_render_grid ---------------------------------------- */
    for (unsigned si = 0; si < sizeof STATES / sizeof STATES[0]; si++)
    for (unsigned ci = 0; ci < sizeof SECS / sizeof SECS[0]; ci++)
    for (unsigned oi = 0; oi < sizeof OFFS / sizeof OFFS[0]; oi++)
    for (unsigned mi = 0; mi < sizeof MAXW / sizeof MAXW[0]; mi++)
    for (unsigned pi = 0; pi < sizeof PAUSE_AT / sizeof PAUSE_AT[0]; pi++)
    for (unsigned ki = 0; ki < sizeof STEPS / sizeof STEPS[0]; ki++) {
        defaults();
        P.state = STATES[si];
        P.clock_seed = (time_t)DAY_BASE + SECS[ci];
        P.expiry_off = OFFS[oi];
        P.break_off = OFFS[oi];
        P.break_active = 1;
        P.pause_poll_at = PAUSE_AT[pi];
        P.clock_step = STEPS[ki];

        reset_run(); old_wait_for_render_grid(MAXW[mi]); snap(&o);
        reset_run(); new_wake_flow_wait_for_render_grid(MAXW[mi]); snap(&n);
        g_cases++;
        if (!same(&o, &n)) report("wait_for_render_grid");
    }

    /* ---- watch_break_end --------------------------------------------- */
    static const long long BREM[] = {-1, 0, 1, 2, 74, 75, 76};
    static const int BPOLL_AT[] = {-1, 0, 3, 100};
    for (int act = 0; act < 2; act++)
    for (int ext = 0; ext < 2; ext++)
    for (unsigned bi = 0; bi < sizeof BREM / sizeof BREM[0]; bi++)
    for (unsigned pi = 0; pi < sizeof BPOLL_AT / sizeof BPOLL_AT[0]; pi++)
    for (unsigned ci = 0; ci < sizeof SECS / sizeof SECS[0]; ci++)
    for (unsigned ki = 0; ki < sizeof STEPS / sizeof STEPS[0]; ki++) {
        defaults();
        P.state = TIMER_BREAK;
        P.clock_seed = (time_t)DAY_BASE + SECS[ci];
        P.break_active = act;
        P.extra_running = ext;
        P.break_off = BREM[bi];
        P.bpoll_at = BPOLL_AT[pi];
        P.clock_step = STEPS[ki];

        reset_run(); old_watch_break_end(); snap(&o);
        reset_run(); new_wake_flow_watch_break_end(); snap(&n);
        g_cases++;
        if (!same(&o, &n)) report("watch_break_end");
    }

    /* ---- watch_final_minute ------------------------------------------ */
    static const long long EXPIRY[] = {0, 1, 2, 15, 16, 60, 75, 76};
    static const int INTERVALS[] = {0, 2, 30};
    static const long long DUE_FROM[] = {-1, 0, 20};
    static const int FPAUSE_AT[] = {-1, 3, 100};
    for (unsigned ei = 0; ei < sizeof EXPIRY / sizeof EXPIRY[0]; ei++)
    for (int ns = 0; ns < 2; ns++)
    for (int saw = 0; saw < 2; saw++)
    for (unsigned ii = 0; ii < sizeof INTERVALS / sizeof INTERVALS[0]; ii++)
    for (int cw = 0; cw < 2; cw++)
    for (unsigned di = 0; di < sizeof DUE_FROM / sizeof DUE_FROM[0]; di++)
    for (unsigned pi = 0; pi < sizeof FPAUSE_AT / sizeof FPAUSE_AT[0]; pi++)
    for (int bs = 0; bs < 2; bs++)
    for (unsigned ki = 0; ki < sizeof STEPS / sizeof STEPS[0]; ki++) {
        defaults();
        P.state = TIMER_RUNNING;
        P.clock_seed = (time_t)DAY_BASE + SECS[ei % (sizeof SECS / sizeof SECS[0])];
        P.expiry_off = EXPIRY[ei];
        P.needs_sync = ns;
        P.state_after_window = saw ? TIMER_IDLE : -1;
        P.interval = INTERVALS[ii];
        P.cfg_writes = cw;
        P.due_from_off = DUE_FROM[di];
        P.pause_poll_at = FPAUSE_AT[pi];
        P.break_starts = bs;
        P.clock_step = STEPS[ki];
        /* Derived rather than another product dimension: still varied, and
           a paint that carried the wrong one would show up in the trace. */
        P.tick_ret = (long)(ei * 37) - 20;
        P.batt_mv = 3300 + (int)(ii * 300);

        reset_run(); old_watch_final_minute(); snap(&o);
        reset_run(); new_wake_flow_watch_final_minute(); snap(&n);
        g_cases++;
        if (!same(&o, &n)) report("watch_final_minute");
    }

    printf("cases=%ld divergences=%ld overflow=%ld\n", g_cases, g_diverge, g_overflow);
    if (g_first[0]) printf("first: %s\n", g_first);
    return g_diverge ? 1 : 0;
}
"""

for mname, edits in MUTANTS.items():
    nb, sm = apply_mutant(new_bodies, seams, edits)
    with open(os.path.join(OUT, f"harness_{mname}.c"), "w") as f:
        f.write(PRE)
        f.write("\n/* ---- wake_flow.c state-assembly seam (extracted verbatim) ---- */\n")
        f.write(sm)
        f.write(f"\n\n/* ---- OLD implementations ({BASE} main.c) ---- */\n")
        f.write(old_bodies)
        f.write("\n\n/* ---- NEW implementations (wake_flow.c) ---- */\n")
        f.write(nb)
        f.write("\n")
        f.write(DRIVER)
print("generated", len(MUTANTS), "harnesses")
