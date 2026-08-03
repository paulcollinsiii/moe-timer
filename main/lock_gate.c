/* The two screen locks, lifted out of main.c so their edges carry tests.
   What each one guarantees, and why they share a module, is in
   lock_gate.h; what lives here is the flow. */
#include "lock_gate.h"

#include "alerts.h"
#include "battery.h"
#include "battery_policy.h"
#include "bedtime.h"
#include "config_cache.h"
#include "display.h"
#include "hal_time.h"
#include "net_apply.h"
#include "time_util.h"
#include "timer.h"
#include "timer_persist.h"

#ifndef NATIVE
#include "esp_log.h"
#else
#define ESP_LOGW(tag, ...) ((void)(tag))
#endif

static const char *TAG = "lock_gate";

/* Battery charge lock (<= 10%, released > 15%): the Charge Me! screen is
   painted once, then the device sleeps long intervals with buttons and
   all timer/NTP work disabled — an e-ink refresh during brownout can
   leave persistent artifacts, and every wake costs charge it can't spare. */
static RTC_DATA_ATTR bool s_charge_locked;
static bool s_charge_lock_released; /* recovery wake: repaint over Charge Me! */

/* Bed Time lock (config HHMM .. day rollover): Bed Time screen painted
   once, buttons stay dark, and the device sleeps ~2 h chunks waking only
   for NTP + the rollover check. RTC-only on purpose: the gate recomputes
   from wall-clock time on every boot, so a hard reset cannot unlock the
   night - it merely replays the engage (paint + alert) once. */
static RTC_DATA_ATTR bool s_bedtime_locked;
static bool s_bedtime_released; /* morning/config release: repaint over Bed Time */

wake_sleep_mode_t lock_gate_sleep_mode(void) {
    return wake_sleep_mode_select(s_charge_locked, s_bedtime_locked);
}

bool lock_gate_charge_locked(void) {
    return s_charge_locked;
}

wake_render_t lock_gate_promote_render(wake_render_t wr) {
    if ((s_charge_lock_released || s_bedtime_released) && wr == WAKE_RENDER_PARTIAL) {
        return WAKE_RENDER_FULL; /* the panel still shows a lock screen — repaint fully */
    }
    return wr;
}

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
        display_charge_me(); /* one full refresh; later wakes leave the panel alone */
        /* Best-effort HA notification (charge_lock: true) — the last stat
           before the long battery-recheck sleeps begin. */
        net_apply_try_window();
    }
    enter_deep_sleep(lock_gate_sleep_mode()); /* charge-locked here: 600 s, no button wake */
}

/* ---- bed time ----------------------------------------------------------- */

void lock_gate_bedtime_engage(time_t now, bool alert) {
    s_bedtime_locked = true;
    ESP_LOGW(TAG, "Bed time engaged (state %d%s)", (int)timer_get_state(), alert ? ", alerting" : "");
    if (timer_get_state() == TIMER_RUNNING) {
        timer_pause(now);
    }
    timer_persist_save();
    display_bedtime(); /* one full refresh; later wakes leave the panel alone */
    if (alert) {
        alert_run(ALERT_BEDTIME);
    }
    /* Best-effort HA stat before the long no-button sleeps begin. */
    net_apply_try_window();
    enter_deep_sleep(lock_gate_sleep_mode()); /* bed-time locked here: ~2 h, no button wake */
}

void lock_gate_check_bedtime(time_t now) {
    if (!bedtime_active(time_util_minutes_of_day(now), config_cache_bedtime_minutes())) {
        if (s_bedtime_locked) {
            s_bedtime_locked = false;
            s_bedtime_released = true; /* repaint over the Bed Time screen */
            ESP_LOGW(TAG, "Bed time released");
        }
        return;
    }
    if (!s_bedtime_locked) {
        lock_gate_bedtime_engage(now, bedtime_should_alert(timer_get_state(), timer_break_active())); /* no return */
    }
    /* Locked re-wake (~2 h cadence): NTP + HA config pickup only, no
       repaint (e-ink retains). Re-check after the window - a bedtime
       edit landing here is the only remote fix path while buttons are
       dead, and it must not wait another 2 h. */
    net_apply_try_window(); /* finish drops the bedtime cache with the rest */
    if (!bedtime_active(time_util_minutes_of_day(hal_time_now()), config_cache_bedtime_minutes())) {
        s_bedtime_locked = false;
        s_bedtime_released = true;
        ESP_LOGW(TAG, "Bed time released (config edit or clock step)");
        return; /* fall through to the normal wake, which repaints */
    }
    enter_deep_sleep(lock_gate_sleep_mode());
}
