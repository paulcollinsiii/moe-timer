#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "alerts.h"
#include "app_state.h"
#include "audio.h"
#include "battery.h"
#include "battery_policy.h"
#include "bedtime.h"
#include "button_actions.h"
#include "button_latch.h"
#include "buttons.h"
#include "display.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal_nvs.h"
#include "light.h"
#include "mqtt_ha.h"
#include "neopixel.h"
#include "net_apply.h"
#include "net_window.h"
#include "nvs_config.h"
#include "nvs_defaults.h"
#include "nvs_flash.h"
#include "quiet_hours.h"
#include "schedule.h"
#include "sleep_plan.h"
#include "stats_json.h"
#include "timer.h"
#include "wake_policy.h"

static const char *TAG = "main";

/* Timezone default lives in nvs_defaults.h (NVS_DEFAULT_TZ); the active TZ
   comes from NVS at boot so HA can change it (ProductOverview section 1). */
/* Kconfig bool as a C expression (defined as 1 when =y, absent when =n) */
#if CONFIG_MAGTAG_PARENT_TESTING
#define PARENT_TESTING true
#else
#define PARENT_TESTING false
#endif
/* IDLE shows only the clock — sync on the menuconfig cadence (default
   hourly) instead of every 10 min. The S2 has no crystal-backed RTC; its
   RC-oscillator timekeeping can drift minutes/day, so don't set this too
   long. */
#define IDLE_SYNC_INTERVAL_SEC (CONFIG_MAGTAG_IDLE_SYNC_INTERVAL_MIN * 60)

/* Status pixels stay dark during configured quiet hours (alert pulses are
   exempt — they accompany an audible, dismissable alarm). The window is
   read from NVS once per wake — this callback fires from the LED task on
   every pixel update; invalidated after a network window applies edits. */
static bool s_quiet_cfg_loaded;
static uint16_t s_quiet_start_cfg;
static uint16_t s_quiet_end_cfg;

/* Bed time follows the same wake-scoped cache pattern; both caches are
   dropped by config_caches_invalidate() after a network window so an HA
   edit applies within the same wake. An invalid stored value falls back
   to the compile-time default rather than daytime-locking the device. */
static bool s_bedtime_cfg_loaded;
static uint16_t s_bedtime_cfg;

static bool status_leds_quiet(void) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (!s_quiet_cfg_loaded) {
        s_quiet_start_cfg = NVS_DEFAULT_QUIET_START;
        s_quiet_end_cfg = NVS_DEFAULT_QUIET_END;
        nvs_config_get_quiet_start(&s_quiet_start_cfg);
        nvs_config_get_quiet_end(&s_quiet_end_cfg);
        s_quiet_cfg_loaded = true;
    }
    return quiet_hours_active(tm.tm_hour * 60 + tm.tm_min, quiet_hhmm_to_minutes(s_quiet_start_cfg),
                              quiet_hhmm_to_minutes(s_quiet_end_cfg));
}

/* Held-through-sleep guard: EXT1 ANY_LOW is level-triggered, so a button
   still held when the release-wait in enter_deep_sleep() times out (3 s)
   re-wakes the chip instantly and would re-fire its action. Record what
   was held at sleep entry; an immediate re-wake by one of those buttons
   is a continuation to ignore, not a new press. */
static RTC_DATA_ATTR uint8_t s_held_mask_at_sleep;
static RTC_DATA_ATTR int64_t s_sleep_entry_time;

/* Battery charge lock (<= 10%, released > 15%): the Charge Me! screen is
   painted once, then the device sleeps long intervals with buttons and
   all timer/NTP work disabled — an e-ink refresh during brownout can
   leave persistent artifacts, and every wake costs charge it can't spare. */
static RTC_DATA_ATTR bool s_charge_locked;
static bool s_charge_lock_released; /* recovery wake: repaint over Charge Me! */
#define CHARGE_LOCK_SLEEP_SEC 600

/* Bed Time lock (config HHMM .. day rollover): Bed Time screen painted
   once, buttons stay dark, and the device sleeps ~2 h chunks waking only
   for NTP + the rollover check. RTC-only on purpose: the gate recomputes
   from wall-clock time on every boot, so a hard reset cannot unlock the
   night - it merely replays the engage (paint + alert) once. */
static RTC_DATA_ATTR bool s_bedtime_locked;
static bool s_bedtime_released; /* morning/config release: repaint over Bed Time */
#define BEDTIME_SLEEP_SEC 7200

/* Persist the timer to NVS so a panic/reset (which wipes RTC memory)
   cannot refund the day's allocation. Write only on change — snapshot
   fields are stable across routine RUNNING ticks, so this costs flash
   wear only on actual state transitions. */
static void save_timer_snapshot(void) {
    timer_snapshot_t snap, stored;
    timer_make_snapshot(&snap);
    if (nvs_config_load_timer_snapshot(&stored) == ESP_OK && memcmp(&snap, &stored, sizeof(snap)) == 0) {
        return;
    }
    esp_err_t ret = nvs_config_save_timer_snapshot(&snap);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "snapshot save failed: %s", esp_err_to_name(ret));
    }
}

/* After a panic/external reset OR power cycle, RTC memory is wiped —
   restore today's timer state from NVS instead of letting the rollover
   refund the allocation. Returns true when state was restored. Called
   twice: at boot (works after a panic, where the RTC clock survives) and
   again after the rollover's NTP sync (covers power-on, where the clock
   is invalid until corrected). */
static bool try_restore_timer_snapshot(time_t now) {
    if (timer_current_date()[0] != '\0')
        return false; /* RTC state intact — normal deep-sleep wake */
    timer_snapshot_t snap;
    if (nvs_config_load_timer_snapshot(&snap) != ESP_OK)
        return false;
    if (!timer_restore_snapshot(&snap, now))
        return false;
    ESP_LOGW(TAG, "Timer state restored from NVS snapshot, state=%d", (int)timer_get_state());
    return true;
}

static const char *reset_reason_str(void);

static void enter_deep_sleep(void) {
    /* Late-wake forensics repeat: the boot-time log of this line is often
       lost to USB CDC re-enumeration; by sleep entry the console has had
       the whole wake to come up. */
    ESP_LOGI(TAG, "this boot: reset %s", reset_reason_str());
    /* Never sleep with the network task alive: it holds WiFi and may be
       mid-publish. Normal paths finished the window already (no-op here);
       this covers cut-short paths. Bounded — on the failsafe path the
       network task may BE the wedge, and deep sleep then powers the radio
       down regardless. No pause polling: this can run in esp_timer
       context. */
    net_window_join(15000, NULL);
    net_window_log_last(); /* timing repeat: the boot-time line is often lost to CDC */
    save_timer_snapshot();
    /* EXT1 ANY_LOW is level-triggered: a still-held button would re-wake
       instantly and re-fire its action. Wait (bounded) for release. */
    for (int i = 0; i < 30 && buttons_scan_held() != 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* Snapshot still-held buttons for the continuation guard. Must read
       BEFORE buttons_configure_wakeup() switches the pads to the RTC mux
       (digital gpio_get_level is unreliable after that). */
    s_held_mask_at_sleep = buttons_scan_held();
    s_sleep_entry_time = (int64_t)time(NULL);

    /* No code path may sleep with the NeoPixel gate LOW — the hold below
       would keep the LEDs powered all night. Ack'd stop: waits for the LED
       task to confirm; on timeout the gate GPIO is forced HIGH without an
       RMT transmit (safe from the failsafe's esp_timer context too). */
    neopixel_stop_sync(500);

    /* Digital pads float in deep sleep; hold the power-control pins so the
       NeoPixel gate (21, HIGH = off) and amp enable (16, LOW = off) cannot
       drift on and drain the battery. neopixel_init() releases the gate
       hold on every wake; the amp hold stays until the (lazy) audio_init
       actually needs the pin — silent wakes leave it held. Re-holding an
       already-held pin is a no-op. */
    gpio_hold_en(GPIO_NUM_21);
    gpio_hold_en(GPIO_NUM_16);
    gpio_deep_sleep_hold_en();

    /* Last NVS write (snapshot) is behind us on every path below; release
       the wake-scoped handle. */
    hal_nvs_close();

    /* Charge-locked: no button wake sources (a press could only burn a
       refresh the battery can't afford) and a fixed long interval instead
       of the planner — wakes only re-check the battery. */
    if (s_charge_locked) {
        esp_sleep_enable_timer_wakeup((uint64_t)CHARGE_LOCK_SLEEP_SEC * 1000000ULL);
        ESP_LOGI(TAG, "Entering deep sleep (charge lock, %d s)", CHARGE_LOCK_SLEEP_SEC);
        esp_deep_sleep_start();
    }

    /* Bed-time locked (charge lock above wins by ordering): buttons stay
       dark until day rollover; fixed ~2 h wakes only re-sync the clock
       and re-check the gate. */
    if (s_bedtime_locked) {
        esp_sleep_enable_timer_wakeup((uint64_t)BEDTIME_SLEEP_SEC * 1000000ULL);
        ESP_LOGI(TAG, "Entering deep sleep (bed time, %d s)", BEDTIME_SLEEP_SEC);
        esp_deep_sleep_start();
    }

    buttons_configure_wakeup();

    /* All sleep-duration policy lives in the pure, host-tested planner
       (sleep_plan.c): minute-boundary alignment for clean renders, the
       NTP early-wake lead, and the expiry/break-end event lead. Alignment
       precision is bounded by the S2's RC-oscillator sleep drift — the
       periodic NTP sync keeps it honest. */
    time_t plan_now = time(NULL);
    sleep_plan_in_t plan_in = {
        .state = timer_get_state(),
        .sec_into_minute = (int)(plan_now % 60),
        .event_remaining_sec = 0,
        .sync_due_by_next_wake = false,
        .break_remaining_sec = 0,
    };
    if (plan_in.state == TIMER_RUNNING) {
        plan_in.event_remaining_sec = (int32_t)(timer_expiry_wall() - (int64_t)plan_now);
        /* due if the recheck window lapses before the wake after next */
        plan_in.sync_due_by_next_wake = timer_needs_ntp_sync(plan_now + 90);
    } else if (plan_in.state == TIMER_BREAK) {
        plan_in.event_remaining_sec = timer_break_remaining(plan_now);
    }
    uint64_t sleep_us = (uint64_t)sleep_plan_seconds(&plan_in) * 1000000ULL;
    esp_sleep_enable_timer_wakeup(sleep_us);
    ESP_LOGI(TAG, "Entering deep sleep (%llu s)", (unsigned long long)(sleep_us / 1000000ULL));
    esp_deep_sleep_start();
}

/* Status pixels: one for timer state here, a different one for WiFi
   (pixel 3, owned by net_window.c) so both can be read at once. */
#define NP_STATE_PIXEL 0

/* Boot forensics: the USB CDC console drops output around sleep/reset
   transitions, so a crash's evidence must ride channels that survive —
   the stat payload (HA "Last reset" sensor) and a late-wake log line.
   Anything but DEEPSLEEP on a wake means the previous wake died. */
static const char *reset_reason_str(void) {
    switch (esp_reset_reason()) {
        case ESP_RST_DEEPSLEEP:
            return "DEEPSLEEP";
        case ESP_RST_POWERON:
            return "POWERON";
        case ESP_RST_BROWNOUT:
            return "BROWNOUT";
        case ESP_RST_PANIC:
            return "PANIC";
        case ESP_RST_INT_WDT:
            return "INT_WDT";
        case ESP_RST_TASK_WDT:
            return "TASK_WDT";
        case ESP_RST_WDT:
            return "WDT";
        case ESP_RST_SW:
            return "SW";
        case ESP_RST_EXT:
            return "EXT";
        default:
            return "UNKNOWN";
    }
}

/* Side-effect-free stat snapshot for the HA session (no timer_tick — a
   read here must never transition the state machine). Assembly rules
   live in app_state.c (host-tested); only the device reads are here. */
static void stats_collect(stats_snapshot_t *out) {
    app_state_in_t in = {
        .batt_mv = battery_read_mv(),
        .light_mv = light_read_mv(),
        .charge_locked = s_charge_locked,
        .parent_testing = PARENT_TESTING,
        .fw_version = esp_app_get_description()->version,
        .reset_reason = reset_reason_str(),
    };
    app_state_stats(&in, time(NULL), out);
}

static void extend_awake_failsafe(int seconds); /* defined with the failsafe */
static void fire_expiry_alert(void);
static bool poll_pause_button(void);
static bool poll_button_a_action(void);

/* ---- network window (WiFi → NTP → snapshot rendezvous → MQTT) ----------
   Mechanics (task, completion signals) live in net_window.c; the
   orchestration (pre-window def capture, post-join reconcile/apply) in
   net_apply.c. main.c only supplies the device effects below. */

/* Collect and hand off the stats snapshot (no-op without a window). */
static void post_stats_snapshot(void) {
    if (!net_window_active())
        return;
    stats_snapshot_t snap;
    stats_collect(&snap);
    net_window_post_snapshot(&snap);
}

/* Join poll: Button A stays live while the MQTT tail drains — the screen
   is already painted and a dropped press would read as broken. Never
   passed from the failsafe's esp_timer context. */
static void poll_button_a_cb(void) {
    (void)poll_button_a_action();
}

/* The window may have applied HA config edits (allocations, holidays,
   school dates, quiet hours, bedtime): drop the wake-scoped caches so
   every read after the finish sees the edited values. */
static void config_caches_invalidate(void) {
    schedule_cache_invalidate();
    s_quiet_cfg_loaded = false;
    s_bedtime_cfg_loaded = false;
}

static void run_locate_alarm(void) {
    alert_run_locate(extend_awake_failsafe);
}

static const net_apply_ops_t NET_APPLY_OPS = {
    .join_poll = poll_button_a_cb,
    .on_config_applied = config_caches_invalidate,
    .on_active_reset_chirp = audio_break_over_chime,
    .on_active_expired_alert = fire_expiry_alert,
    .post_stats = post_stats_snapshot,
    .on_locate = run_locate_alarm,
};

/* Traffic-light state feedback while the slow e-ink refresh runs:
   RUNNING = green, PAUSED = amber, EXPIRED = red, BREAK = cyan,
   IDLE = white. Status class — quiet hours handled by the module. */
static void neopixel_show_timer_state(void) {
    switch (timer_get_state()) {
        case TIMER_RUNNING:
            neopixel_status_pixel(NP_STATE_PIXEL, 0, 20, 0);
            break;
        case TIMER_PAUSED:
            neopixel_status_pixel(NP_STATE_PIXEL, 25, 15, 0);
            break;
        case TIMER_EXPIRED:
            neopixel_status_pixel(NP_STATE_PIXEL, 25, 0, 0);
            break;
        case TIMER_BREAK:
            neopixel_status_pixel(NP_STATE_PIXEL, 0, 10, 25); /* blue-cyan */
            break;
        default:
            neopixel_status_pixel(NP_STATE_PIXEL, 10, 10, 10);
            break;
    }
}

/* Render state via app_state.c (assembly rules host-tested); only the
   battery ADC read is device-side. Light/fw/reset are stats-only and
   deliberately not read here — no ADC work per paint. */
static display_state_t make_state(int32_t remaining, time_t now) {
    int mv = battery_read_mv();
    ESP_LOGD(TAG, "battery: %d mV (%d%%)", mv, battery_percent_from_mv(mv));
    app_state_in_t in = {
        .batt_mv = mv,
        .parent_testing = PARENT_TESTING,
    };
    return app_state_display(&in, remaining, now);
}

/* ---- battery charge lock ---------------------------------------------- */

/* Runs before wake dispatch. Returns normally when operation may continue;
   when the battery is in the lock band it paints Charge Me! once (pausing
   a RUNNING timer so the allocation doesn't burn while the device is
   unusable), then sleeps — this call does not return. */
static void check_charge_lock(void) {
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
            timer_pause(time(NULL));
        }
        display_charge_me(); /* one full refresh; later wakes leave the panel alone */
        /* Best-effort HA notification (charge_lock: true) — the last stat
           before the long battery-recheck sleeps begin. */
        net_apply_try_window();
    }
    enter_deep_sleep(); /* lock-aware: long interval, no button wake */
}

/* ---- bed time ----------------------------------------------------------- */

static int minutes_of_day(time_t t) {
    struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_hour * 60 + tm.tm_min;
}

static int bedtime_cfg_minutes(void) {
    if (!s_bedtime_cfg_loaded) {
        s_bedtime_cfg = NVS_DEFAULT_BEDTIME;
        nvs_config_get_bedtime(&s_bedtime_cfg);
        s_bedtime_cfg_loaded = true;
    }
    int m = bedtime_minutes((int)s_bedtime_cfg);
    if (m < 0 && s_bedtime_cfg != 0) {
        m = bedtime_minutes(NVS_DEFAULT_BEDTIME);
    }
    return m;
}

/* Lock onto the Bed Time screen and sleep - does not return. The timer
   is paused, never expired: day rollover resets the slots overnight, so
   expiring would only skew the daily-summary stats. */
static void bedtime_engage(time_t now, bool alert) {
    s_bedtime_locked = true;
    ESP_LOGW(TAG, "Bed time engaged (state %d%s)", (int)timer_get_state(), alert ? ", alerting" : "");
    if (timer_get_state() == TIMER_RUNNING) {
        timer_pause(now);
    }
    save_timer_snapshot();
    display_bedtime(); /* one full refresh; later wakes leave the panel alone */
    if (alert) {
        alert_run(ALERT_BEDTIME);
    }
    /* Best-effort HA stat before the long no-button sleeps begin. */
    net_apply_try_window();
    enter_deep_sleep(); /* lock-aware: ~2 h interval, no button wake */
}

/* Gate, modeled on check_charge_lock: called from both wake handlers
   right after day rollover (rollover-first ordering is what clears the
   lock on the new day). May not return. */
static void check_bedtime(time_t now) {
    if (!bedtime_active(minutes_of_day(now), bedtime_cfg_minutes())) {
        if (s_bedtime_locked) {
            s_bedtime_locked = false;
            s_bedtime_released = true; /* repaint over the Bed Time screen */
            ESP_LOGW(TAG, "Bed time released");
        }
        return;
    }
    if (!s_bedtime_locked) {
        bedtime_engage(now, bedtime_should_alert(timer_get_state())); /* no return */
    }
    /* Locked re-wake (~2 h cadence): NTP + HA config pickup only, no
       repaint (e-ink retains). Re-check after the window - a bedtime
       edit landing here is the only remote fix path while buttons are
       dead, and it must not wait another 2 h. */
    net_apply_try_window(); /* finish drops the bedtime cache with the rest */
    if (!bedtime_active(minutes_of_day(time(NULL)), bedtime_cfg_minutes())) {
        s_bedtime_locked = false;
        s_bedtime_released = true;
        ESP_LOGW(TAG, "Bed time released (config edit or clock step)");
        return; /* fall through to the normal wake, which repaints */
    }
    enter_deep_sleep();
}

/* Returns true when a break was started (caller should go straight to
   sleep). Persists BREAK before the alarm, same rationale as the EXPIRED
   at-transition save. */
static bool maybe_start_break(time_t now) {
    uint16_t interval_min = NVS_DEFAULT_BREAK_INTERVAL_MIN, duration_min = NVS_DEFAULT_BREAK_DURATION_MIN;
    nvs_config_get_break_interval_min(&interval_min);
    nvs_config_get_break_duration_min(&duration_min);
    if (interval_min == 0) /* eye-rest breaks disabled */
        return false;
    if (!timer_break_due(now, (int32_t)interval_min * 60))
        return false;
    /* A break that would still be running at bedtime is pointless - the
       device would lock mid-break. Skip it and go straight to Bed Time,
       audibly (this is the one alerting path that starts before the
       threshold itself is reached). */
    if (bedtime_break_would_cross(minutes_of_day(now), (int)duration_min, bedtime_cfg_minutes())) {
        ESP_LOGW(TAG, "Screen break due but would cross bed time");
        bedtime_engage(now, true); /* no return */
    }
    ESP_LOGI(TAG, "Screen break due (accum %ld s)", (long)timer_run_accum(now));
    timer_start_break(now, (int32_t)duration_min * 60);
    save_timer_snapshot();
    display_state_t st = make_state(timer_tick(now), now);
    neopixel_show_timer_state(); /* blue during the refresh */
    display_full_refresh(&st);   /* inverted SCREEN BREAK layout */
    alert_run(ALERT_BREAK);      /* pulse end darkens the pixels */
    return true;                 /* caller sleeps; stop_sync guards the gate */
}

/* Full expiry sequence: big TIME'S UP screen, beeps + red pulse, then back
   to the main layout (empty bar, TIME'S UP state in the corner) once the
   alert is dismissed or times out — the big screen would only last until
   the next tick redraw anyway. */
static void fire_expiry_alert(void) {
    /* Persist EXPIRED before the ~15 s alert + redraw, not at the eventual
       enter_deep_sleep: an EN reset or power cut mid-alert would otherwise
       restore the stale RUNNING snapshot and replay the final minute. */
    save_timer_snapshot();
    display_timesup();
    alert_run(ALERT_EXPIRY);
    time_t now = time(NULL);
    display_state_t st = make_state(timer_tick(now), now);
    display_full_refresh(&st);
}

/* ---- day rollover ----------------------------------------------------- */

/* Yesterday's usage numbers for HA, captured BEFORE the rollover resets
   the slots; published by the rollover's own network window. */
static void queue_rollover_summary(void) {
    if (timer_current_date()[0] == '\0') {
        return; /* cold boot / restored-from-nothing: no day to report */
    }
    int32_t used = timer_screen_used_sec(time(NULL));
    uint16_t comp[TIMER_EXTRA_SLOTS];
    for (int i = 0; i < TIMER_EXTRA_SLOTS; i++) {
        comp[i] = timer_slot_completions(1 + i);
    }
    mqtt_ha_queue_summary(timer_current_date(), used, comp);
}

static void handle_day_rollover(time_t *now) {
    if (!timer_is_new_day(*now))
        return;
    /* last_date + wall time in the log: if a rollover ever fires when the
       date has NOT actually changed, this pinpoints why (bad stored date
       vs. stepped clock). */
    ESP_LOGW(TAG, "Day rollover (last_date='%s', now=%lld)", timer_current_date(), (long long)*now);
    queue_rollover_summary();    /* yesterday's stats, before any reset */
    mqtt_ha_queue_bonus_clear(); /* clear the retained HA bonus target this window */
    /* Fail-open: reset to IDLE with today's allocation even if sync fails */
    net_apply_try_window();
    *now = time(NULL);
    /* Power cycling must not refund the allocation: with the clock now
       corrected, a same-day NVS snapshot beats a reset. Only a genuine
       date change (or Button B in parent mode) resets the day. */
    if (try_restore_timer_snapshot(*now)) {
        return;
    }
    timer_reset();
    timer_record_date(*now);
}

/* ---- event watch ------------------------------------------------------- */

/* The planner lands the pre-event wake ~SLEEP_PLAN_EVENT_LEAD_SEC out;
   any wake inside SLEEP_PLAN_WATCH_SEC stays awake so the expiry (TIME'S
   UP) or break end (chime + PAUSED) fires within a tick of wall time. */

/* Buttons are only dispatched on EXT1 wake — while the firmware is awake
   a press would vanish. Long awake waits poll this instead: a Button A
   press while RUNNING pauses immediately (the one action that must not
   be lost — pause is time-sensitive). The GPIO ISR latches the edge the
   moment it lands (even inside an e-ink flush or NTP sync); this consumes
   the latch, so no press is ever lost to a blind spot. Masked take: only
   the A bit is consumed — latched B/C presses stay in the latch for the
   tick-wake drain (a poll during the grid wait must not eat them).
   Returns true when it paused. */
static bool poll_pause_button(void) {
    bool a_pressed = buttons_take_pressed_mask(1u << BTN_A) != 0;
    if (timer_get_state() != TIMER_RUNNING || !a_pressed)
        return false;
    time_t now = time(NULL);
    timer_pause(now);
    ESP_LOGI(TAG, "button A while awake: paused");
    return true;
}

/* Latched Button A during the window join-wait: the screen has already
   painted and the device looks done, so a dropped press reads as broken.
   Mirrors the wake handler: RUNNING pauses, IDLE starts, PAUSED resumes,
   BREAK/EXPIRED stay wake-press-only. The LED acks instantly; the repaint
   rides the post-join changed-state re-render — the panel must stay quiet
   while the MQTT tail is transmitting (brownout, see the snapshot
   rendezvous). The clock was already synced this wake, so a start here
   needs no expiry shift. */
static bool poll_button_a_action(void) {
    if (buttons_take_pressed_mask(1u << BTN_A) == 0)
        return false;
    timer_state_t st = timer_get_state();
    if (button_a_apply(time(NULL)) == BTN_A_NONE)
        return false;
    ESP_LOGI(TAG, "button A during join: state %d -> %d", (int)st, (int)timer_get_state());
    neopixel_show_timer_state();
    return true;
}

/* Absorb the wake residue so the render lands on the state's grid:
   RUNNING/BREAK on the countdown's round minute (the display truly reads
   1:11:00), clock-only states on the wall :00. Bounded — wakes that are
   legitimately off-grid (event watch handoff, slow sync) render where
   they are and self-correct next cycle. Aborts early on a pause press
   (the caller then renders PAUSED, off-grid but honest). */
static void wait_for_render_grid(int max_wait_sec) {
    time_t now = time(NULL);
    timer_state_t st = timer_get_state();
    int32_t event_remaining = 0;
    if (st == TIMER_RUNNING) {
        event_remaining = (int32_t)(timer_expiry_wall() - (int64_t)now);
    } else if (st == TIMER_BREAK) {
        event_remaining = timer_break_remaining(now);
    }
    int32_t to = wake_policy_grid_wait_sec(st, event_remaining, (int)(now % 60), max_wait_sec);
    for (int32_t i = 0; i < to * 10; i++) {
        if (poll_pause_button())
            return;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* BREAK tail: stay awake through the last seconds so the end (chime +
   PAUSED repaint) lands within a tick of wall time. */
static void watch_break_end(void) {
    int32_t brem = timer_break_remaining(time(NULL));
    if (brem <= 0 || brem > SLEEP_PLAN_WATCH_SEC)
        return;
    ESP_LOGI(TAG, "Break ends in %ld s: staying awake", (long)brem);
    neopixel_show_timer_state();
    while (timer_break_remaining(time(NULL)) > 0) {
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    time_t now = time(NULL);
    int32_t remaining = timer_tick(now); /* BREAK -> PAUSED */
    audio_break_over_chime();
    display_state_t st = make_state(remaining, now);
    display_full_refresh(&st);
}

/* RUNNING tail: own the final minute — countdown partials, binary LEDs,
   the pause poll, and the expiry alert at zero. */
static void watch_final_minute(void) {
    time_t now = time(NULL);
    int64_t remaining = timer_expiry_wall() - (int64_t)now;
    if (remaining <= 0 || remaining > SLEEP_PLAN_WATCH_SEC)
        return;

    ESP_LOGI(TAG, "Final minute: staying awake (%lld s remaining)", (long long)remaining);
    buttons_take_pressed(); /* only presses made DURING the watch may pause */
    /* Expiry is a wall time, so a clock step here directly sharpens the
       moment the alert fires. Skip when recently synced or when the sync
       itself (~5-9 s) would blow past the expiry. */
    if (timer_needs_ntp_sync(now) && remaining > 15) {
        net_apply_try_window();
        /* The window's reconcile may have reset/expired the timer (config
           edit); the alert (if any) already fired — don't watch a countdown
           that no longer exists, and never double-fire the alert below. */
        if (timer_get_state() != TIMER_RUNNING) {
            return;
        }
    }
    neopixel_show_timer_state();

    /* Break config read once — the loop below spins at 250 ms. Short
       allocations can put break-due INSIDE this watch (e.g. 3 min screen
       with a 2 min interval: due lands at exactly 60 s remaining); the
       per-wake check in the handlers has already passed by then, so the
       loop must keep checking or the break is silently swallowed by the
       expiry. */
    uint16_t break_interval_min = NVS_DEFAULT_BREAK_INTERVAL_MIN;
    nvs_config_get_break_interval_min(&break_interval_min);

    /* Countdown: partial display steps at the quarter-minute marks (the
       step schedule lives in wake_policy — a late wake skips passed
       marks), and the last 15 s on the pixels as a binary count (status
       class: light green, brightness-scaled, muted by quiet hours). */
    int64_t rem = timer_expiry_wall() - (int64_t)time(NULL);
    int next_step = wake_policy_first_countdown_step((int32_t)rem);
    int32_t leds_shown = -1;
    while ((rem = timer_expiry_wall() - (int64_t)time(NULL)) > 0) {
        /* The event watch owns the whole final minute — without this poll
           a pause press here would be lost and the expiry unavoidable. */
        if (poll_pause_button()) {
            neopixel_stop(); /* clear the binary-countdown pixels */
            time_t pnow = time(NULL);
            display_state_t st = make_state(timer_tick(pnow), pnow);
            neopixel_show_timer_state(); /* amber through the refresh until sleep */
            display_full_refresh(&st);
            return;
        }
        if (break_interval_min != 0 && timer_break_due(time(NULL), (int32_t)break_interval_min * 60)) {
            neopixel_stop(); /* clear the binary-countdown pixels */
            /* Back-to-back renders are safe: display.c absorbs the
               driver's refresh-rate guard interval instead of letting the
               frame be dropped. */
            if (maybe_start_break(time(NULL))) {
                return; /* BREAK painted + alarm run; caller sleeps through it */
            }
        }
        if (next_step < WAKE_COUNTDOWN_STEPS && rem <= (int64_t)wake_policy_countdown_step(next_step)) {
            time_t step_now = time(NULL);
            display_state_t st = make_state(wake_policy_countdown_step(next_step), step_now);
            display_update(&st); /* partial; ~2-3 s, well under the 15 s spacing */
            next_step++;
        }
        if (rem <= 15 && (int32_t)rem != leds_shown) {
            neopixel_status_binary4((uint8_t)rem, 20, 60, 20); /* light green */
            leds_shown = (int32_t)rem;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    timer_tick(time(NULL)); /* RUNNING -> EXPIRED */
    fire_expiry_alert();
}

static void maybe_wait_for_event(void) {
    if (timer_get_state() == TIMER_BREAK) {
        watch_break_end();
    } else if (timer_get_state() == TIMER_RUNNING) {
        watch_final_minute();
    }
}

/* ---- wake handlers ----------------------------------------------------- */

/* Apply one button action (A/B/C — D is wake-only). Shared by the EXT1
   wake handler and the tick-wake latch drain so both honour the same
   state guards. Refreshes *before on a Button C selection change so
   landing on an already-EXPIRED slot does not re-fire the expiry alert.
   allow_net_window gates the NTP window on a start/resume: a wake that
   already ran a window skips the redundant second one (clock corrected,
   buffered HA effects already applied). Returns true when the press
   changed timer state (the caller must render). */
static bool dispatch_button_action(button_id_t btn, time_t *now, timer_state_t *before, bool allow_net_window) {
    switch (btn) {
        case BTN_A:
            if (*before == TIMER_BREAK) {
                ESP_LOGI(TAG, "button A ignored during screen break");
                return false;
            }
            /* Start/resume immediately — waiting on NTP first confused
               users. Sync runs after; any clock step is applied to the
               expiry via timer_shift_expiry (measured against the
               monotonic clock, which NTP cannot step). */
            switch (button_a_apply(*now)) {
                case BTN_A_STARTED:
                case BTN_A_RESUMED:
                    /* Hold the pre-press colour briefly so the WHITE/AMBER ->
                       GREEN transition is visible as an acknowledgement */
                    vTaskDelay(pdMS_TO_TICKS(250));
                    neopixel_show_timer_state();

                    /* NTP-gated paint: wait only for the sync (seconds) so the
                       panel renders once, with the corrected clock and shifted
                       expiry. The MQTT phase is released AFTER the paint (the
                       snapshot post in the finish tail) and joined before
                       sleep. Fail-open: on sync failure the timer keeps
                       running on the uncorrected clock — remaining time is
                       still a consistent duration; only the shown clock may
                       be off. If the sync settles late (during the MQTT
                       tail), the finish applies the step instead. */
                    if (allow_net_window && net_apply_open()) {
                        if (net_window_wait_ntp()) {
                            timer_shift_expiry(net_window_take_clock_step());
                        } else {
                            net_apply_note_start_unsynced();
                        }
                    }
                    *now = time(NULL);
                    return true;
                case BTN_A_PAUSED:
                    return true;
                default:
                    return false; /* EXPIRED: renders only (wake path) */
            }
        case BTN_B:
            /* Reset the selected timer to full: reloadable extras without
               ParentTesting, anything else with it — never while RUNNING
               (B is dropped from the wake mask then, same as C; this guard
               covers presses that ride in on another wake). */
            if (!timer_reload_allowed(PARENT_TESTING) || !timer_reload()) {
                ESP_LOGI(TAG, "Button B reset unavailable (state %d)", (int)*before);
                return false;
            }
            return true;
        case BTN_C:
            /* Swap timer type; refused while RUNNING (pause first) or in a
               Screen Break (enforced). Landing on an already-EXPIRED timer
               is a selection change, not a transition — refresh the
               baseline so the expiry alert does not re-fire below. */
            if (timer_select_next()) {
                ESP_LOGI(TAG, "button C: selected slot %d", timer_active_slot());
                *before = timer_get_state();
                return true;
            }
            ESP_LOGI(TAG, "button C swap unavailable (state %d)", (int)*before);
            return false;
        default:
            return false;
    }
}

/* Post-action tail shared by the wake handler and the tick-wake drain:
   render the resulting state, release the MQTT phase, join the window,
   and re-render when the join changed what the panel shows. */
static void finish_action_and_render(button_id_t btn, timer_state_t before, time_t now) {
    int32_t remaining = timer_tick(now);
    display_state_t st = make_state(remaining, now);

    if (before == TIMER_BREAK && timer_get_state() == TIMER_PAUSED) {
        audio_break_over_chime(); /* break over — ready to resume */
    }

    /* Button D is the user-facing "refresh everything" button — it always
       gets a real full refresh regardless of the render policy. */
    bool force_full = (btn == BTN_D);
    wake_render_t bwr = wake_policy_render(before, timer_get_state(), true, false);
    if (bwr == WAKE_RENDER_EXPIRY_ALERT) {
        fire_expiry_alert(); /* alert owns the NeoPixels (red pulse) */
    } else {
        /* Includes EXPIRED: any button returns the display to the main
           layout (empty bar, TIME'S UP state). */
        ESP_LOGI(TAG, "button %d: state %d -> %d, %s refresh", (int)btn, (int)before, (int)timer_get_state(),
                 (force_full || bwr == WAKE_RENDER_FULL) ? "full" : "partial");
        neopixel_show_timer_state(); /* resulting state, lit until sleep */
        if (force_full || bwr == WAKE_RENDER_FULL) {
            display_full_refresh(&st);
        } else {
            display_update(&st); /* partial cadence: every Nth is promoted */
        }
    }

    /* Paint done: release the MQTT phase (display refresh current and
       radio TX bursts must never coincide — brownout), then join, apply
       the buffered network→timer effects, reconcile a redefined timer.
       Re-render only when something changed what the panel shows (a
       Button A action landed during the join, a config edit moved the
       timer, or the expiry passed while draining). */
    post_stats_snapshot();
    timer_state_t painted = timer_get_state();
    net_finish_t nf = net_apply_finish();
    if (nf != NET_FINISH_ALERTED && (nf == NET_FINISH_CHANGED || timer_get_state() != painted)) {
        time_t rnow = time(NULL);
        int32_t rrem = timer_tick(rnow);
        wake_render_t rwr = wake_policy_render(painted, timer_get_state(), true, false);
        if (rwr == WAKE_RENDER_EXPIRY_ALERT) {
            fire_expiry_alert();
        } else {
            display_state_t rst = make_state(rrem, rnow);
            neopixel_show_timer_state();
            if (force_full || rwr == WAKE_RENDER_FULL) {
                display_full_refresh(&rst);
            } else {
                display_update(&rst);
            }
        }
    }
}

/* Shared post-action tail: if the action pushed the accrual past the
   break interval (e.g. a resume landing after it), paint the break and
   sleep through it; otherwise render the action's result and drain the
   window. */
static void finish_or_break(button_id_t btn, timer_state_t before, time_t now) {
    if (maybe_start_break(now)) {
        post_stats_snapshot(); /* break screen painted: release MQTT */
        net_apply_finish();    /* drain + apply deferred before sleeping */
        enter_deep_sleep();
    }
    finish_action_and_render(btn, before, now);
}

static void handle_timer_tick(void) {
    time_t now = time(NULL);
    handle_day_rollover(&now);
    check_bedtime(now); /* may not return; before the sync block so a
                           locked re-wake runs exactly one net window
                           (the rare release-by-edit fall-through repaints
                           and may add this wake's regular sync) */

    bool synced_this_wake = false;
    if (wake_policy_sync_due(timer_get_state(), timer_needs_ntp_sync(now), now, timer_last_ntp_sync(),
                             IDLE_SYNC_INTERVAL_SEC)) {
        net_apply_try_window();
        now = time(NULL);
        synced_this_wake = true;
    }

    /* Cold boot / external reset only: the rollover + sync above already
       showed the WiFi pixel, but the grid wait + first paint below can
       hold a blank panel for tens of seconds more with buttons still
       wake-press-only — a dark, silent device reads as hung (field
       report). Deep-sleep tick wakes stay dark: a dim blink every minute,
       all day, isn't worth the battery. */
    if (esp_reset_reason() != ESP_RST_DEEPSLEEP) {
        neopixel_show_timer_state();
    }

    if (maybe_start_break(now)) {
        enter_deep_sleep(); /* break just started; sleep through it */
    }

    /* Land the render on the state's grid — the planner woke us on (or,
       when a sync was due, ~20 s before) the grid point; absorb the
       residue here. 25 s covers the sync lead without stalling
       event-watch wakes. Captured BEFORE the wait: a pause press during
       it must register as a state change (full refresh). Skipped on
       power-on/reset: the panel is blank and holding it dark for up to
       25 more seconds (field: 19 s) is worse than one off-minute render
       — the next tick wake re-aligns. */
    timer_state_t before = timer_get_state();
    if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
        wait_for_render_grid(25);
    }
    now = time(NULL);

    int32_t remaining = timer_tick(now);

    /* The grid wait makes the true remaining a round minute at render
       time; snap away wake/render jitter so 1:10:59 never shows.
       Genuinely off-grid renders (slow sync) stay honest. */
    int32_t shown = remaining;
    if (timer_get_state() == TIMER_RUNNING) {
        shown = wake_policy_snap_minute(shown, SLEEP_PLAN_WATCH_SEC);
    }
    display_state_t st = make_state(shown, now);
    if (st.timer_state == TIMER_BREAK) {
        st.break_remaining_sec = wake_policy_snap_minute(st.break_remaining_sec, SLEEP_PLAN_WATCH_SEC);
    }

    if (before == TIMER_BREAK && timer_get_state() == TIMER_PAUSED) {
        audio_break_over_chime(); /* break over — ready to resume */
    }

    wake_render_t wr = wake_policy_render(before, timer_get_state(), false, false);
    if ((s_charge_lock_released || s_bedtime_released) && wr == WAKE_RENDER_PARTIAL) {
        wr = WAKE_RENDER_FULL; /* the panel still shows a lock screen — repaint fully */
    }
    switch (wr) {
        case WAKE_RENDER_EXPIRY_ALERT:
            fire_expiry_alert();
            break;
        case WAKE_RENDER_FULL:
            display_full_refresh(&st);
            break;
        default:
            display_update(&st); /* partial; policy promotes every 5th to full */
            break;
    }

    /* A press that landed while this wake was awake (sync, grid wait,
       e-ink flush) is in the latch — act on it now or it evaporates at
       deep sleep (losing the start/pause race against the minute render).
       Same guards as a wake press via the shared dispatch; D stays
       wake-press-only. Must run BEFORE maybe_wait_for_event: the
       final-minute watch discards pre-watch latched presses at entry. */
    int pick = button_latch_pick(buttons_take_pressed(), (1u << BTN_A) | (1u << BTN_B) | (1u << BTN_C));
    if (pick >= 0) {
        timer_state_t painted = timer_get_state();
        if (dispatch_button_action((button_id_t)pick, &now, &painted, !synced_this_wake)) {
            neopixel_show_timer_state();
            finish_or_break((button_id_t)pick, painted, now);
        }
    }
    maybe_wait_for_event();
    enter_deep_sleep();
}

static void handle_button_wake(void) {
    button_id_t btn = buttons_get_wakeup_button();

    /* Continuation of a hold, not a new press: same button as at sleep
       entry and the sleep lasted no time at all. Skip all action AND
       display work (a hold would otherwise churn the panel every ~3 s)
       and go back to waiting for release. */
    if (btn != BTN_NONE && (s_held_mask_at_sleep & (1u << (int)btn)) && (int64_t)time(NULL) - s_sleep_entry_time <= 2) {
        ESP_LOGI(TAG, "button %d still held from previous wake - ignoring", (int)btn);
        enter_deep_sleep(); /* does not return */
    }

    /* Immediate "button heard" ack — current state colour, updated to the
       resulting state below once the action has run. */
    neopixel_show_timer_state();

    time_t now = time(NULL);
    handle_day_rollover(&now);
    /* IDLE overnight: the threshold crossing may first be observed on a
       button press (idle wakes are up to an hour apart). The press is
       swallowed and the transition is silent per the alert rules. */
    check_bedtime(now); /* may not return */
    timer_state_t before = timer_get_state();

    switch (btn) {
        case BTN_A:
        case BTN_B:
        case BTN_C:
            dispatch_button_action(btn, &now, &before, true);
            break;
        case BTN_D:
            /* NTP-gated paint, same as BTN A: sync now, MQTT after paint */
            if (net_apply_open()) {
                net_window_wait_ntp();
            }
            now = time(NULL);
            break;
        case BTN_NONE:
        default:
            break;
    }

    /* Drain latch: the wake press itself was handled via the EXT1 decode
       above; its release bounce (or a second tap during the action) must
       not replay through the awake-press consumers below — e.g. a resume
       with <70 s remaining flows straight into the final-minute watch,
       where a stale A edge would instantly re-pause. */
    buttons_take_pressed();

    finish_or_break(btn, before, now); /* e.g. resume with accrual already past the interval */
    maybe_wait_for_event();
    enter_deep_sleep();
}

/* Last-resort battery protection: no wake may run forever (WiFi driver
   hang, stuck BUSY, firmware bug) — the CPU would otherwise stay awake
   until the battery dies. Runs in the esp_timer task; enter_deep_sleep
   persists the snapshot first, so no allocation is lost. A mid-refresh
   force-sleep can leave the panel scruffy for one frame — acceptable for
   a path that only fires when something is already wedged. */
static void awake_failsafe_cb(void *arg) {
    (void)arg;
    ESP_LOGE(TAG, "Awake failsafe: still awake after %d s - forcing deep sleep", CONFIG_MAGTAG_MAX_AWAKE_SEC);
    enter_deep_sleep();
}

static esp_timer_handle_t s_failsafe_timer;

static void arm_awake_failsafe(void) {
    static const esp_timer_create_args_t args = {.callback = awake_failsafe_cb, .name = "awake_cap"};
    esp_err_t ret = esp_timer_create(&args, &s_failsafe_timer);
    if (ret == ESP_OK) {
        ret = esp_timer_start_once(s_failsafe_timer, (uint64_t)CONFIG_MAGTAG_MAX_AWAKE_SEC * 1000000ULL);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "awake failsafe not armed: %s", esp_err_to_name(ret));
    }
}

/* Push the awake failsafe out so a long deliberate awake stretch (the
   locate alarm) isn't cut short by it. */
static void extend_awake_failsafe(int seconds) {
    if (s_failsafe_timer != NULL) {
        esp_timer_stop(s_failsafe_timer);
        esp_timer_start_once(s_failsafe_timer, (uint64_t)seconds * 1000000ULL);
    }
}

void app_main(void) {
    /* MUST be first peripheral call: GPIO 21 power gate HIGH (NeoPixels off) */
    neopixel_init();
    arm_awake_failsafe();
    neopixel_set_quiet_cb(status_leds_quiet);
    neopixel_set_status_brightness(CONFIG_MAGTAG_STATUS_LED_BRIGHTNESS);

    /* Panic-loop breaker: the S2 ROM USB console can panic when a host
       port-open races boot prints (seen in bring-up). Each panic reboots,
       re-enumerates USB, and re-races — freezing the device for as long
       as a monitor keeps reconnecting. After a panic, stay quiet briefly
       so the host's open completes against silence and the loop breaks. */
    if (esp_reset_reason() == ESP_RST_PANIC) {
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(nvs_config_init_defaults());

    /* TZ from NVS (HA config-in) with the compile-time default as fallback */
    char tz[48];
    nvs_config_get_tz(tz, sizeof(tz));
    setenv("TZ", tz, 1);
    tzset();

    /* Slot definitions live in rodata, not RTC memory — install them
       before the first timer_* call on every boot/wake. */
    timer_defs_install();

    /* Must run after TZ is set (date comparison) and before the wake
       handlers (whose rollover check would otherwise reset the timer). */
    try_restore_timer_snapshot(time(NULL));

    net_apply_init(&NET_APPLY_OPS);
    buttons_init();
    battery_init();
    /* audio + light init lazily on first use (most wakes need neither);
       until then the amp pin stays under its deep-sleep hold (off). */
    display_init();

    /* Heap headroom check: the LED + network task stacks now ride
       alongside WiFi and the LVGL framebuffer — regressions show up here
       long before an alloc fails in the field. */
    ESP_LOGI(TAG, "free heap after init: %lu B (min ever %lu B)", (unsigned long)esp_get_free_heap_size(),
             (unsigned long)esp_get_minimum_free_heap_size());

    uint32_t causes = esp_sleep_get_wakeup_causes();
    /* Reset reason distinguishes a real cold boot from an external reset
       (e.g. monitor DTR/RTS) — both report wake cause UNDEFINED. */
    ESP_LOGI(TAG, "Wakeup causes: 0x%08lx, reset reason: %d", (unsigned long)causes, (int)esp_reset_reason());

    /* Battery gate before any wake work: does not return while locked */
    check_charge_lock();

    if (causes & BIT(ESP_SLEEP_WAKEUP_EXT1)) {
        handle_button_wake();
    } else {
        handle_timer_tick(); /* RTC timer wake AND cold boot */
    }
}
