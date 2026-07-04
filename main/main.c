#include <stdlib.h>
#include <time.h>

#include "audio.h"
#include "buttons.h"
#include "display.h"
#include "esp_log.h"
#include "esp_sleep.h"
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

static void enter_deep_sleep(void) {
    buttons_configure_wakeup();
    esp_sleep_enable_timer_wakeup(WAKE_INTERVAL_US);
    ESP_LOGI(TAG, "Entering deep sleep");
    esp_deep_sleep_start();
}

/* WiFi lifecycle is entirely inside ntp_sync(): init->connect->sync->deinit */
static esp_err_t try_ntp_sync(void) {
    esp_err_t ret = ntp_sync();
    if (ret == ESP_OK) {
        s_last_ntp_sync = time(NULL);
        timer_record_ntp_sync(s_last_ntp_sync);
    } else {
        ESP_LOGW(TAG, "NTP sync failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static display_state_t make_state(int32_t remaining, time_t now) {
    day_type_t dt = schedule_get_day_type(now);
    return (display_state_t){
        .remaining_sec = remaining,
        .allocation_sec = schedule_get_allocation_sec(dt),
        .timer_state = timer_get_state(),
        .day_type = dt,
        .wall_time = now,
        .last_sync_time = s_last_ntp_sync,
    };
}

/* ---- expiry alert ---------------------------------------------------- */

static volatile bool s_audio_done;

static void neopixel_alert_task(void *arg) {
    (void)arg;
    neopixel_alert_start(); /* blocks until neopixel_stop() sets its flag */
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
    neopixel_stop();
    vTaskDelay(pdMS_TO_TICKS(100)); /* let alert tasks observe stop flags and exit */
}

/* ---- day rollover ----------------------------------------------------- */

static void handle_day_rollover(time_t *now) {
    if (!timer_is_new_day(*now))
        return;
    ESP_LOGI(TAG, "Day rollover");
    /* Fail-open: reset to IDLE with today's allocation even if sync fails */
    try_ntp_sync();
    *now = time(NULL);
    timer_reset();
    timer_record_date(*now);
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
        display_timesup();
        run_expiry_alert();
    } else if (timer_get_state() != before) {
        display_full_refresh(&st);
    } else {
        display_update(&st); /* partial; policy promotes every 5th to full */
    }
    enter_deep_sleep();
}

static void handle_button_wake(void) {
    time_t now = time(NULL);
    handle_day_rollover(&now);
    button_id_t btn = buttons_get_wakeup_button();
    timer_state_t before = timer_get_state();

    switch (btn) {
        case BTN_A:
            if (before == TIMER_RUNNING) {
                timer_pause(now);
            } else if (before == TIMER_IDLE) {
                /* NTP sync is mandatory before first start */
                if (try_ntp_sync() != ESP_OK) {
                    display_sync_failed();
                    enter_deep_sleep(); /* does not return */
                }
                now = time(NULL);
                day_type_t dt = schedule_get_day_type(now);
                timer_start(now, (int32_t)schedule_get_allocation_sec(dt));
            } else if (before == TIMER_PAUSED) {
                /* Best-effort sync; drift self-corrects on next success */
                try_ntp_sync();
                now = time(NULL);
                timer_resume(now);
            }
            break;
        case BTN_B:
            timer_reset();
            timer_record_date(now);
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
        display_timesup();
        run_expiry_alert();
    } else {
        display_full_refresh(&st); /* button wakes always full-refresh */
    }
    enter_deep_sleep();
}

void app_main(void) {
    /* MUST be first peripheral call: GPIO 21 power gate HIGH (NeoPixels off) */
    neopixel_init();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(nvs_config_init_defaults());

    setenv("TZ", MAGTAG_TZ, 1);
    tzset();

    buttons_init();
    audio_init();
    display_init();

    uint32_t causes = esp_sleep_get_wakeup_causes();
    ESP_LOGI(TAG, "Wakeup causes: 0x%08lx", (unsigned long)causes);

    if (causes & BIT(ESP_SLEEP_WAKEUP_EXT1)) {
        handle_button_wake();
    } else {
        handle_timer_tick(); /* RTC timer wake AND cold boot */
    }
}
