#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "audio.h"
#include "battery.h"
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
#include "schedule.h"
#include "timer.h"

static const char *TAG = "main";

/* Compile-time timezone (ProductOverview section 1) */
#define MAGTAG_TZ "EST5EDT,M3.2.0,M11.1.0"
#define WAKE_INTERVAL_US (55ULL * 1000000ULL)

static RTC_DATA_ATTR time_t s_last_ntp_sync;

/* Held-through-sleep guard: EXT1 ANY_LOW is level-triggered, so a button
   still held when the release-wait in enter_deep_sleep() times out (3 s)
   re-wakes the chip instantly and would re-fire its action. Record what
   was held at sleep entry; an immediate re-wake by one of those buttons
   is a continuation to ignore, not a new press. */
static RTC_DATA_ATTR uint8_t s_held_mask_at_sleep;
static RTC_DATA_ATTR int64_t s_sleep_entry_time;

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

    buttons_configure_wakeup();
    esp_sleep_enable_timer_wakeup(WAKE_INTERVAL_US);
    ESP_LOGI(TAG, "Entering deep sleep");
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
    neopixel_set_pixel(NP_WIFI_PIXEL, 0, 0, 20); /* blue: sync in progress */
#endif
    esp_err_t ret = ntp_sync();
    if (ret == ESP_OK) {
        s_last_ntp_sync = time(NULL);
        timer_record_ntp_sync(s_last_ntp_sync);
    } else {
        ESP_LOGW(TAG, "NTP sync failed: %s", esp_err_to_name(ret));
#if CONFIG_MAGTAG_SYNC_LED_FEEDBACK
        for (int i = 0; i < 3; i++) {
            neopixel_set_pixel(NP_WIFI_PIXEL, 30, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(150));
            neopixel_set_pixel(NP_WIFI_PIXEL, 0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(150));
        }
#endif
    }
#if CONFIG_MAGTAG_SYNC_LED_FEEDBACK
    /* Clear only the WiFi pixel — the state pixel stays lit through the
       e-ink refresh; enter_deep_sleep() guarantees the gate goes HIGH. */
    neopixel_set_pixel(NP_WIFI_PIXEL, 0, 0, 0);
#endif
    return ret;
}

/* Traffic-light state feedback while the slow e-ink refresh runs:
   RUNNING = green, PAUSED = amber, EXPIRED = red, IDLE = white. */
static void neopixel_show_timer_state(void) {
    switch (timer_get_state()) {
        case TIMER_RUNNING:
            neopixel_set_pixel(NP_STATE_PIXEL, 0, 20, 0);
            break;
        case TIMER_PAUSED:
            neopixel_set_pixel(NP_STATE_PIXEL, 25, 15, 0);
            break;
        case TIMER_EXPIRED:
            neopixel_set_pixel(NP_STATE_PIXEL, 25, 0, 0);
            break;
        default:
            neopixel_set_pixel(NP_STATE_PIXEL, 10, 10, 10);
            break;
    }
}

static display_state_t make_state(int32_t remaining, time_t now) {
    day_type_t dt = schedule_get_day_type(now);
    uint32_t alloc = schedule_get_allocation_sec(dt);
    /* IDLE shows today's full allocation (full bar), not 0 (ProductOverview) */
    if (timer_get_state() == TIMER_IDLE) {
        remaining = (int32_t)alloc;
    }
    int mv = battery_read_mv();
    int pct = battery_percent_from_mv(mv);
    ESP_LOGI(TAG, "battery: %d mV (%d%%)", mv, pct);
    return (display_state_t){
        .remaining_sec = remaining,
        .allocation_sec = alloc,
        .timer_state = timer_get_state(),
        .day_type = dt,
        .wall_time = now,
        .last_sync_time = s_last_ntp_sync,
        .battery_pct = (uint8_t)pct,
    };
}

/* ---- expiry alert ---------------------------------------------------- */

static volatile bool s_audio_done;
static volatile bool s_np_alert_done;

static void neopixel_alert_task(void *arg) {
    (void)arg;
    neopixel_alert_start(); /* loops until the stop flag; does its own final flush */
    s_np_alert_done = true;
    vTaskDelete(NULL);
}

static void audio_alert_task(void *arg) {
    (void)arg;
    audio_beep_sequence(); /* self-terminates after 5 cycles (~15 s) */
    s_audio_done = true;
    vTaskDelete(NULL);
}

static void run_expiry_alert(void) {
    s_audio_done = false;
    s_np_alert_done = false;
    xTaskCreate(neopixel_alert_task, "np_alert", 2048, NULL, 5, NULL);
    xTaskCreate(audio_alert_task, "beep", 2048, NULL, 5, NULL);

    /* Poll for dismissal; cap slightly past the 15 s sequence */
    bool dismissed = false;
    for (int i = 0; i < 160 && !s_audio_done && !dismissed; i++) {
        for (int b = 0; b < 4; b++) {
            if (buttons_is_pressed((button_id_t)b)) {
                ESP_LOGI(TAG, "Alert dismissed by button");
                dismissed = true;
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    audio_stop();
    /* Flag only — calling neopixel_stop() here raced the alert task's own
       flush_pixels(): two tasks on one RMT channel wedged
       rmt_tx_wait_all_done(portMAX_DELAY) forever (device stuck awake on
       the TIME'S UP screen, buttons dead; found in hardware smoke test). */
    neopixel_request_stop();
    for (int i = 0; i < 40 && !s_np_alert_done; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!s_np_alert_done) {
        ESP_LOGW(TAG, "np_alert task did not finish; forcing LED off");
    }
    neopixel_stop();                /* single-task now — idempotent gate-off */
    vTaskDelay(pdMS_TO_TICKS(100)); /* let the audio task observe its stop flag and exit */
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

/* ---- final-minute watch ------------------------------------------------ */

/* With <=60 s left, a 55 s sleep can overshoot expiry by most of a minute.
   Stay awake instead: lock the clock with one sync, keep the state pixel
   lit, and fire TIME'S UP within a tick of wall time. Runs at most once
   per day (right before expiry), so the battery cost is negligible. */
#define FINAL_MINUTE_SEC 60

static void maybe_wait_for_expiry(void) {
    if (timer_get_state() != TIMER_RUNNING)
        return;
    time_t now = time(NULL);
    int64_t remaining = g_rtc_state.expiry_wall_time - (int64_t)now;
    if (remaining <= 0 || remaining > FINAL_MINUTE_SEC)
        return;

    ESP_LOGI(TAG, "Final minute: staying awake (%lld s remaining)", (long long)remaining);
    /* Expiry is a wall time, so a clock step here directly sharpens the
       moment the alert fires. Skip when recently synced (drift over the
       10-min window is sub-second) or when the sync itself (~5-9 s)
       would blow past the expiry. */
    if (timer_needs_ntp_sync(now) && remaining > 15) {
        try_ntp_sync();
    }
    neopixel_show_timer_state();

    while (g_rtc_state.expiry_wall_time - (int64_t)time(NULL) > 0) {
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    timer_tick(time(NULL)); /* RUNNING -> EXPIRED */
    fire_expiry_alert();
}

/* ---- wake handlers ----------------------------------------------------- */

static void handle_timer_tick(void) {
    time_t now = time(NULL);
    handle_day_rollover(&now);

    if (timer_get_state() == TIMER_RUNNING && timer_needs_ntp_sync(now)) {
        try_ntp_sync();
        now = time(NULL);
    }

    timer_state_t before = timer_get_state();
    int32_t remaining = timer_tick(now);
    display_state_t st = make_state(remaining, now);

    if (timer_get_state() == TIMER_EXPIRED && before != TIMER_EXPIRED) {
        fire_expiry_alert();
    } else if (timer_get_state() != before) {
        display_full_refresh(&st);
    } else {
        display_update(&st); /* partial; policy promotes every 5th to full */
    }
    maybe_wait_for_expiry();
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
            if (before == TIMER_RUNNING) {
                timer_pause(now);
            } else if (before == TIMER_IDLE || before == TIMER_PAUSED) {
                /* Start/resume immediately — waiting on NTP first confused
                   users. Sync runs after; any clock step is applied to the
                   expiry via timer_shift_expiry (measured against the
                   monotonic clock, which NTP cannot step). */
                if (before == TIMER_IDLE) {
                    day_type_t dt = schedule_get_day_type(now);
                    timer_start(now, (int32_t)schedule_get_allocation_sec(dt));
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
#if CONFIG_MAGTAG_PARENT_TESTING
            timer_reset();
            timer_record_date(now);
#else
            /* Production: allocation resets only on day rollover */
            ESP_LOGI(TAG, "Button B reset disabled (MAGTAG_PARENT_TESTING=n)");
#endif
            break;
        case BTN_D:
            try_ntp_sync();
            now = time(NULL);
            break;
        case BTN_C: /* unbound in v1 */
        case BTN_NONE:
        default:
            break;
    }

    int32_t remaining = timer_tick(now);
    display_state_t st = make_state(remaining, now);

    if (timer_get_state() == TIMER_EXPIRED && before != TIMER_EXPIRED) {
        fire_expiry_alert(); /* alert owns the NeoPixels (red pulse) */
    } else {
        /* Includes EXPIRED: any button returns the display to the main
           layout (empty bar, TIME'S UP state) via a full refresh. */
        ESP_LOGI(TAG, "button %d: state %d -> %d, full refresh", (int)btn, (int)before, (int)timer_get_state());
        neopixel_show_timer_state(); /* resulting state, shown during refresh */
        display_full_refresh(&st);   /* button wakes always full-refresh */
        neopixel_stop();
    }
    maybe_wait_for_expiry();
    enter_deep_sleep();
}

void app_main(void) {
    /* MUST be first peripheral call: GPIO 21 power gate HIGH (NeoPixels off) */
    neopixel_init();

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

    if (causes & BIT(ESP_SLEEP_WAKEUP_EXT1)) {
        handle_button_wake();
    } else {
        handle_timer_tick(); /* RTC timer wake AND cold boot */
    }
}
