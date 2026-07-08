#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "audio.h"
#include "battery.h"
#include "battery_policy.h"
#include "buttons.h"
#include "display.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "neopixel.h"
#include "ntp.h"
#include "nvs_config.h"
#include "nvs_flash.h"
#include "quiet_hours.h"
#include "schedule.h"
#include "sleep_plan.h"
#include "timer.h"
#include "wake_policy.h"

static const char *TAG = "main";

/* Compile-time timezone (ProductOverview section 1) */
#define MAGTAG_TZ "EST5EDT,M3.2.0,M11.1.0"
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
   exempt — they accompany an audible, dismissable alarm). */
static bool status_leds_quiet(void) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    return quiet_hours_active(tm.tm_hour * 60 + tm.tm_min, quiet_hhmm_to_minutes(CONFIG_MAGTAG_QUIET_START_HHMM),
                              quiet_hhmm_to_minutes(CONFIG_MAGTAG_QUIET_END_HHMM));
}

static RTC_DATA_ATTR time_t s_last_ntp_sync;

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
    if (g_rtc_state.last_date[0] != '\0')
        return false; /* RTC state intact — normal deep-sleep wake */
    timer_snapshot_t snap;
    if (nvs_config_load_timer_snapshot(&snap) != ESP_OK)
        return false;
    if (!timer_restore_snapshot(&snap, now))
        return false;
    ESP_LOGW(TAG, "Timer state restored from NVS snapshot, state=%d", (int)timer_get_state());
    return true;
}

static void enter_deep_sleep(void) {
    save_timer_snapshot();
    /* EXT1 ANY_LOW is level-triggered: a still-held button would re-wake
       instantly and re-fire its action. Wait (bounded) for release. */
    for (int i = 0; i < 30; i++) {
        bool held = false;
        for (int b = 0; b < 4; b++) {
            held = held || buttons_is_pressed((button_id_t)b);
        }
        if (!held)
            break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* Snapshot still-held buttons for the continuation guard. Must read
       BEFORE buttons_configure_wakeup() switches the pads to the RTC mux
       (digital gpio_get_level is unreliable after that). */
    s_held_mask_at_sleep = 0;
    for (int b = 0; b < 4; b++) {
        if (buttons_is_pressed((button_id_t)b))
            s_held_mask_at_sleep |= (uint8_t)(1u << b);
    }
    s_sleep_entry_time = (int64_t)time(NULL);

    /* No code path may sleep with the NeoPixel gate LOW — the hold below
       would keep the LEDs powered all night. */
    neopixel_stop();

    /* Digital pads float in deep sleep; hold the power-control pins so the
       NeoPixel gate (21, HIGH = off) and amp enable (16, LOW = off) cannot
       drift on and drain the battery. Released in the *_init() on wake. */
    gpio_hold_en(GPIO_NUM_21);
    gpio_hold_en(GPIO_NUM_16);
    gpio_deep_sleep_hold_en();

    /* Charge-locked: no button wake sources (a press could only burn a
       refresh the battery can't afford) and a fixed long interval instead
       of the planner — wakes only re-check the battery. */
    if (s_charge_locked) {
        esp_sleep_enable_timer_wakeup((uint64_t)CHARGE_LOCK_SLEEP_SEC * 1000000ULL);
        ESP_LOGI(TAG, "Entering deep sleep (charge lock, %d s)", CHARGE_LOCK_SLEEP_SEC);
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

/* Status pixels: one for timer state, a different one for WiFi, so both
   can be read at once. Swap the indices if the physical layout reads
   better the other way around. */
#define NP_STATE_PIXEL 0
#define NP_WIFI_PIXEL 3

/* WiFi lifecycle is entirely inside ntp_sync(): init->connect->sync->deinit */
static esp_err_t try_ntp_sync(void) {
#if CONFIG_MAGTAG_SYNC_LED_FEEDBACK
    /* status class: quiet hours + brightness handled inside the module */
    neopixel_status_pixel(NP_WIFI_PIXEL, 0, 0, 20); /* blue: sync in progress */
#endif
    esp_err_t ret = ntp_sync();
    if (ret == ESP_OK) {
        s_last_ntp_sync = time(NULL);
        timer_record_ntp_sync(s_last_ntp_sync);
    } else {
        ESP_LOGW(TAG, "NTP sync failed: %s", esp_err_to_name(ret));
#if CONFIG_MAGTAG_SYNC_LED_FEEDBACK
        for (int i = 0; i < 3; i++) {
            neopixel_status_pixel(NP_WIFI_PIXEL, 30, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(150));
            neopixel_status_pixel(NP_WIFI_PIXEL, 0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(150));
        }
#endif
    }
#if CONFIG_MAGTAG_SYNC_LED_FEEDBACK
    /* Clear only the WiFi pixel — the state pixel stays lit through the
       e-ink refresh; enter_deep_sleep() guarantees the gate goes HIGH. */
    neopixel_status_pixel(NP_WIFI_PIXEL, 0, 0, 0);
#endif
    return ret;
}

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

static display_state_t make_state(int32_t remaining, time_t now) {
    day_type_t dt = schedule_get_day_type(now);
    /* Extra timers have a fixed configured duration; Screen (slot 0)
       follows the day schedule. */
    const timer_def_t *def = timer_active_def();
    uint32_t alloc = (def != NULL) ? (uint32_t)def->duration_sec : schedule_get_allocation_sec(dt);
    /* IDLE shows today's full allocation (full bar), not 0 (ProductOverview) */
    if (timer_get_state() == TIMER_IDLE) {
        remaining = (int32_t)alloc;
    }
    timer_state_t ts = timer_get_state();
    int mv = battery_read_mv();
    int pct = battery_percent_from_mv(mv);
    ESP_LOGI(TAG, "battery: %d mV (%d%%)", mv, pct);
    return (display_state_t){
        .remaining_sec = remaining,
        .allocation_sec = alloc,
        .timer_state = ts,
        .day_type = dt,
        .wall_time = now,
        .last_sync_time = s_last_ntp_sync,
        .battery_pct = (uint8_t)pct,
        .break_remaining_sec = timer_break_remaining(now),
        .break_duration_sec = (uint32_t)CONFIG_MAGTAG_BREAK_DURATION_MIN * 60,
        .timer_name = (def != NULL) ? def->name : NULL,
        .charge_warn = battery_policy_evaluate(pct, false) != BATT_OK,
        .completions = timer_completions(),
        .reloadable = (def != NULL) && def->reloadable,
        .swap_available = timer_swap_allowed(),
        .reload_available = timer_reload_allowed(PARENT_TESTING),
    };
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
    }
    enter_deep_sleep(); /* lock-aware: long interval, no button wake */
}

/* ---- expiry alert ---------------------------------------------------- */

static volatile bool s_audio_done;

static void audio_alert_task(void *arg) {
    (void)arg;
    audio_beep_sequence(); /* self-terminates after 5 cycles (~15 s) */
    s_audio_done = true;
    vTaskDelete(NULL);
}

static void run_expiry_alert(void) {
    s_audio_done = false;
    buttons_take_pressed();                /* drain: a press from BEFORE the alarm must not pre-dismiss it */
    neopixel_alert_pulse_begin(248, 0, 0); /* red; task + teardown owned by the module */
    xTaskCreate(audio_alert_task, "beep", 2048, NULL, 5, NULL);

    /* Wait for dismissal — latched taps of any length count, the level
       scan catches a button already held down through the drain; cap
       slightly past the alarm (3 s per cycle). */
    bool dismissed = false;
    for (int i = 0; i < CONFIG_MAGTAG_EXPIRY_ALARM_CYCLES * 30 + 10 && !s_audio_done && !dismissed; i++) {
        dismissed = buttons_take_pressed() != 0;
        for (int b = 0; b < 4 && !dismissed; b++) {
            dismissed = buttons_is_pressed((button_id_t)b);
        }
        if (dismissed) {
            ESP_LOGI(TAG, "Alert dismissed by button");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    audio_stop();
    neopixel_alert_pulse_end();
    vTaskDelay(pdMS_TO_TICKS(100)); /* let the audio task observe its stop flag and exit */
}

/* ---- eye-rest break ---------------------------------------------------- */

static void break_alarm_task(void *arg) {
    (void)arg;
    audio_break_alarm(); /* ~6 s, stop-flag aware */
    s_audio_done = true;
    vTaskDelete(NULL);
}

/* Break-start alarm: beeps + cyan pulse. Alert-class, so it fires during
   quiet hours (like the expiry alert — it accompanies an audible alarm).
   Any button silences it. */
static void run_break_alarm(void) {
    s_audio_done = false;
    buttons_take_pressed();                  /* drain: only presses AFTER the alarm starts silence it */
    neopixel_alert_pulse_begin(0, 150, 220); /* cyan — matches the BREAK identity */
    xTaskCreate(break_alarm_task, "brk_alarm", 2048, NULL, 5, NULL);
    /* Cap slightly past the alarm (~2.2 s per cycle) */
    bool silenced = false;
    for (int i = 0; i < CONFIG_MAGTAG_BREAK_ALARM_CYCLES * 22 + 10 && !s_audio_done && !silenced; i++) {
        silenced = buttons_take_pressed() != 0;
        for (int b = 0; b < 4 && !silenced; b++) {
            silenced = buttons_is_pressed((button_id_t)b);
        }
        if (silenced) {
            ESP_LOGI(TAG, "Break alarm silenced by button");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    audio_stop();
    neopixel_alert_pulse_end();
    vTaskDelay(pdMS_TO_TICKS(100)); /* let the alarm task observe the stop flag */
}

/* Returns true when a break was started (caller should go straight to
   sleep). Persists BREAK before the alarm, same rationale as the EXPIRED
   at-transition save. */
static bool maybe_start_break(time_t now) {
#if CONFIG_MAGTAG_BREAK_INTERVAL_MIN > 0
    if (!timer_break_due(now, CONFIG_MAGTAG_BREAK_INTERVAL_MIN * 60))
        return false;
    ESP_LOGI(TAG, "Screen break due (accum %ld s)", (long)timer_run_accum(now));
    timer_start_break(now, CONFIG_MAGTAG_BREAK_DURATION_MIN * 60);
    save_timer_snapshot();
    display_state_t st = make_state(timer_tick(now), now);
    neopixel_show_timer_state(); /* blue during the refresh */
    display_full_refresh(&st);   /* inverted SCREEN BREAK layout */
    run_break_alarm();
    neopixel_stop();
    return true;
#else
    (void)now;
    return false;
#endif
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
    run_expiry_alert();
    time_t now = time(NULL);
    display_state_t st = make_state(timer_tick(now), now);
    display_full_refresh(&st);
}

/* ---- day rollover ----------------------------------------------------- */

static void handle_day_rollover(time_t *now) {
    if (!timer_is_new_day(*now))
        return;
    /* last_date + wall time in the log: if a rollover ever fires when the
       date has NOT actually changed, this pinpoints why (bad stored date
       vs. stepped clock). */
    ESP_LOGW(TAG, "Day rollover (last_date='%s', now=%lld)", g_rtc_state.last_date, (long long)*now);
    /* Fail-open: reset to IDLE with today's allocation even if sync fails */
    try_ntp_sync();
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
   the latch, so no press is ever lost to a blind spot. Latched B/C/D
   presses in the same take are dropped by design (wake-press-only
   semantics). Returns true when it paused. */
static bool poll_pause_button(void) {
    bool a_pressed = (buttons_take_pressed() & (1u << BTN_A)) != 0;
    if (timer_get_state() != TIMER_RUNNING || !a_pressed)
        return false;
    time_t now = time(NULL);
    timer_pause(now);
    ESP_LOGI(TAG, "button A while awake: paused");
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
    int to;
    if (timer_get_state() == TIMER_RUNNING) {
        to = (int)((timer_expiry_wall() - (int64_t)now) % 60);
    } else if (timer_get_state() == TIMER_BREAK) {
        to = timer_break_remaining(now) % 60;
    } else {
        to = 60 - (int)(now % 60);
        if (to == 60)
            to = 0; /* already on the wall boundary */
    }
    if (to > 0 && to <= max_wait_sec) {
        for (int i = 0; i < to * 10; i++) {
            if (poll_pause_button())
                return;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

static void maybe_wait_for_event(void) {
    if (timer_get_state() == TIMER_BREAK) {
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
        return;
    }

    if (timer_get_state() != TIMER_RUNNING)
        return;
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
        try_ntp_sync();
    }
    neopixel_show_timer_state();

    /* Countdown: partial display steps at the quarter-minute marks (values
       pinned so the text reads exactly 00:01:00/45/30/15), and the last
       15 s on the pixels as a binary count (status class: light green,
       brightness-scaled, muted by quiet hours). */
    static const int32_t STEPS[] = {60, 45, 30, 15};
    const int n_steps = (int)(sizeof(STEPS) / sizeof(STEPS[0]));
    int next_step = 0;
    int64_t rem = timer_expiry_wall() - (int64_t)time(NULL);
    while (next_step < n_steps && (int64_t)STEPS[next_step] > rem) {
        next_step++; /* woke late (e.g. slow sync): skip already-passed steps */
    }
    int32_t leds_shown = -1;
    while ((rem = timer_expiry_wall() - (int64_t)time(NULL)) > 0) {
        /* The event watch owns the whole final minute — without this poll
           a pause press here would be lost and the expiry unavoidable. */
        if (poll_pause_button()) {
            neopixel_stop(); /* clear the binary-countdown pixels */
            time_t pnow = time(NULL);
            display_state_t st = make_state(timer_tick(pnow), pnow);
            neopixel_show_timer_state(); /* amber during the refresh */
            display_full_refresh(&st);
            neopixel_stop();
            return;
        }
        if (next_step < n_steps && rem <= (int64_t)STEPS[next_step]) {
            time_t step_now = time(NULL);
            display_state_t st = make_state(STEPS[next_step], step_now);
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

/* ---- wake handlers ----------------------------------------------------- */

static void handle_timer_tick(void) {
    time_t now = time(NULL);
    handle_day_rollover(&now);

    if (wake_policy_sync_due(timer_get_state(), timer_needs_ntp_sync(now), now, s_last_ntp_sync,
                             IDLE_SYNC_INTERVAL_SEC)) {
        try_ntp_sync();
        now = time(NULL);
    }

    if (maybe_start_break(now)) {
        enter_deep_sleep(); /* break just started; sleep through it */
    }

    /* Land the render on the state's grid — the planner woke us on (or,
       when a sync was due, ~20 s before) the grid point; absorb the
       residue here. 25 s covers the sync lead without stalling
       event-watch wakes. Captured BEFORE the wait: a pause press during
       it must register as a state change (full refresh). */
    timer_state_t before = timer_get_state();
    wait_for_render_grid(25);
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

    wake_render_t wr = wake_policy_render(before, timer_get_state(), false);
    if (s_charge_lock_released && wr == WAKE_RENDER_PARTIAL) {
        wr = WAKE_RENDER_FULL; /* the panel still shows Charge Me! — repaint fully */
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
    timer_state_t before = timer_get_state();

    switch (btn) {
        case BTN_A:
            if (before == TIMER_BREAK) {
                ESP_LOGI(TAG, "button A ignored during screen break");
            } else if (before == TIMER_RUNNING) {
                timer_pause(now);
            } else if (before == TIMER_IDLE || before == TIMER_PAUSED) {
                /* Start/resume immediately — waiting on NTP first confused
                   users. Sync runs after; any clock step is applied to the
                   expiry via timer_shift_expiry (measured against the
                   monotonic clock, which NTP cannot step). */
                if (before == TIMER_IDLE) {
                    const timer_def_t *def = timer_active_def();
                    int32_t alloc = (def != NULL) ? def->duration_sec
                                                  : (int32_t)schedule_get_allocation_sec(schedule_get_day_type(now));
                    timer_start(now, alloc);
                } else {
                    timer_resume(now);
                }
                /* Hold the pre-press colour briefly so the WHITE/AMBER ->
                   GREEN transition is visible as an acknowledgement */
                vTaskDelay(pdMS_TO_TICKS(250));
                neopixel_show_timer_state();

                int64_t mono_before_us = esp_timer_get_time();
                time_t wall_before = time(NULL);
                if (try_ntp_sync() == ESP_OK) {
                    int64_t elapsed_sec = (esp_timer_get_time() - mono_before_us) / 1000000;
                    int64_t step = (int64_t)time(NULL) - ((int64_t)wall_before + elapsed_sec);
                    timer_shift_expiry(step);
                }
                /* Fail-open: on sync failure the timer keeps running on the
                   uncorrected clock — remaining time is still a consistent
                   duration; only the displayed clock may be off. */
                now = time(NULL);
            }
            break;
        case BTN_B:
            /* Reset the selected timer to full: reloadable extras without
               ParentTesting, anything else with it — never while RUNNING
               (B is dropped from the wake mask then, same as C; this guard
               covers presses that ride in on another wake). */
            if (!timer_reload_allowed(PARENT_TESTING) || !timer_reload()) {
                ESP_LOGI(TAG, "Button B reset unavailable (state %d)", (int)before);
            }
            break;
        case BTN_C:
            /* Swap timer type; refused while RUNNING (pause first) or in a
               Screen Break (enforced). Landing on an already-EXPIRED timer
               is a selection change, not a transition — refresh the
               baseline so the expiry alert does not re-fire below. */
            if (timer_select_next()) {
                ESP_LOGI(TAG, "button C: selected slot %d", timer_active_slot());
                before = timer_get_state();
            } else {
                ESP_LOGI(TAG, "button C swap unavailable (state %d)", (int)before);
            }
            break;
        case BTN_D:
            try_ntp_sync();
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

    if (maybe_start_break(now)) {
        enter_deep_sleep(); /* e.g. resume with accrual already past the interval */
    }

    int32_t remaining = timer_tick(now);
    display_state_t st = make_state(remaining, now);

    if (before == TIMER_BREAK && timer_get_state() == TIMER_PAUSED) {
        audio_break_over_chime(); /* break over — ready to resume */
    }

    if (wake_policy_render(before, timer_get_state(), true) == WAKE_RENDER_EXPIRY_ALERT) {
        fire_expiry_alert(); /* alert owns the NeoPixels (red pulse) */
    } else {
        /* Includes EXPIRED: any button returns the display to the main
           layout (empty bar, TIME'S UP state) via a full refresh. */
        ESP_LOGI(TAG, "button %d: state %d -> %d, full refresh", (int)btn, (int)before, (int)timer_get_state());
        neopixel_show_timer_state(); /* resulting state, shown during refresh */
        display_full_refresh(&st);   /* button wakes always full-refresh */
        neopixel_stop();
    }
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

static void arm_awake_failsafe(void) {
    static const esp_timer_create_args_t args = {.callback = awake_failsafe_cb, .name = "awake_cap"};
    esp_timer_handle_t t;
    esp_err_t ret = esp_timer_create(&args, &t);
    if (ret == ESP_OK) {
        ret = esp_timer_start_once(t, (uint64_t)CONFIG_MAGTAG_MAX_AWAKE_SEC * 1000000ULL);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "awake failsafe not armed: %s", esp_err_to_name(ret));
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

    setenv("TZ", MAGTAG_TZ, 1);
    tzset();

    /* Slot definitions live in rodata, not RTC memory — install them
       before the first timer_* call on every boot/wake. */
    timer_defs_install();

    /* Must run after TZ is set (date comparison) and before the wake
       handlers (whose rollover check would otherwise reset the timer). */
    try_restore_timer_snapshot(time(NULL));

    buttons_init();
    battery_init();
    audio_init();
    display_init();

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
