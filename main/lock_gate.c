/* The screen locks (charge, bed time, config error, and since BUG-14 no
   clock), lifted out of main.c so their edges carry tests. What each one
   guarantees, and why they share a module, is in lock_gate.h; what lives
   here is the flow. */
#include "lock_gate.h"

#include "alerts.h"
#include "battery.h"
#include "battery_policy.h"
#include "bedtime.h"
#include "config_cache.h"
#include "config_validate.h"
#include "display.h"
#include "hal_time.h"
#include "mqtt_ha.h"
#include "net_apply.h"
#include "schedule.h"
#include "time_util.h"
#include "timer.h"
#include "timer_persist.h"

#ifndef NATIVE
#include "esp_log.h"
#else
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
#endif

static const char *TAG = "lock_gate";

/* Battery charge lock (<= 10%, released > 15%): the Charge Me! screen is
   painted on the engage wake (twice — see THE LAST WORD ON THE PANEL),
   then the device sleeps long intervals with buttons and all timer/NTP
   work disabled, and those re-wakes touch neither the panel nor the radio
   — an e-ink refresh during brownout can leave persistent artifacts, and
   every wake costs charge it can't spare. */
static RTC_DATA_ATTR bool s_charge_locked;
static bool s_charge_lock_released; /* recovery wake: repaint over Charge Me! */

/* Bed Time lock (config HHMM .. day rollover): Bed Time screen painted
   as the last thing before each locked sleep (see THE LAST WORD ON THE
   PANEL), buttons stay dark, and the device sleeps ~2 h chunks waking
   only for NTP + the rollover check. RTC-only on purpose: the gate recomputes
   from wall-clock time on every boot, so a hard reset cannot unlock the
   night - it merely replays the engage (paint + alert) once. The one
   exception is a clock that was never set (a power-on reset whose
   rollover window could not reach NTP): the gate skips it, and the
   replay waits for the first wake after NTP works (check_bedtime(),
   BUG-11). */
static RTC_DATA_ATTR bool s_bedtime_locked;
static bool s_bedtime_released; /* morning/config release: repaint over Bed Time */

/* Config-error lock (design 5.3): today's chore_free_* exceeds the
   allocation it is paired with, which cannot mean anything, so the panel
   names the pair and the device stops until someone fixes it. RTC-backed
   like the other two, and for the same reason plus one: the flag is what
   tells a locked re-wake it is a RE-wake, so it repaints rather than
   re-engaging — no second pause, no second persist, no second "engaged"
   line in the log. Unlike the other two, the sleep it schedules leaves
   Button D armed — the lock ends only when a human edits config, so it
   must not also be the thing that stops them forcing that edit in.

   RTC MEMORY SURVIVES DEEP SLEEP AND NOT esp_restart(), so an OTA reboot
   or a panic zeroes this flag and the next boot REPLAYS the engage: one
   more full refresh, one more timer_persist_save(), and a pause that is
   already a no-op on an already-paused timer. Idempotent, and the same
   replay the bed-time lock documents above. Not worth RTC_NOINIT_ATTR:
   that trades a benign replay for a flag that survives a crash it may
   have caused.

   s_config_released IS A PLAIN STATIC, deliberately and with a known
   edge: it means "this wake released the lock, so promote the next
   partial render to full". A wake that releases and then takes an
   early-out sleep before rendering loses that promotion, and Config Error
   can sit on a healthy panel until something else asks for a full
   refresh. That is the shape s_bedtime_released and s_charge_lock_released
   already have (and s_clock_released since BUG-14) — the fix is one
   mechanism for all of them, in RTC memory
   with an explicit clear, and it belongs to whoever takes that on rather
   than to the gate that noticed it. */
static RTC_DATA_ATTR bool s_config_locked;
static bool s_config_released; /* fix applied: repaint over Config Error */

/* No-clock lock (BUG-14, owner decision 2026-09-25): a power-on whose
   first NTP attempt failed has no idea what day it is, so it hands out no
   screen time at all until NTP works. "If WiFi is down, most versions of
   screen time are moot anyway." Without this the device ran a stand-in
   day dated 1970, and a battery pull while offline refunded the real one.

   RTC-backed like the others, for the config lock's reason: the flag is
   what makes a locked re-wake a RE-wake (retry, repaint, sleep) rather
   than an engage. A panic while locked zeroes it and the next boot
   replays the engage — which is only a paint, so the replay is free.

   It SHARES THE CONFIG LOCK'S SLEEP (lock_gate_sleep_mode below): the
   same CONFIG_ERR_SLEEP_SEC cadence, a window on every locked re-wake,
   and Button D armed among A-D (plus BOOT, which the config-error lock
   leaves dark: it is the way back into setup) so a press retries at once. Both locks are "waiting for something a human
   can fix, retried over the network", which is exactly what that sleep
   was sized for. It holds for as long as NTP fails, with no give-up:
   WiFi that works with NTP blocked keeps it locked indefinitely (owner
   decision Q3, 2026-09-25). */
static RTC_DATA_ATTR bool s_clock_locked;
static bool s_clock_released; /* clock set: repaint over "No Clock" */

wake_sleep_mode_t lock_gate_sleep_mode(void) {
    return wake_sleep_mode_select(s_charge_locked, s_bedtime_locked, s_config_locked || s_clock_locked);
}

bool lock_gate_clock_locked(void) {
    return s_clock_locked;
}

bool lock_gate_bedtime_in_force(time_t now) {
    /* The same two questions check_bedtime() asks, answered without any of
       its effects. An unset clock gets no bed-time decision (BUG-11), so it
       is never in force here either. */
    return time_util_clock_plausible(now) &&
           bedtime_active(time_util_minutes_of_day(now), config_cache_bedtime_minutes());
}

bool lock_gate_charge_locked(void) {
    return s_charge_locked;
}

bool lock_gate_config_locked(void) {
    return s_config_locked;
}

wake_render_t lock_gate_promote_render(wake_render_t wr) {
    if ((s_charge_lock_released || s_bedtime_released || s_config_released || s_clock_released) &&
        wr == WAKE_RENDER_PARTIAL) {
        return WAKE_RENDER_FULL; /* the panel still shows a lock screen — repaint fully */
    }
    return wr;
}

/* ---- THE LAST WORD ON THE PANEL -----------------------------------------

   EVERY LOCKED PATH REPAINTS ITS OWN SCREEN IMMEDIATELY BEFORE THE SLEEP,
   unconditionally. e-ink retains, so this reads like a wasted refresh, and
   the three comments that used to say so were wrong for one reason.

   THE NETWORK WINDOW IS NOT A PASSIVE READER. Each lock runs
   net_apply_try_window() with its flag already set, and that window's
   reconcile can fire on_active_expired_alert() (net_apply.c), whose own
   comment says the hook "owns the display: TIME'S UP + alert + repaint".
   The repaint is paint_current_state_full(), which asks
   display_screen_for() for whatever screen the app state deserves — the
   chore checklist, among others. Every one of these gates PAUSES a running
   timer as it engages, and PAUSED is exactly the state whose reconcile can
   cross zero and return TIMER_RECONCILE_EXPIRED (timer.c). So each gate
   arms the thing that paints over it, and then sleeps on top of the
   result. It needs a parent editing HA config during the window, which on
   the config lock is not a corner case but the expected behaviour.

   IT IS WORST UNDER THE CONFIG LOCK IN KIND: that sleep narrows the EXT1
   mask to D alone (buttons_policy.c), so the panel would be offering
   "Chores" above a button that is not a wake source at all. It is worst
   under the CHARGE lock in consequence: that sleep arms nothing and runs
   CHARGE_LOCK_SLEEP_SEC, so no press can clear what is left on the glass —
   only the battery recovering, or the interval expiring, will.

   WHY UNCONDITIONALLY, rather than only when something actually painted.
   Any conditional needs a "did anything paint" fact, and every source of
   one is a contract with a module that does not own this lock: a return
   code net_apply would have to be told to set, or a paint counter behind
   display.c's single render() chokepoint. The second is genuinely exact
   today — but display.c is in no host suite, so nothing would notice the
   counter going quiet, and both are the same enumerated-paths argument
   this file has already had to retract twice. The unconditional repaint is
   a property of THIS module: whatever painted, and whenever it was added,
   the lock's own screen is what the panel is left holding.

   WHAT IT COSTS, measured rather than assumed: one full refresh, ~3-4 s
   (ssd1680.c), on a wake that has just spent up to NET_JOIN_TIMEOUT_MS
   (90 s, net_window.h) of radio plus an MQTT round — order 1% of the wake
   it rides on. Every wake that pays it is an error state or a night, never
   the steady state.

   IT ALSO FIXES A SECOND THING on the config gate: the repaint names the
   pair the POST-window re-check read, so a day rollover into a
   different-but-still-broken day type stops leaving yesterday's numbers on
   the glass.

   THE CHARGE LOCK REPAINTS INSIDE ITS ENGAGE BLOCK rather than before its
   sleep, and that is the one deliberate difference. It is the only lock
   that opens no window on a locked re-wake (sleep_plan.h), so on that path
   nothing can have painted and there is nothing to undo — and a refresh
   per 600 s wake on a flat battery is the artifact risk the lock exists to
   avoid. Once per lock episode is the whole cost here. */

/* ---- battery charge lock ---------------------------------------------- */

void lock_gate_check_charge(void) {
    int pct = battery_percent_from_mv(battery_read_mv());
    batt_policy_t pol = battery_policy_evaluate(pct, s_charge_locked);
    if (pol != BATT_LOCK) {
        if (s_charge_locked) {
            s_charge_locked = false;
            s_charge_lock_released = true; /* repaint over the Charge Me! screen */
            ESP_LOGW(TAG, "Charge lock released (%d%%)", pct);
        }
        return;
    }
    if (!s_charge_locked) {
        s_charge_locked = true;
        ESP_LOGW(TAG, "Charge lock engaged (%d%%)", pct);
        if (timer_get_state() == TIMER_RUNNING) {
            timer_pause(hal_time_now());
        }
        /* Painted BEFORE the window, not after: the screen has to be up
           before 90 s of radio on a pack that may not survive it. */
        display_charge_me();
        /* Best-effort HA notification (charge_lock: true) — the last stat
           before the long battery-recheck sleeps begin. */
        net_apply_try_window();
        display_charge_me(); /* THE LAST WORD ON THE PANEL — see above */
    }
    enter_deep_sleep(lock_gate_sleep_mode()); /* charge-locked here: 600 s, no button wake */
}

/* ---- no clock (BUG-14) -------------------------------------------------- */

/* Put a real day in RAM on the wake the clock first reads plausible.
   The stand-in day an unset clock dated ("1970-01-01") is never usable:
   today's snapshot is restored over it, or, with none for today, the day
   starts fresh — the same restore-else-reset pair the rollover ends with.
   A no-op when RAM already holds a real day, which is the ordinary case:
   a sync that lands in the rollover's own window has settled the day
   before this gate runs. It matters for a sync that lands in THIS gate's
   window, after the rollover has already been and gone.

   THE RESET BRANCH QUEUES THE BONUS CLEAR, as the day rollover does
   before its window: a fresh day must not inherit the retained HA bonus
   target of the day the power went on. The rollover that ran on the
   power-on queued one too, but that flag is plain RAM and its window
   failed, so it is gone. The restore branch queues none: today's
   snapshot carries today's bonus_applied, which the retained target
   still matches.

   Returns true when it put a day in RAM (either branch), false for the
   no-op. */
static bool settle_day(time_t now) {
    if (time_util_day_plausible(timer_current_date())) {
        return false;
    }
    if (!timer_persist_try_restore(now)) {
        timer_reset();
        timer_record_date(now);
        mqtt_ha_queue_bonus_clear();
    }
    return true;
}

/* A DAY SETTLED OUTSIDE A WINDOW'S AFTER-NTP HOOK OWES ONE WINDOW (owner
   decision Q-B, 2026-09-25). check_clock's two releases outside the hook
   (the late sync, and the clock set between wakes) settle the day after
   any window of this wake has posted its stats. So the reset branch's
   bonus clear, which is plain RAM (mqtt_ha.c), and the grants and target
   the locked windows held have no window to ride. On a D-press wake
   nothing else opens one, and the clear would die at sleep; the next
   window would then re-grant the old day's retained target on the fresh
   day (the cycle-2 review, MINOR-1).

   Paid at the END of lock_gate_check_bedtime(), not in check_clock(), so
   it is never a second window: the bed-time engage (which does not
   return) and the config gate open a window of their own on exactly the
   wakes they act, and lock_window() below marks the debt paid by either.
   Nor is it a second window on a tick wake: the paid window records its
   sync (net_window.c), so the tick's sync block
   (wake_policy_sync_due) no longer finds one due. Plain RAM, because it
   is paid in the same call that raises it. */
static bool s_settle_window_owed;

static void lock_window(void) {
    s_settle_window_owed = false; /* any window from here on carries it */
    net_apply_try_window();
}

static bool release_clock(time_t now) {
    s_clock_locked = false;
    s_clock_released = true;
    return settle_day(now);
}

/* The retry window's after-NTP hook (net_apply_try_window_then): runs
   after the sync settles and before the stats snapshot is posted, which
   is before the window's MQTT phase does anything. A release HERE settles
   the real day first, so the HA effects that phase buffers (a cmd grant,
   a bonus target) are acked for, and applied by the finish to, the day
   they belong to, and the stat it publishes is that day's rather than
   NO_CLOCK. Released any later, the finish would put them on the 1970
   stand-in and settle_day's reset would then wipe them, after HA had
   been told they landed (the BUG-14 round-2 review, MAJOR-1). While the
   clock stays unset the hook does nothing: the snapshot then says
   no_clock, and mqtt_ha leaves those commands retained (stats_json.h). */
static void clock_after_ntp(void) {
    const time_t t = hal_time_now();
    if (time_util_clock_plausible(t)) {
        (void)release_clock(t); /* settled in time for this window: nothing owed */
        /* The window recorded this sync, and settle_day's reset (either
           branch: try_restore clears the stand-in first) wiped it. Put it
           back, or the regular sync block later in this wake opens a
           second window to learn what this one just did. */
        timer_record_ntp_sync(t);
    }
}

/* Returns true when THIS call released the lock (lock_gate.h). Does not
   return while the clock is still unset.

   THE ENGAGE WAKE OPENS NO WINDOW OF ITS OWN. The only way to reach it is
   a power-on (or a panic while already locked, which replays it), and
   either way the RTC day is empty, so the day rollover has just run and
   its window has just failed to reach NTP. A second attempt seconds later
   would cost another full association timeout for the same answer, so
   the engage paints and sleeps. Every locked RE-wake then carries exactly
   one window: nothing else runs on it, and it is the retry. There is no
   give-up: it retries every CONFIG_ERR_SLEEP_SEC, or on D, for as long
   as NTP fails (owner decision Q3).

   Nothing is paused or saved: the day in RAM is the stand-in the rollover
   has just reset, IDLE, and timer_persist_save() refuses it anyway. */
static bool check_clock(time_t *now) {
    if (time_util_clock_plausible(*now)) {
        if (!s_clock_locked) {
            return false;
        }
        /* Set between wakes — an async sync that landed after the last
           window's checks. No window has run on a settled day, so none has
           consumed a day-scoped command, and the day rollover stood aside
           for this gate (wake_flow_handle_day_rollover): settle_day() here
           does the restore-or-reset, and on a reset queues the bonus
           clear. The window that carries it, and the held commands, is
           owed (s_settle_window_owed above). */
        s_settle_window_owed = release_clock(*now);
        ESP_LOGW(TAG, "No-clock lock released");
        return true;
    }
    if (!s_clock_locked) {
        s_clock_locked = true;
        ESP_LOGW(TAG, "No-clock lock engaged: NTP failed after a power-on");
        display_no_clock();
        enter_deep_sleep(lock_gate_sleep_mode()); /* config-lock sleep: 30 min, D and BOOT armed */
        return false;                             /* unreachable on device */
    }
    /* Locked re-wake, on the cadence or on a D press: the retry. */
    net_apply_try_window_then(clock_after_ntp);
    *now = hal_time_now();
    if (!s_clock_locked) {
        ESP_LOGW(TAG, "No-clock lock released (NTP in the lock's window)");
        return true;
    }
    if (time_util_clock_plausible(*now)) {
        /* NTP settled after the hook looked (the wait timed out, the
           sync landed during the MQTT tail). That window's snapshot said
           no_clock, so it held every day-scoped command: nothing was put
           on the stand-in, and releasing now loses nothing. What this
           window's MQTT phase can no longer carry (the reset's bonus
           clear, the held commands) rides the owed window
           (s_settle_window_owed above). The sync is deliberately NOT
           re-recorded here: the owed window records its own, and if that
           one's NTP fails, a tick wake's sync block still finds a sync
           due and tries again. A timed-out sync that still lands is rare. */
        s_settle_window_owed = release_clock(*now);
        ESP_LOGW(TAG, "No-clock lock released (late NTP in the lock's window)");
        return true;
    }
    display_no_clock(); /* THE LAST WORD ON THE PANEL — see above */
    enter_deep_sleep(lock_gate_sleep_mode());
    return false; /* unreachable on device: enter_deep_sleep does not return */
}

/* ---- bed time ----------------------------------------------------------- */

void lock_gate_bedtime_engage(time_t now, bool alert) {
    s_bedtime_locked = true;
    /* Read once, into a local, BEFORE the log rather than inside its
       argument list: ESP_LOGx wraps its arguments in a compile-time level
       guard, so an inline call stops happening entirely at a level where
       the statement is compiled out (HAZ-1). The local also collapses what
       were two reads of the same state into one. */
    const timer_state_t st = timer_get_state();
    ESP_LOGW(TAG, "Bed time engaged (state %d%s)", (int)st, alert ? ", alerting" : "");
    if (st == TIMER_RUNNING) {
        timer_pause(now);
    }
    timer_persist_save();
    display_bedtime(); /* one full refresh; later wakes leave the panel alone */
    if (alert) {
        alert_run(ALERT_BEDTIME);
    }
    /* Best-effort HA stat before the long no-button sleeps begin. */
    lock_window();
    display_bedtime();                        /* THE LAST WORD ON THE PANEL — see above */
    enter_deep_sleep(lock_gate_sleep_mode()); /* bed-time locked here: ~2 h, no button wake */
}

/* `now` IN AND OUT, the same shape wake_flow_handle_day_rollover() uses,
   and for the same reason: the release-by-edit path below returns from
   behind a network window that can carry an NTP step, and the caller's
   instant is stale from there on. The config gate that runs next asks
   "what day is it today", so it has to get the moved clock and not the
   one this wake started with.

   Returns true when THIS call released the lock — the answer
   lock_gate_check_bedtime() hands its caller (see lock_gate.h). */
static bool check_bedtime(time_t *now) {
    /* A CLOCK THAT WAS NEVER SET GETS NO BED-TIME DECISION AT ALL (BUG-11).
       After a genuine power-on reset the clock reads the 1970 epoch plus
       uptime until NTP lands. The power-on's day-rollover window tries NTP
       before this gate runs, so the clock is normally set by now. The
       skip fires only when that attempt failed (no WiFi), and then on
       every wake until NTP works. Judging an unset clock against the
       window gives a wrong answer either way: a short sleep where bed
       time should have held, or, in a time zone that puts near-epoch
       local time inside the window, a wrong two-hour lock.

       So the check is skipped, and skipping is neutral. Nothing engages,
       nothing is released, s_bedtime_locked is left as it stands, and the
       answer is false because no lock let go. The config gate still runs,
       and the first wake after NTP works decides normally. A device that
       lost power and has no WiFi therefore does not enter bed time until
       it syncs.

       Holding the flag is safe only because nothing can raise it on an
       unset clock. The power-on zeroed it (RTC_DATA_ATTR), and the one
       other path that raises it, the break planner's bed-time crossing in
       wake_flow_maybe_start_break(), is guarded by the same check. Were
       that guard lost, a flag raised offline would stand, buttons dead,
       until NTP worked, because this gate is the only code that clears it.

       This asks whether the clock was EVER set (time_util.h), not whether
       NTP set it this session. The OTA gate asks the second question, and
       asking it here would skip bed time on most wakes. Both wake handlers
       reach this through lock_gate_check_bedtime(), so one guard covers
       both.

       SINCE BUG-14 THE NO-CLOCK GATE RUNS FIRST and does not return on an
       unset clock, so through lock_gate_check_bedtime() this skip is no
       longer reached. It stays as the belt: it costs one comparison, and
       it keeps this function correct on its own terms if the order in
       lock_gate_check_bedtime() is ever changed. */
    if (!time_util_clock_plausible(*now)) {
        ESP_LOGI(TAG, "Bed time not evaluated: clock not set");
        return false;
    }
    if (!bedtime_active(time_util_minutes_of_day(*now), config_cache_bedtime_minutes())) {
        if (s_bedtime_locked) {
            s_bedtime_locked = false;
            s_bedtime_released = true; /* repaint over the Bed Time screen */
            ESP_LOGW(TAG, "Bed time released");
            return true;
        }
        return false;
    }
    if (!s_bedtime_locked) {
        lock_gate_bedtime_engage(*now, bedtime_should_alert(timer_get_state(), timer_break_active())); /* no return */
    }
    /* Locked re-wake (~2 h cadence): NTP + HA config pickup. Re-check
       after the window - a bedtime edit landing here is the only remote
       fix path while buttons are dead, and it must not wait another 2 h. */
    lock_window(); /* finish drops the bedtime cache with the rest */
    *now = hal_time_now();
    if (!bedtime_active(time_util_minutes_of_day(*now), config_cache_bedtime_minutes())) {
        s_bedtime_locked = false;
        s_bedtime_released = true;
        ESP_LOGW(TAG, "Bed time released (config edit or clock step)");
        /* Fall through to the normal wake, which repaints; the true is
           what tells it the presses made so far were the lock's. */
        return true;
    }
    display_bedtime(); /* THE LAST WORD ON THE PANEL — see above */
    enter_deep_sleep(lock_gate_sleep_mode());
    return false; /* unreachable on device: enter_deep_sleep does not return */
}

/* ---- config error ------------------------------------------------------- */

/* Today's stored pair, and whether it can mean anything.

   THE RAW MINUTE PAIR, not schedule_get_chore_free_sec(). That accessor
   CLAMPS the free slice to the allocation, which is exactly right for
   every consumer that does arithmetic with it and fatal here: after the
   clamp no pair fails the rule, so a gate built on it would find every
   device healthy for ever and this screen would never paint. Minutes also
   keeps the predicate honest — it takes two uint16_t, and a seconds value
   truncates silently and can invert the answer (config_validate.h).

   TODAY's day type, resolved from the instant passed in, and resolved
   AGAIN after the network window rather than cached: rows C11/C12 are the
   whole reason this gate exists, and a device whose only good day is
   tomorrow must be able to reach tomorrow. */
static bool config_pair_ok(time_t now, day_type_t *day_type, uint16_t *free_min, uint16_t *alloc_min) {
    *day_type = schedule_get_day_type(now);
    schedule_get_chore_free_pair_min(*day_type, free_min, alloc_min);
    return config_is_valid_chore_free_min(*free_min, *alloc_min);
}

/* Returns true when THIS call released the lock, on either path — see
   lock_gate_check_bedtime() in lock_gate.h for what the caller does with
   it. */
static bool check_config_error(time_t now) {
    day_type_t day_type = DAY_WEEKDAY;
    uint16_t free_min = 0;
    uint16_t alloc_min = 0;

    if (config_pair_ok(now, &day_type, &free_min, &alloc_min)) {
        if (s_config_locked) {
            s_config_locked = false;
            s_config_released = true; /* repaint over the Config Error screen */
            ESP_LOGW(TAG, "Config error released");
            return true;
        }
        return false;
    }

    if (!s_config_locked) {
        s_config_locked = true;
        /* Read once into a local before the log, for the same reason the
           bed-time engage does it: ESP_LOGx wraps its arguments in a
           compile-time level guard, so an inline call stops happening at
           a level where the statement is compiled out. */
        const timer_state_t st = timer_get_state();
        ESP_LOGW(TAG, "Config error engaged (day %d, chore_free %u > %u min, state %d)", (int)day_type,
                 (unsigned)free_min, (unsigned)alloc_min, (int)st);
        if (st == TIMER_RUNNING) {
            /* Paused, never expired — the same choice the bed-time lock
               makes, and here there is a second reason: the allocation
               this timer is burning was computed against a chore gate
               whose numbers are the thing under dispute. */
            timer_pause(now);
        }
        timer_persist_save();
        /* Painted BEFORE the window for the same reason the charge gate
           does it: the parent should be reading the screen while the radio
           is still trying, not after it gives up. */
        display_config_error(day_type, free_min, alloc_min);
    }

    /* NO ARM HERE FOR "ANOTHER LOCK PAINTED OVER US", and its removal is
       the point rather than a tidy-up. What stood here was an
       `else if (s_charge_lock_released || s_bedtime_released)` repaint,
       because both of the other gates run EARLIER in this same wake —
       charge in app_main's boot phase, bed time a few lines above — and
       each paints as it engages, so a lock that went up over the top of
       this one and has just let go leaves Charge Me! or Bed Time on a
       panel whose device is still config-locked. That case is real and
       the two tests that name it still run. What was wrong with the arm
       is that it ENUMERATED the two painters it knew about, and the
       window below is a third it did not: it can repaint through
       on_active_expired_alert() (see THE LAST WORD ON THE PANEL above).
       The pre-sleep repaint covers all of them, names none of them, and
       reads the pair the post-window re-check read — which the arm could
       not, running as it did before the window. */

    /* One window, on the engage wake and on every locked re-wake alike.
       It is the only remote fix path, and the re-check after it is what
       keeps a corrected config from waiting out a whole interval behind a
       panel that has already been told it is wrong. Button D exists to
       reach this line early — and that is ALL a D press on a locked device
       does: the true returned on a release below is what stops the wake
       handler also running D's own action (✓3 or the sync) on top. */
    lock_window(); /* finish drops the schedule cache with the rest */
    if (config_pair_ok(hal_time_now(), &day_type, &free_min, &alloc_min)) {
        s_config_locked = false;
        s_config_released = true;
        ESP_LOGW(TAG, "Config error released (config edit, clock step or day change)");
        /* Fall through to the normal wake, which repaints; the true is
           what tells it the presses made so far were the lock's. */
        return true;
    }
    /* THE LAST WORD ON THE PANEL — see above. A repaint, NOT a re-engage:
       the timer was paused and persisted when the lock first went up, and
       the pair named is the one the re-check just read. */
    display_config_error(day_type, free_min, alloc_min);
    enter_deep_sleep(lock_gate_sleep_mode());
    return false; /* unreachable on device: enter_deep_sleep does not return */
}

/* Same precedence as the gates themselves: charge in app_main, then the
   no-clock gate, bed time, and config error last. The first one standing is
   the screen the device would have slept on. */
bool lock_gate_repaint_standing_lock(void) {
    if (s_charge_locked) {
        display_charge_me();
    } else if (s_clock_locked) {
        display_no_clock();
    } else if (s_bedtime_locked) {
        display_bedtime();
    } else if (s_config_locked) {
        day_type_t day_type = DAY_WEEKDAY;
        uint16_t free_min = 0;
        uint16_t alloc_min = 0;
        (void)config_pair_ok(hal_time_now(), &day_type, &free_min, &alloc_min);
        display_config_error(day_type, free_min, alloc_min);
    } else {
        return false;
    }
    return true;
}

bool lock_gate_check_bedtime(time_t now) {
    /* Each may not return; the first two may advance `now` past an NTP
       step. The no-clock gate is FIRST because the other two read the
       clock: past it, `now` is plausible, so bed time and the day type
       are judged against a real time. All three run whatever the earlier
       ones answered: a bed-time release is exactly the morning the config
       gate picks the panel back up, and a clock release is the wake bed
       time and config are first judged at all. */
    s_settle_window_owed = false;
    const bool clock_released = check_clock(&now);
    const bool bedtime_released = check_bedtime(&now);
    const bool config_released = check_config_error(now);
    if (s_settle_window_owed) {
        /* A day the clock gate settled outside a window, and neither gate
           after it opened one: pay it now (s_settle_window_owed above). */
        ESP_LOGI(TAG, "Window for the day the no-clock release settled");
        lock_window();
    }
    return clock_released || bedtime_released || config_released;
}
