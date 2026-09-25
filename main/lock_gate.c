/* The three screen locks, lifted out of main.c so their edges carry
   tests. What each one guarantees, and why they share a module, is in
   lock_gate.h; what lives here is the flow. */
#include "lock_gate.h"

#include "alerts.h"
#include "battery.h"
#include "battery_policy.h"
#include "bedtime.h"
#include "config_cache.h"
#include "config_validate.h"
#include "display.h"
#include "hal_time.h"
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
   already have — the fix is one mechanism for all three, in RTC memory
   with an explicit clear, and it belongs to whoever takes that on rather
   than to the gate that noticed it. */
static RTC_DATA_ATTR bool s_config_locked;
static bool s_config_released; /* fix applied: repaint over Config Error */

wake_sleep_mode_t lock_gate_sleep_mode(void) {
    return wake_sleep_mode_select(s_charge_locked, s_bedtime_locked, s_config_locked);
}

bool lock_gate_charge_locked(void) {
    return s_charge_locked;
}

bool lock_gate_config_locked(void) {
    return s_config_locked;
}

wake_render_t lock_gate_promote_render(wake_render_t wr) {
    if ((s_charge_lock_released || s_bedtime_released || s_config_released) && wr == WAKE_RENDER_PARTIAL) {
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
    net_apply_try_window();
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
       both. */
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
    net_apply_try_window(); /* finish drops the bedtime cache with the rest */
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
    net_apply_try_window(); /* finish drops the schedule cache with the rest */
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

bool lock_gate_check_bedtime(time_t now) {
    /* Each may not return; the first may advance `now` past an NTP step.
       Both run whatever the first answered: a bed-time release is exactly
       the morning the config gate picks the panel back up. */
    const bool bedtime_released = check_bedtime(&now);
    const bool config_released = check_config_error(now);
    return bedtime_released || config_released;
}
