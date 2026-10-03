#pragma once
#include <stdbool.h>
#include <stdint.h>

/* WiFi + MQTT provisioning plan (docs/planning/20261003.wifi-provisioning.plan.md),
   task 2: the pure pieces that decide whether a wake enters setup mode, plus
   the BOOT hold gesture that can ask for it on demand. No ESP-IDF deps —
   host-tested; the hardware (GPIO0, EXT1) stays in buttons.c. */

#ifndef NATIVE
#include "sdkconfig.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- setup vs normal ---------------------------------------------------- */

typedef enum {
    SETUP_TRIGGER_MODE_NORMAL = 0, /* the ordinary wake */
    SETUP_TRIGGER_MODE_SETUP,      /* bring up the SoftAP + provisioning manager */
} setup_trigger_mode_t;

/* Everything the decision reads. `button_wake` and `cold_boot` are not a
   new taxonomy: they are the two wake-classification facts the codebase
   already computes elsewhere —
     button_wake  same cause wake_policy_render() takes (wake_policy.h):
                  true when this wake's cause includes EXT1 (any armed
                  pad, A-D or BOOT).
     cold_boot    esp_reset_reason() != ESP_RST_DEEPSLEEP, the test
                  wake_flow.c already runs to tell a fresh boot apart
                  from a periodic deep-sleep wake.
   Reusing them as plain bools here, rather than inventing a parallel
   wake_cause_t, is deliberate — see the design note in
   docs/planning/20261003.wifi-provisioning.plan.md task 2. */
typedef struct {
    bool has_wifi_ssid;       /* nvs_config_get_wifi_ssid() returned non-empty */
    bool button_wake;         /* this wake's cause includes EXT1 */
    bool cold_boot;           /* a fresh boot, not a deep-sleep wake */
    bool boot_hold_completed; /* the BOOT hold tracker reached ENTER_SETUP this wake */
} setup_trigger_in_t;

/* {normal, setup} from (has SSID, wake cause, BOOT hold). Pure — host-tested.

   No SSID: setup on a cold boot or any button wake (this is also the
   recovery path after an nvs_flash_erase() wipe, which leaves the device
   with no SSID too) — but NOT on a plain timer wake, so timers keep
   running offline and the no-SSID network path keeps failing cleanly
   (wifi_session.c:75 returns ESP_ERR_INVALID_STATE).

   SSID present: setup only once the BOOT hold tracker has reached
   ENTER_SETUP for THIS wake (a completed >= SETUP_TRIGGER_BOOT_HOLD_MS
   hold, released). An empty MQTT URI and repeated WiFi join failures are
   deliberately not inputs here — see setup_trigger_wifi_failing_hint()
   below for the latter. */
setup_trigger_mode_t setup_trigger_decide(const setup_trigger_in_t *in);

/* ---- BOOT hold tracker --------------------------------------------------- */

/* D5: 5 s, with on-panel confirmation at the threshold ("Release to enter
   setup"). Kconfig so a board can tune the gesture; host tests have no
   sdkconfig and use the same figure as the fallback (pattern:
   include/timer.h's NTP_SYNC_INTERVAL_SEC). */
#ifdef CONFIG_MAGTAG_BOOT_HOLD_MS
#define SETUP_TRIGGER_BOOT_HOLD_MS CONFIG_MAGTAG_BOOT_HOLD_MS
#else
#define SETUP_TRIGGER_BOOT_HOLD_MS 5000
#endif

typedef enum {
    SETUP_TRIGGER_BOOT_HOLD_IDLE = 0, /* not held */
    SETUP_TRIGGER_BOOT_HOLD_HOLDING,  /* held, below the threshold */
    SETUP_TRIGGER_BOOT_HOLD_ARMED,    /* held past the threshold: show "Release to enter setup" */
} setup_trigger_boot_hold_state_t;

typedef enum {
    SETUP_TRIGGER_BOOT_HOLD_EVENT_NONE = 0,
    SETUP_TRIGGER_BOOT_HOLD_EVENT_ARMED,       /* this sample just crossed the threshold */
    SETUP_TRIGGER_BOOT_HOLD_EVENT_ENTER_SETUP, /* released after ARMED: run the gesture */
    SETUP_TRIGGER_BOOT_HOLD_EVENT_CANCELLED,   /* released before the threshold: do nothing */
} setup_trigger_boot_hold_event_t;

/* Plain data, not opaque: the caller (the setup screen) reads `.state`
   directly to know whether to paint "Release to enter setup", the same
   way sleep_plan_in_t and buttons_policy_in_t are read elsewhere in this
   tree. Only setup_trigger_boot_hold_sample() below writes it. */
typedef struct {
    setup_trigger_boot_hold_state_t state;
    uint32_t press_start_ms; /* set on IDLE -> HOLDING; meaningless in IDLE */
} setup_trigger_boot_hold_t;

void setup_trigger_boot_hold_reset(setup_trigger_boot_hold_t *t);

/* Feed one (timestamp, BOOT down?) sample. `now_ms` is a free-running
   millisecond clock (esp_timer_get_time() / 1000, or xTaskGetTickCount()'s
   tick-to-ms); elapsed time is computed with unsigned subtraction, so a
   wraparound past UINT32_MAX ms (~49.7 days of uptime) self-corrects
   rather than reporting a huge or negative hold.

   No debounce inside the tracker: any single `down == false` sample while
   HOLDING or ARMED ends the hold (HOLDING -> CANCELLED, ARMED ->
   ENTER_SETUP). The caller's polling cadence is the only debounce, same as
   the rest of this button family (buttons.c:DEBOUNCE_US). */
setup_trigger_boot_hold_event_t setup_trigger_boot_hold_sample(setup_trigger_boot_hold_t *t, uint32_t now_ms,
                                                               bool down);

/* ---- "WiFi failing" status hint ----------------------------------------- */

/* Not swept — "more than a fluke, not yet a certainty". Repeated WiFi join
   failures are deliberately NOT a setup trigger (a router outage must not
   turn every wake into a SoftAP session and drain the battery); this is
   only the status-line hint that tells the owner BOOT is the way out. */
#define SETUP_TRIGGER_WIFI_FAIL_HINT_THRESHOLD 3

/* True once `consecutive_join_failures` reaches the threshold: the status
   line should read "WiFi failing — hold BOOT for setup". Pure function over
   a caller-supplied count — no persistent consecutive-join-failure counter
   exists yet anywhere in this tree (wifi_session.c's s_retry_num resets on
   every connect attempt and is not RTC-backed; stats_json.h's ota->fails
   counts OTA retries, a different failure). Whoever wires this up owns
   adding and threading that count; see docs/planning/20261003.wifi-
   provisioning.plan.md task 2's notes before adding an RTC field for it —
   that's a RTC_STATE_VERSION bump and this task deliberately defers it. */
bool setup_trigger_wifi_failing_hint(uint32_t consecutive_join_failures);

#ifdef __cplusplus
}
#endif
