/* Network window mechanics: task lifecycle, the two completion signals,
   and the snapshot rendezvous. Moved verbatim from main.c — the
   orchestration that APPLIES window results (grants, bonus, def
   reconcile, locate) stays there. */
#include "net_window.h"

#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_ha.h"
#include "neopixel.h"
#include "ntp.h"
#include "ota_flow.h"
#include "panic_diag.h"
#include "sdkconfig.h"
#include "timer.h"
#include "wifi_session.h"

static const char *TAG = "net_window";

/* WiFi status pixel. Pixel 0 (timer state) belongs to status_led.c, which
   wake_flow.c drives — the two sit at opposite ends of the strip so both
   can be read at once. That holds in every mode BUT the chore checklist,
   which claims all four pixels and makes this one the GATE — M2-HW2's
   strip inversion (2026-09-22) moved the gate onto this very pixel; it
   was a chore row before (status_led.h). A network window opened on a
   button wake can therefore repaint the gate, and the NTP-success triple
   below is (0,20,0), byte-identical to the checklist's "done" green,
   which on the gate reads as THE DAY RELEASED — a convincing FALSE
   RELEASE. net_window_claim_leds() is the fix and its header carries the
   whole of the argument; every write below goes through wifi_pixel(). */
#define NP_WIFI_PIXEL 3

/* Set once by net_window_claim_leds(), never cleared: see the header for
   why there is no release. Plain bool with no barrier because both the
   write and every read below are on the MAIN task — the four call sites
   are net_window_spawn(), net_window_wait_ntp() and net_window_join(),
   none of which is net_window_task(). The window task does not touch the
   pixels at all (net_window.h's "the task owns the radio and NOTHING
   else"), which is what makes that true and is the reason to keep it
   true. */
static bool s_leds_claimed;

/* THE ONLY writer of NP_WIFI_PIXEL in this module. Four call sites
   collapsed into one so the claim cannot be honoured at three of them and
   missed at the fourth — which is the shape this bug would come back in.
   The CONFIG_MAGTAG_SYNC_LED_FEEDBACK fence moved in here with them, so
   adding a fifth write is adding a call to this and nothing else.
   Status class: quiet hours and brightness are handled inside
   neopixel.c. */
static void wifi_pixel(uint8_t r, uint8_t g, uint8_t b) {
#if CONFIG_MAGTAG_SYNC_LED_FEEDBACK
    if (s_leds_claimed)
        return;
    neopixel_status_pixel(NP_WIFI_PIXEL, r, g, b);
#else
    (void)r;
    (void)g;
    (void)b;
#endif
}

void net_window_claim_leds(void) {
    s_leds_claimed = true;
}

static SemaphoreHandle_t s_ntp_settled; /* (a) sync resolved — paint may go, MQTT still ahead */
static SemaphoreHandle_t s_window_done; /* (b) radio down, results buffered */
static QueueHandle_t s_snapshot_q;      /* orchestrator → task, one-deep, by value */
static esp_err_t s_ntp_result;
static int64_t s_clock_step; /* measured mono-vs-wall step; valid when the sync succeeded */
static bool s_active;
/* Last window's phase timing, repeated at sleep entry: the boot-time log
   line is often lost to USB CDC re-enumeration (field report), and this
   is the budget evidence the power tuning needs. */
static int64_t s_last_wifi_ms;
static int64_t s_last_sntp_ms;
static int64_t s_last_mqtt_ms;
static int64_t s_last_total_ms = -1;

static void net_window_task(void *arg) {
    (void)arg;
    /* Breadcrumbs for the whole window. These write the NET slot of the
       panic record, which is separate from the main task's slot
       precisely so that a panic in here is not misattributed to whatever
       the main task was doing (panic_diag.h). Every mark below also
       resamples this task's stack high-water mark into stack_net, so the
       published floor is the pessimistic one — after wifi, after SNTP,
       after the TLS session the update check opens. */
    (void)panic_diag_enter(PANIC_PHASE_NET);
    int64_t mono_before_us = esp_timer_get_time();
    time_t wall_before = time(NULL);
    /* Phase timing: real-world budget evidence for tightening the wifi/
       SNTP/MQTT ceilings (power tuning) — captured per window so field
       logs accumulate a distribution, not a guess. */
    int64_t wifi_ms = 0, sntp_ms = 0, mqtt_ms = 0;
    esp_err_t ret = wifi_session_begin();
    wifi_ms = (esp_timer_get_time() - mono_before_us) / 1000;
    bool wifi_up = (ret == ESP_OK);
    if (wifi_up) {
        int64_t t = esp_timer_get_time();
        ret = ntp_sync_in_session();
        sntp_ms = (esp_timer_get_time() - t) / 1000;
        if (ret == ESP_OK) {
            /* Step measured against the monotonic clock, which NTP cannot
               move — the orchestrator applies it via timer_shift_expiry. */
            int64_t elapsed_sec = (esp_timer_get_time() - mono_before_us) / 1000000;
            s_clock_step = (int64_t)time(NULL) - ((int64_t)wall_before + elapsed_sec);
        }
    }
    s_ntp_result = ret;
    xSemaphoreGive(s_ntp_settled);
    if (wifi_up) {
        stats_snapshot_t snap;
        /* Rendezvous, which doubles as power serialization: the
           orchestrator posts the snapshot only after the e-ink paint
           finished, so panel refresh current and WiFi TX bursts (plus the
           config NVS flash writes below) never coincide — the combination
           browned out the rail in on-device testing. Stats are still
           collected post clock-correction; exactly one post per window,
           so this receive cannot starve. The radio just idles associated
           while the panel refreshes. */
        if (xQueueReceive(s_snapshot_q, &snap, portMAX_DELAY) == pdTRUE) {
            /* The update check RIDES this window: one small HTTPS GET,
               placed here for two reasons and constrained by a third.

               AFTER the rendezvous, because the rendezvous is the point
               at which the panel is known to be idle — the same brownout
               condition the receive above exists for. A manifest GET is
               a TX burst like any other.

               BEFORE mqtt_ha_window, because the second window (the
               download) runs after MQTT has closed and cannot publish
               its own outcome; ota_result has to be in NVS before the
               payload is built or it waits a whole day. See the note in
               task 12 of docs/planning/ota.plan.md: this ordering is
               NECESSARY for a check result to reach the same window, but
               it is not on its own SUFFICIENT — `snap` was filled on the
               main task before the post above, so the publisher has to
               read ota_result from NVS rather than out of this snapshot.
               It does: mqtt_ha.c's publish_states() calls
               ota_flow_stat() on the line that builds the payload, and
               stats_snapshot_t deliberately carries no OTA fields at all
               so that the other arrangement cannot be written by
               accident.

               s_ntp_result rather than a wider "the clock looks set":
               ota_gate_in_t::time_valid means "NTP has set the clock
               this session", and TLS certificate validity is exactly
               what it is protecting.

               A no-op unless ota_flow_arm() armed this wake, so the
               ordinary window pays one comparison for it. */
            /* The two phases the OTA hypothesis turns on. OTA_CHECK is
               the manifest GET — DNS, the TLS handshake against the
               pinned root, and the JSON read — and it is the ONLY thing
               separating this device from the one on the same firmware
               that never panics (its OTA URL is http://, which
               config_is_ota_url rejects, so it never enters this call at
               all). If the breadcrumb comes back OTA_CHECK, that is the
               answer. */
            (void)panic_diag_enter(PANIC_PHASE_OTA_CHECK);
            ota_flow_check(s_ntp_result == ESP_OK);
            (void)panic_diag_enter(PANIC_PHASE_MQTT);
            int64_t t = esp_timer_get_time();
            mqtt_ha_window(&snap);
            mqtt_ms = (esp_timer_get_time() - t) / 1000;
        }
        wifi_session_end();
    }
    /* Window over: the net slot goes idle so a later panic on the main
       task is not reported as "...+MQTT". Placed after wifi_session_end()
       so the radio teardown is still covered, and outside the wifi_up
       branch so the no-wifi path clears it too. */
    panic_diag_exit(PANIC_PHASE_MQTT, PANIC_PHASE_NONE);
    s_last_wifi_ms = wifi_ms;
    s_last_sntp_ms = sntp_ms;
    s_last_mqtt_ms = mqtt_ms;
    s_last_total_ms = (esp_timer_get_time() - mono_before_us) / 1000;
    ESP_LOGI(TAG, "window: wifi %lld ms, sntp %lld ms, mqtt %lld ms, total %lld ms", (long long)wifi_ms,
             (long long)sntp_ms, (long long)mqtt_ms, (long long)s_last_total_ms);
    /* Stack sizing evidence (ESP-IDF watermark is in bytes). Read into a
       local rather than left in the log argument: an argument is not
       evaluated at a level where the statement is compiled out (HAZ-1), and
       a stack floor that silently stops being sampled when someone trims the
       log level is the worst way to lose this number. */
    const UBaseType_t stack_floor = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "net task stack floor: %u B free", (unsigned)stack_floor);
    xSemaphoreGive(s_window_done);
    vTaskDelete(NULL);
}

void net_window_log_last(void) {
    if (s_last_total_ms < 0)
        return; /* no window this boot */
    ESP_LOGI(TAG, "window (sleep-entry repeat): wifi %lld ms, sntp %lld ms, mqtt %lld ms, total %lld ms",
             (long long)s_last_wifi_ms, (long long)s_last_sntp_ms, (long long)s_last_mqtt_ms,
             (long long)s_last_total_ms);
}

bool net_window_spawn(void) {
    if (s_active) {
        ESP_LOGE(TAG, "network window already open");
        return false;
    }
    if (s_ntp_settled == NULL) {
        s_ntp_settled = xSemaphoreCreateBinary();
        s_window_done = xSemaphoreCreateBinary();
        s_snapshot_q = xQueueCreate(1, sizeof(stats_snapshot_t));
    }
    if (s_ntp_settled == NULL || s_window_done == NULL || s_snapshot_q == NULL) {
        return false;
    }
    /* Drain leftovers from a window a forced sleep cut short */
    xSemaphoreTake(s_ntp_settled, 0);
    xSemaphoreTake(s_window_done, 0);
    stats_snapshot_t stale;
    xQueueReceive(s_snapshot_q, &stale, 0);
    s_ntp_result = ESP_FAIL;
    s_clock_step = 0;

    wifi_pixel(0, 0, 20); /* blue: window open */
    if (xTaskCreate(net_window_task, "net_win", 10240, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "network task create failed - skipping window");
        wifi_pixel(0, 0, 0);
        return false;
    }
    s_active = true;
    return true;
}

bool net_window_wait_ntp(void) {
    if (!s_active)
        return false;
    if (xSemaphoreTake(s_ntp_settled, pdMS_TO_TICKS(NET_NTP_SETTLE_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "NTP settle wait timed out; painting with uncorrected clock");
        return false; /* fail-open; the join records a late sync */
    }
    bool ok = (s_ntp_result == ESP_OK);
    if (ok) {
        timer_record_ntp_sync(time(NULL));
    } else {
        ESP_LOGW(TAG, "NTP sync failed: %s", esp_err_to_name(s_ntp_result));
    }
    wifi_pixel(ok ? 0 : 30, ok ? 20 : 0, 0);
    return ok;
}

void net_window_post_snapshot(const stats_snapshot_t *snap) {
    if (!s_active)
        return;
    xQueueSend(s_snapshot_q, snap, 0); /* one-deep, drained at spawn: never full */
}

bool net_window_join(int timeout_ms, void (*poll_cb)(void)) {
    if (!s_active)
        return true;
    int waited = 0;
    while (xSemaphoreTake(s_window_done, pdMS_TO_TICKS(100)) != pdTRUE) {
        if (poll_cb != NULL) {
            poll_cb();
        }
        waited += 100;
        if (waited >= timeout_ms) {
            ESP_LOGE(TAG, "network task did not finish in %d ms", timeout_ms);
            return false;
        }
    }
    s_active = false;
    wifi_pixel(0, 0, 0);
    /* Sync landed after the paint's bounded wait gave up? Still record it. */
    if (xSemaphoreTake(s_ntp_settled, 0) == pdTRUE && s_ntp_result == ESP_OK) {
        timer_record_ntp_sync(time(NULL));
    }
    return true;
}

bool net_window_active(void) {
    return s_active;
}

esp_err_t net_window_ntp_result(void) {
    return s_ntp_result;
}

int64_t net_window_take_clock_step(void) {
    int64_t step = s_clock_step;
    s_clock_step = 0;
    return step;
}
