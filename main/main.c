#include <stdlib.h>
#include <time.h>

#include "alerts.h"
#include "app_state.h"
#include "audio.h"
#include "battery.h"
#include "buttons.h"
#include "config_cache.h"
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
#include "lock_gate.h"
#include "neopixel.h"
#include "net_apply.h"
#include "net_window.h"
#include "nvs_config.h"
#include "nvs_flash.h"
#include "sleep_plan.h"
#include "stats_json.h"
#include "status_led.h"
#include "timer.h"
#include "timer_persist.h"
#include "wake_flow.h"

static const char *TAG = "main";

/* Timezone default lives in nvs_defaults.h (NVS_DEFAULT_TZ); the active TZ
   comes from NVS at boot so HA can change it (ProductOverview section 1). */
/* Kconfig bool as a C expression (defined as 1 when =y, absent when =n) */
#if CONFIG_MAGTAG_PARENT_TESTING
#define PARENT_TESTING true
#else
#define PARENT_TESTING false
#endif

/* Adapts the wake-scoped quiet-hours cache to neopixel.c's bool(void)
   callback ABI, which has nowhere to take the clock from. */
static bool status_leds_quiet(void) {
    return config_cache_quiet_active(time(NULL));
}

/* Declared in lock_gate.h: the gates there end a wake by calling this,
   and it stays here because deep sleep is an ESP-IDF contract with no
   module home. Does not return. */
void enter_deep_sleep(wake_sleep_mode_t mode) {
    /* Late-wake forensics repeat: the boot-time log of this line is often
       lost to USB CDC re-enumeration; by sleep entry the console has had
       the whole wake to come up. */
    ESP_LOGI(TAG, "this boot: reset %s", wake_flow_reset_reason_str(esp_reset_reason()));
    /* Never sleep with the network task alive: it holds WiFi and may be
       mid-publish. Normal paths finished the window already (no-op here);
       this covers cut-short paths. Bounded — on the failsafe path the
       network task may BE the wedge, and deep sleep then powers the radio
       down regardless. No pause polling: this can run in esp_timer
       context. */
    net_window_join(15000, NULL);
    net_window_log_last(); /* timing repeat: the boot-time line is often lost to CDC */
    timer_persist_save();
    /* EXT1 ANY_LOW is level-triggered: a still-held button would re-wake
       instantly and re-fire its action. Wait (bounded) for release. */
    for (int i = 0; i < 30 && buttons_scan_held() != 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* Snapshot still-held buttons for the continuation guard (the guard
       itself, and the RTC memory behind it, live in wake_flow). Must read
       while the pads are still digital: buttons_configure_wakeup_if() at
       the end of this function is what hands them to the RTC mux, and
       gpio_get_level is unreliable afterwards. */
    wake_flow_note_sleep_entry();

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

    /* Safety net: a latched break end that nothing drained is lost here.
       wake_flow's pre-sleep event watch drains on every path that does
       timer work, so this should be unreachable — log it rather than
       chime, since
       this also runs from the failsafe's esp_timer context where audio
       is not safe. A line here means a new code path skipped the drain. */
    int32_t undrained = 0;
    if (timer_break_take_ended(time(NULL), &undrained)) {
        ESP_LOGW(TAG, "break end reached sleep undrained (%ld s late)", (long)undrained);
    }

    /* Last NVS write (snapshot) is behind us on every path below; release
       the wake-scoped handle. */
    hal_nvs_close();

    /* All sleep-duration policy lives in the pure, host-tested planner
       (sleep_plan.c): minute-boundary alignment for clean renders, the
       NTP early-wake lead, the expiry/break-end event lead, and which of
       the readings below each state actually uses. Alignment precision is
       bounded by the S2's RC-oscillator sleep drift — the periodic NTP
       sync keeps it honest.
       Every reading is taken unconditionally: all seven are side-effect
       free getters, so gathering them costs nothing and keeps the choice
       of which ones matter on the tested side of the seam. */
    time_t plan_now = time(NULL);
    sleep_plan_timer_in_t plan_readings = {
        .state = timer_get_state(),
        .now = plan_now,
        .expiry_wall = timer_expiry_wall(),
        .ntp_recheck_due = timer_needs_ntp_sync(plan_now + SLEEP_PLAN_SYNC_LOOKAHEAD_SEC),
        .break_active = timer_break_active(),
        .break_remaining_sec = timer_break_remaining(plan_now),
        .extra_running = timer_any_extra_running(),
    };
    sleep_plan_in_t plan_in = sleep_plan_from_timer(&plan_readings);
    /* The mode picks between the planner and a lock's fixed interval, and
       carries the button decision with it — the readings above are pure
       getters, so gathering them on a locked wake costs nothing and keeps
       this path straight. */
    sleep_outcome_t out = sleep_plan_outcome(mode, &plan_in);
    buttons_configure_wakeup_if(out.enable_buttons);
    esp_sleep_enable_timer_wakeup((uint64_t)out.seconds * 1000000ULL);
    ESP_LOGI(TAG, "Entering deep sleep (%s%lu s)", out.reason, (unsigned long)out.seconds);
    esp_deep_sleep_start();
}

/* Declared in wake_flow.h: the stats-assembly seam. Side-effect-free (no
   timer_tick — a read here must never transition the state machine).
   Assembly rules live in app_state.c (host-tested); only the device reads
   are here, which is what keeps it in main.c. */
void stats_collect(stats_snapshot_t *out) {
    app_state_in_t in = {
        .batt_mv = battery_read_mv(),
        .light_mv = light_read_mv(),
        .charge_locked = lock_gate_charge_locked(),
        .parent_testing = PARENT_TESTING,
        .fw_version = esp_app_get_description()->version,
        .reset_reason = wake_flow_reset_reason_str(esp_reset_reason()),
    };
    app_state_stats(&in, time(NULL), out);
}

/* ---- network window (WiFi → NTP → snapshot rendezvous → MQTT) ----------
   Mechanics (task, completion signals) live in net_window.c; the
   orchestration (pre-window def capture, post-join reconcile/apply) in
   net_apply.c. main.c only supplies the device effects below. */

/* Join poll: Button A stays live while the MQTT tail drains — the screen
   is already painted and a dropped press would read as broken. Never
   passed from the failsafe's esp_timer context. */
static void poll_button_a_cb(void) {
    (void)wake_flow_poll_button_a_action();
}

static const net_apply_ops_t NET_APPLY_OPS = {
    .join_poll = poll_button_a_cb,
    .on_config_applied = config_cache_invalidate,
    .on_active_reset_chirp = audio_break_over_chime,
    .on_active_expired_alert = wake_flow_fire_expiry_alert,
    .post_stats = wake_flow_post_stats_snapshot,
    .on_locate = alert_run_locate,
};

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

/* Declared in wake_flow.h: the state-assembly seam. The final-minute
   watch assembles two renders of its own — a partial at each countdown
   mark and a full refresh with the LED lit between the assembly and the
   flush — and wrapping either as a paint seam would put that flow back in
   main.c. So the seam is cut at the only part that cannot move (the
   battery ADC read and the compile-time ParentTesting flag above) and
   this is the branch-free thunk over it. */
display_state_t make_display_state(int32_t remaining, time_t now) {
    return make_state(remaining, now);
}

/* Declared in wake_flow.h: the break-end owner there repaints through
   this after draining an edge, and it stays here because the state it
   paints needs the battery ADC read and the compile-time ParentTesting
   flag above. */
void paint_current_state_full(void) {
    time_t now = time(NULL);
    display_state_t st = make_state(timer_tick(now), now);
    display_full_refresh(&st);
}

/* Declared in wake_flow.h: the break gate there paints a freshly started
   break through this, for the same reason as the seam above — plus the
   LED between the assembly and the refresh, which is what holds the panel
   blue for the whole multi-second flush. */
void paint_break_started(time_t now) {
    display_state_t st = make_state(timer_tick(now), now);
    status_led_show_timer_state(); /* blue during the refresh */
    display_full_refresh(&st);     /* inverted SCREEN BREAK layout */
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
    enter_deep_sleep(lock_gate_sleep_mode());
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
    timer_persist_try_restore(time(NULL));

    /* Paired with the line below: net_apply_init is what makes .on_locate
       dispatchable, so this is where a missing install would bite. Order
       against arm_awake_failsafe is free — the extender null-guards. */
    alerts_set_extend_awake(extend_awake_failsafe);
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
    lock_gate_check_charge();

    /* Both handlers live in wake_flow.c and neither returns. This branch
       is the wake-cause decode itself — the hardware fact esp_sleep.h
       reports — and nothing else about the wake is decided here. */
    if (causes & BIT(ESP_SLEEP_WAKEUP_EXT1)) {
        wake_flow_handle_button_wake();
    } else {
        wake_flow_handle_timer_tick(); /* RTC timer wake AND cold boot */
    }
}
