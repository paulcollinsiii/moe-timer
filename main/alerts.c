/* Audible alert engine, moved from main.c: one loop for every alert
   (expiry, break start, bed time, locate) — drain the press latch so
   only presses AFTER the alarm dismiss it, pulse the NeoPixels, run the
   audio pattern on its own short task, and poll for dismissal — latched
   taps of any length count, the level scan catches a button already held
   down through the drain. Alert-class, so it fires during quiet hours. */
#include "alerts.h"

#include <stddef.h>
#include <time.h>

#include "audio.h"
#include "buttons.h"
#include "neopixel.h"

#ifndef NATIVE
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h" /* the two alarm-length Kconfig ints below */
#else
/* The host build has no FreeRTOS and no sdkconfig.h. The three RTOS calls
   the alert loop makes are declared here and defined by the suite
   (test/test_alerts), which models them cooperatively; the two Kconfig
   ints arrive as -D from test/CMakeLists.txt.

   The log macros discard their varargs, so any function call made inside
   a log argument would be invisible to every host test. All six call
   sites in this file pass constants or already-computed values — keep it
   that way, or a side effect smuggled in as a %d argument would run on
   device and be unobservable here. */
typedef unsigned int TickType_t;
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
int xTaskCreate(TaskFunction_t fn, const char *name, unsigned int stack, void *arg, unsigned int prio,
                TaskHandle_t *created);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t task);
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#endif

static const char *TAG = "alerts";

typedef struct {
    uint8_t r, g, b;        /* alert pulse colour */
    void (*audio_fn)(void); /* blocking beep pattern, stop-flag aware */
    int max_poll_iters;     /* 100 ms each; cap slightly past the audio */
    const char *task_name;
    const char *dismiss_log;
} alert_pattern_t;

static volatile bool s_audio_done;
static volatile bool s_audio_task_exited;
static const alert_pattern_t *s_alert_active; /* set before the task spawns */

static void alert_audio_task(void *arg) {
    (void)arg;
    s_alert_active->audio_fn();
    s_audio_done = true;
    /* Exit ack LAST: once set, the DAC is free and run_alert may spawn
       the next audio task (locate loops alerts back-to-back). */
    s_audio_task_exited = true;
    vTaskDelete(NULL);
}

/* Returns true when a button dismissed the alert (vs. audio running out). */
static bool run_alert(const alert_pattern_t *p) {
    s_alert_active = p;
    s_audio_done = false;
    s_audio_task_exited = false;
    buttons_take_pressed();                       /* drain: a press from BEFORE the alarm must not pre-dismiss it */
    neopixel_alert_pulse_begin(p->r, p->g, p->b); /* task + teardown owned by the module */
    /* DAC write path is deeper than the old LEDC one. On create failure
       the alert runs visual-only: the poll loop still caps the pulse and
       a button still dismisses it. */
    if (xTaskCreate(alert_audio_task, p->task_name, 3072, NULL, 5, NULL) != pdPASS) {
        ESP_LOGW(TAG, "audio task create failed - visual-only alert");
        s_audio_task_exited = true; /* nothing to join below */
    }
    bool dismissed = false;
    for (int i = 0; i < p->max_poll_iters && !s_audio_done && !dismissed; i++) {
        dismissed = buttons_take_pressed() != 0 || buttons_scan_held() != 0;
        if (dismissed) {
            ESP_LOGI(TAG, "%s", p->dismiss_log);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    audio_stop();
    neopixel_alert_pulse_end();
    /* Join the audio task (bounded): a fixed grace raced the audio task's
       exit — locate loops back into run_alert immediately, and a stale
       task's s_audio_done = true would instantly end the NEXT alert while
       both tasks drive the DAC. The stop-flag-aware patterns exit within
       one beep segment; 2 s is generous. */
    for (int i = 0; i < 20 && !s_audio_task_exited; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!s_audio_task_exited) {
        ESP_LOGW(TAG, "audio task did not exit after stop");
    }
    return dismissed;
}

/* Expiry: red, self-terminates after the configured cycles (3 s each). */
static const alert_pattern_t ALERT_PAT_EXPIRY = {
    .r = 248,
    .g = 0,
    .b = 0,
    .audio_fn = audio_beep_sequence,
    .max_poll_iters = CONFIG_MAGTAG_EXPIRY_ALARM_CYCLES * 30 + 10,
    .task_name = "beep",
    .dismiss_log = "Alert dismissed by button",
};

/* Break start: cyan — matches the BREAK identity (~2.2 s per cycle). */
static const alert_pattern_t ALERT_PAT_BREAK = {
    .r = 0,
    .g = 150,
    .b = 220,
    .audio_fn = audio_break_alarm,
    .max_poll_iters = CONFIG_MAGTAG_BREAK_ALARM_CYCLES * 22 + 10,
    .task_name = "brk_alarm",
    .dismiss_log = "Break alarm silenced by button",
};

/* Locate: red, one beep sequence per run_alert call — looped by
   alert_run_locate until dismissed or timed out. Classic beeps at max
   volume regardless of the configured tone/volume: it exists to be
   found. */
static const alert_pattern_t ALERT_PAT_LOCATE = {
    .r = 248,
    .g = 0,
    .b = 0,
    .audio_fn = audio_locate_alarm,
    .max_poll_iters = 40,
    .task_name = "locate",
    .dismiss_log = "Locate dismissed by button",
};

/* Bed time: purple, break-alarm length; only fires when the crossing
   interrupts a RUNNING timer or an in-progress BREAK. A button press
   silences the audio - the lock itself has nothing to dismiss (buttons
   are not wake sources afterwards). */
static const alert_pattern_t ALERT_PAT_BEDTIME = {
    .r = 120,
    .g = 0,
    .b = 200,
    .audio_fn = audio_bedtime_alarm,
    .max_poll_iters = CONFIG_MAGTAG_BREAK_ALARM_CYCLES * 50 + 10,
    .task_name = "bed_alarm",
    .dismiss_log = "Bed time alarm silenced by button",
};

bool alert_run(alert_kind_t kind) {
    switch (kind) {
        case ALERT_BREAK:
            return run_alert(&ALERT_PAT_BREAK);
        case ALERT_BEDTIME:
            return run_alert(&ALERT_PAT_BEDTIME);
        case ALERT_EXPIRY:
        default:
            return run_alert(&ALERT_PAT_EXPIRY);
    }
}

#define LOCATE_MAX_SEC 600

/* Installed once at boot by main.c, which owns the esp_timer handle
   behind it. NULL until then, and NULL for good if the install is ever
   dropped — hence the guard and the warning below, which is the only
   trace a forgotten install leaves. */
static void (*s_extend_awake)(int seconds);

void alerts_set_extend_awake(void (*cb)(int seconds)) {
    s_extend_awake = cb;
}

void alert_run_locate(void) {
    ESP_LOGI(TAG, "Locate: alarming until dismissed (<= %d s)", LOCATE_MAX_SEC);
    /* Before the first pulse, not after: an alarm the failsafe has already
       cut short cannot be rescued by extending it afterwards. */
    if (s_extend_awake != NULL) {
        s_extend_awake(LOCATE_MAX_SEC + 60);
    } else {
        ESP_LOGW(TAG, "Locate: no awake extender installed - alarm may be cut short");
    }
    int64_t start = (int64_t)time(NULL);
    bool dismissed = false;
    while (!dismissed && (int64_t)time(NULL) - start < LOCATE_MAX_SEC) {
        dismissed = run_alert(&ALERT_PAT_LOCATE);
    }
    neopixel_stop();
    ESP_LOGI(TAG, "Locate: %s", dismissed ? "dismissed" : "timed out");
}
