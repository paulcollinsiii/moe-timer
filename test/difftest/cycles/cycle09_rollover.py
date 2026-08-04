#!/usr/bin/env python3
"""Generate a differential harness: OLD (136cb06 main.c) vs NEW (wake_flow.c)
four functions, driven over a cartesian product, comparing FULL ORDERED
EFFECT TRACES.

Functions are extracted MECHANICALLY (brace matching by name) from the two
sources so no body is hand-transcribed. Only these substitutions are made,
each of them a change this refactor explicitly claims is device-identical:
  * time(NULL)      -> hal_time_now()   (hal_time.c: `return time(NULL);`)
  * symbol renaming to old_/new_ namespaces
The NEW side additionally routes through the two main.c paint seams, which
are themselves extracted mechanically from the CURRENT main.c.
"""
import re
import subprocess
import sys
import os

REPO = "/workspaces/magtag-espidf/.claude/worktrees/refactor-main-impl"
OUT = os.path.dirname(os.path.abspath(__file__))
BASE = "136cb06"


def read_git(rev, path):
    return subprocess.check_output(["git", "-C", REPO, "show", f"{rev}:{path}"], text=True)


def read_file(path):
    with open(os.path.join(REPO, path)) as f:
        return f.read()


def extract(src, name):
    """Pull a whole function definition out by name via brace matching."""
    # find the line that declares it: `... name(` at start-of-definition
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
new_main = read_file("main/main.c")

OLD_NAMES = ["maybe_start_break", "fire_expiry_alert", "queue_rollover_summary", "handle_day_rollover"]
NEW_NAMES = ["wake_flow_maybe_start_break", "wake_flow_fire_expiry_alert",
             "queue_rollover_summary", "wake_flow_handle_day_rollover"]

old_bodies = "\n\n".join(extract(old_main, n) for n in OLD_NAMES)
new_bodies = "\n\n".join(extract(new_flow, n) for n in NEW_NAMES)
seams = "\n\n".join(extract(new_main, n) for n in ["paint_break_started", "paint_current_state_full"])

# --- the only permitted rewrites -------------------------------------------
old_bodies = old_bodies.replace("time(NULL)", "hal_time_now()")
seams = seams.replace("time(NULL)", "hal_time_now()")
old_bodies = namespace(old_bodies, "old_", OLD_NAMES)
new_bodies = namespace(new_bodies, "new_", NEW_NAMES)
seams = re.sub(r"\bstatic\b\s+", "", seams)  # make_state is ours; seams stay non-static

# NEW bodies must call the harness's seam implementations, which the
# namespacing above did not touch (they are not in NEW_NAMES) - good.

MUTANTS = {
    "control": [],
    # 1. reorder two effects: persist and paint swap places
    "reorder_persist_paint": [
        ("    timer_persist_save();\n    paint_break_started(now);",
         "    paint_break_started(now);\n    timer_persist_save();")],
    # 2. move a clock sample: read the corrected clock BEFORE the window
    "move_clock_sample": [
        ("    net_apply_try_window();\n    *now = hal_time_now();",
         "    *now = hal_time_now();\n    net_apply_try_window();")],
    # 3. boundary shift +-1 on the disable check
    "boundary_interval_zero_plus1": [("if (interval_min == 0)", "if (interval_min <= 1)")],
    # 4. delete a state write
    "delete_record_date": [("    timer_record_date(*now);", "    ;")],
    # 5. boundary shift on the completions loop
    "boundary_slot_offset": [("comp[i] = timer_slot_completions(1 + i);", "comp[i] = timer_slot_completions(i);")],
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
/* Differential sweep harness - throwaway, never committed. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "alerts.h"
#include "bedtime.h"
#include "config_cache.h"
#include "display.h"
#include "hal_time.h"
#include "lock_gate.h"
#include "mqtt_ha.h"
#include "net_apply.h"
#include "nvs_config.h"
#include "nvs_defaults.h"
#include "status_led.h"
#include "time_util.h"
#include "timer.h"
#include "timer_persist.h"

/* Both sides get log macros that EVALUATE their arguments, exactly as the
   real ESP_LOG* do on device. wake_flow.c's NATIVE block discards them,
   which is why timer_run_accum() and timer_current_date() are invisible to
   the committed host suite - here they are not. */
static void diff_logv(const char *tag, const char *fmt, ...);
#define ESP_LOGI(tag, ...) diff_logv(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) diff_logv(tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) diff_logv(tag, __VA_ARGS__)
static const char *TAG = "diff";

/* ---- the ordered effect trace ------------------------------------------ */
enum {
    E_CLOCK = 1, E_CFG_INT, E_CFG_DUR, E_BREAK_DUE, E_RUN_ACCUM, E_BED_MIN,
    E_BED_ENGAGE, E_START_BREAK, E_PERSIST_SAVE, E_BATT, E_TIMER_TICK,
    E_MAKE_STATE, E_LED, E_FULL_REFRESH, E_ALERT, E_TIMESUP, E_IS_NEW_DAY,
    E_CUR_DATE, E_SCREEN_USED, E_SLOT_COMP, E_QUEUE_SUMMARY, E_BONUS_CLEAR,
    E_TRY_WINDOW, E_RESTORE, E_RESET, E_RECORD_DATE, E_LOGLINE, E_RET,
};

#define TRACE_MAX 256
typedef struct { int ev; long long a, b, c; } tev_t;
static tev_t g_tr[TRACE_MAX];
static int g_tr_n;
static void tr(int ev, long long a, long long b, long long c) {
    if (g_tr_n < TRACE_MAX) { g_tr[g_tr_n].ev = ev; g_tr[g_tr_n].a = a;
        g_tr[g_tr_n].b = b; g_tr[g_tr_n].c = c; g_tr_n++; }
}
static void diff_logv(const char *tag, const char *fmt, ...) {
    /* Evaluates the varargs (that is the point) but records only that a
       line happened - the log TEXT is not part of the device contract we
       are pinning, the CALLS inside the argument list are. */
    va_list ap; va_start(ap, fmt); (void)tag; (void)fmt; va_end(ap);
    tr(E_LOGLINE, 0, 0, 0);
}

/* ---- the fake clock: ADVANCES ON EVERY READ ---------------------------- */
static time_t g_clock;
static int g_clock_step;
time_t hal_time_now(void) {
    g_clock += g_clock_step;
    tr(E_CLOCK, (long long)g_clock, 0, 0);
    return g_clock;
}
void hal_delay_ms(uint32_t ms) { (void)ms; }

/* ---- injected model ---------------------------------------------------- */
static bool m_cfg_writes;
static uint16_t m_interval, m_duration;
static bool m_break_due;
static int m_bed_min;
static int32_t m_run_accum;
static int32_t m_tick_ret;
static int m_batt_mv;
static bool m_alert_dismissed;
static bool m_new_day;
static const char *m_date;
static int32_t m_screen_used;
static uint16_t m_comp[TIMER_SLOT_COUNT];
static bool m_restore_ok;
static time_t m_window_jump; /* extra seconds the window's sync steps by */

static jmp_buf g_bed_jmp;

esp_err_t nvs_config_get_break_interval_min(uint16_t *out) {
    tr(E_CFG_INT, (long long)*out, 0, 0);
    if (!m_cfg_writes) return ESP_FAIL;
    *out = m_interval; return ESP_OK;
}
esp_err_t nvs_config_get_break_duration_min(uint16_t *out) {
    tr(E_CFG_DUR, (long long)*out, 0, 0);
    if (!m_cfg_writes) return ESP_FAIL;
    *out = m_duration; return ESP_OK;
}
bool timer_break_due(time_t now, int32_t interval_sec) {
    tr(E_BREAK_DUE, (long long)now, (long long)interval_sec, 0);
    return m_break_due;
}
int32_t timer_run_accum(time_t now) { tr(E_RUN_ACCUM, (long long)now, 0, 0); return m_run_accum; }
int config_cache_bedtime_minutes(void) { tr(E_BED_MIN, m_bed_min, 0, 0); return m_bed_min; }
void lock_gate_bedtime_engage(time_t now, bool alert) {
    tr(E_BED_ENGAGE, (long long)now, alert ? 1 : 0, 0);
    longjmp(g_bed_jmp, 1); /* does not return, exactly as on device */
}
void timer_start_break(time_t now, int32_t dur) { tr(E_START_BREAK, (long long)now, (long long)dur, 0); }
void timer_persist_save(void) { tr(E_PERSIST_SAVE, 0, 0, 0); }
int battery_read_mv(void) { tr(E_BATT, m_batt_mv, 0, 0); return m_batt_mv; }
int battery_percent_from_mv(int mv) { return mv / 40; }
int32_t timer_tick(time_t now) { tr(E_TIMER_TICK, (long long)now, 0, 0); return m_tick_ret; }
void status_led_show_timer_state(void) { tr(E_LED, 0, 0, 0); }
void display_full_refresh(const display_state_t *st) {
    tr(E_FULL_REFRESH, (long long)st->remaining_sec, (long long)st->wall_time, 0);
}
void display_timesup(void) { tr(E_TIMESUP, 0, 0, 0); }
bool alert_run(alert_kind_t kind) { tr(E_ALERT, (int)kind, 0, 0); return m_alert_dismissed; }
bool timer_is_new_day(time_t now) { tr(E_IS_NEW_DAY, (long long)now, 0, 0); return m_new_day; }
const char *timer_current_date(void) { tr(E_CUR_DATE, (long long)(m_date[0]), 0, 0); return m_date; }
int32_t timer_screen_used_sec(time_t now) { tr(E_SCREEN_USED, (long long)now, 0, 0); return m_screen_used; }
uint16_t timer_slot_completions(int slot) {
    tr(E_SLOT_COMP, slot, 0, 0);
    if (slot < 0 || slot >= TIMER_SLOT_COUNT) return 0xFFFFu;
    return m_comp[slot];
}
void mqtt_ha_queue_summary(const char *date, int32_t used, const uint16_t comp[TIMER_EXTRA_SLOTS]) {
    long long packed = 0;
    for (int i = 0; i < TIMER_EXTRA_SLOTS; i++) packed = packed * 1000 + comp[i];
    tr(E_QUEUE_SUMMARY, (long long)date[0], (long long)used, packed);
}
void mqtt_ha_queue_bonus_clear(void) { tr(E_BONUS_CLEAR, 0, 0, 0); }
esp_err_t net_apply_try_window(void) {
    tr(E_TRY_WINDOW, 0, 0, 0);
    g_clock += m_window_jump; /* the sync steps the wall clock */
    return ESP_OK;
}
bool timer_persist_try_restore(time_t now) { tr(E_RESTORE, (long long)now, 0, 0); return m_restore_ok; }
void timer_reset(void) { tr(E_RESET, 0, 0, 0); }
void timer_record_date(time_t now) { tr(E_RECORD_DATE, (long long)now, 0, 0); }

/* main.c's make_state, modelled: the ADC read then the assembly. Two
   display_state_t fields carry the pair so display_full_refresh can report
   exactly what was painted. */
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
"""

DRIVER = r"""
/* ---- the driver -------------------------------------------------------- */

#define DAY_BASE 1785283200 /* 2026-07-29 00:00:00 UTC */

typedef struct { tev_t ev[TRACE_MAX]; int n; int ret; int bedded; } run_t;

static void snap(run_t *r, int ret, int bedded) {
    memcpy(r->ev, g_tr, sizeof g_tr);
    r->n = g_tr_n; r->ret = ret; r->bedded = bedded;
}

static int same(const run_t *a, const run_t *b) {
    if (a->n != b->n || a->ret != b->ret || a->bedded != b->bedded) return 0;
    for (int i = 0; i < a->n; i++) {
        if (a->ev[i].ev != b->ev[i].ev || a->ev[i].a != b->ev[i].a ||
            a->ev[i].b != b->ev[i].b || a->ev[i].c != b->ev[i].c) return 0;
    }
    return 1;
}

static time_t g_clock_seed;

static void reset_clock(void) { g_clock = g_clock_seed; g_tr_n = 0; }

static long g_cases, g_diverge;
static char g_first[512];

static void report(const char *what, const run_t *o, const run_t *n) {
    g_diverge++;
    if (g_first[0]) return;
    int k = -1;
    int lim = o->n < n->n ? o->n : n->n;
    for (int i = 0; i < lim; i++)
        if (o->ev[i].ev != n->ev[i].ev || o->ev[i].a != n->ev[i].a ||
            o->ev[i].b != n->ev[i].b || o->ev[i].c != n->ev[i].c) { k = i; break; }
    if (k < 0) k = lim;
    snprintf(g_first, sizeof g_first,
             "%s: n_old=%d n_new=%d ret_old=%d ret_new=%d bed_old=%d bed_new=%d first_diff@%d "
             "old=(%d,%lld,%lld,%lld) new=(%d,%lld,%lld,%lld)",
             what, o->n, n->n, o->ret, n->ret, o->bedded, n->bedded, k,
             k < o->n ? o->ev[k].ev : -1, k < o->n ? o->ev[k].a : -1,
             k < o->n ? o->ev[k].b : -1, k < o->n ? o->ev[k].c : -1,
             k < n->n ? n->ev[k].ev : -1, k < n->n ? n->ev[k].a : -1,
             k < n->n ? n->ev[k].b : -1, k < n->n ? n->ev[k].c : -1);
}

int main(void) {
    setenv("TZ", "UTC0", 1);
    tzset();

    static const uint16_t INTERVALS[] = {0, 1, 2, 29, 30, 31, 65535};
    static const uint16_t DURATIONS[] = {0, 1, 9, 10, 15, 600};
    static const int BEDS[] = {-1, 0, 1, 1199, 1200, 1201, 1439};
    static const int MINS[] = {0, 1, 719, 1189, 1190, 1199, 1200, 1439};
    static const int STEPS[] = {0, 1, 7};

    /* ---- maybe_start_break ------------------------------------------- */
    for (unsigned ii = 0; ii < sizeof INTERVALS / sizeof INTERVALS[0]; ii++)
    for (unsigned di = 0; di < sizeof DURATIONS / sizeof DURATIONS[0]; di++)
    for (int due = 0; due < 2; due++)
    for (int wr = 0; wr < 2; wr++)
    for (unsigned bi = 0; bi < sizeof BEDS / sizeof BEDS[0]; bi++)
    for (unsigned mi = 0; mi < sizeof MINS / sizeof MINS[0]; mi++)
    for (unsigned si = 0; si < sizeof STEPS / sizeof STEPS[0]; si++) {
        time_t now = (time_t)DAY_BASE + (time_t)MINS[mi] * 60;
        run_t o, n; int ret, bed;
        g_clock_step = STEPS[si];
        g_clock_seed = (time_t)DAY_BASE + 12345;
        m_interval = INTERVALS[ii]; m_duration = DURATIONS[di];
        m_break_due = due; m_cfg_writes = wr; m_bed_min = BEDS[bi];
        m_run_accum = 4242; m_tick_ret = 777; m_batt_mv = 3900;
        m_alert_dismissed = (int)(ii + di) % 2;

        reset_clock();
        bed = 0; ret = -1;
        if (setjmp(g_bed_jmp) == 0) ret = old_maybe_start_break(now) ? 1 : 0; else bed = 1;
        snap(&o, ret, bed);

        reset_clock();
        bed = 0; ret = -1;
        if (setjmp(g_bed_jmp) == 0) ret = new_wake_flow_maybe_start_break(now) ? 1 : 0; else bed = 1;
        snap(&n, ret, bed);

        g_cases++;
        if (!same(&o, &n)) report("maybe_start_break", &o, &n);
    }

    /* ---- fire_expiry_alert -------------------------------------------- */
    for (unsigned si = 0; si < sizeof STEPS / sizeof STEPS[0]; si++)
    for (int tk = 0; tk < 4; tk++)
    for (int ad = 0; ad < 2; ad++)
    for (int mv = 0; mv < 3; mv++) {
        static const int32_t TICKS[] = {-60, 0, 1, 3600};
        static const int MVS[] = {3300, 3900, 4200};
        run_t o, n;
        g_clock_step = STEPS[si];
        g_clock_seed = (time_t)DAY_BASE + 9999;
        m_tick_ret = TICKS[tk]; m_alert_dismissed = ad; m_batt_mv = MVS[mv];

        reset_clock(); old_fire_expiry_alert(); snap(&o, 0, 0);
        reset_clock(); new_wake_flow_fire_expiry_alert(); snap(&n, 0, 0);
        g_cases++;
        if (!same(&o, &n)) report("fire_expiry_alert", &o, &n);
    }

    /* ---- handle_day_rollover ------------------------------------------ */
    static const char *DATES[] = {"", "2026-07-28", "\x01"};
    static const int32_t USED[] = {0, 1234, 86400};
    static const time_t JUMPS[] = {0, 1, 3600, -60};
    for (int nd = 0; nd < 2; nd++)
    for (unsigned dd = 0; dd < sizeof DATES / sizeof DATES[0]; dd++)
    for (int ro = 0; ro < 2; ro++)
    for (unsigned uu = 0; uu < sizeof USED / sizeof USED[0]; uu++)
    for (unsigned jj = 0; jj < sizeof JUMPS / sizeof JUMPS[0]; jj++)
    for (unsigned si = 0; si < sizeof STEPS / sizeof STEPS[0]; si++)
    for (int cv = 0; cv < 3; cv++) {
        run_t o, n;
        time_t on, nn;
        g_clock_step = STEPS[si];
        g_clock_seed = (time_t)DAY_BASE + 555;
        m_new_day = nd; m_date = DATES[dd]; m_restore_ok = ro;
        m_screen_used = USED[uu]; m_window_jump = JUMPS[jj];
        for (int i = 0; i < TIMER_SLOT_COUNT; i++)
            m_comp[i] = (uint16_t)(cv * 17 + i * 3 + 1);

        on = (time_t)DAY_BASE + 305;
        reset_clock(); old_handle_day_rollover(&on); snap(&o, 0, 0);
        nn = (time_t)DAY_BASE + 305;
        reset_clock(); new_wake_flow_handle_day_rollover(&nn); snap(&n, 0, 0);
        o.ret = (int)(on % 100000); n.ret = (int)(nn % 100000); /* the out-param */
        g_cases++;
        if (!same(&o, &n)) report("handle_day_rollover", &o, &n);
    }

    printf("cases=%ld divergences=%ld\n", g_cases, g_diverge);
    if (g_first[0]) printf("first: %s\n", g_first);
    return g_diverge ? 1 : 0;
}
"""

for mname, edits in MUTANTS.items():
    nb, sm = apply_mutant(new_bodies, seams, edits)
    with open(os.path.join(OUT, f"harness_{mname}.c"), "w") as f:
        f.write(PRE)
        f.write("\n/* ---- main.c paint seams (extracted verbatim) ---- */\n")
        f.write(sm)
        f.write("\n\n/* ---- OLD implementations (136cb06 main.c) ---- */\n")
        f.write(old_bodies)
        f.write("\n\n/* ---- NEW implementations (wake_flow.c) ---- */\n")
        f.write(nb)
        f.write("\n")
        f.write(DRIVER)
print("generated", len(MUTANTS), "harnesses")
