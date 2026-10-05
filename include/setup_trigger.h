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

/* What kind of start this wake is, folded from the reset reason by
   wake_flow_reset_class(). Named here because it feeds `cold_boot` below. */
typedef enum {
    SETUP_TRIGGER_RESET_WAKE = 0, /* a deep-sleep wake */
    SETUP_TRIGGER_RESET_COLD,     /* a deliberate start: power-on, EN, software restart, USB */
    SETUP_TRIGGER_RESET_FAULT,    /* the previous run died: panic, watchdog, brownout, ... */
} setup_trigger_reset_class_t;

/* Everything the decision reads. `button_wake` and `cold_boot` are not a
   new taxonomy: they are the two wake-classification facts the codebase
   already computes elsewhere —
     button_wake  same cause wake_policy_render() takes (wake_policy.h):
                  true when this wake's cause includes EXT1 (any armed
                  pad, A-D or BOOT).
     cold_boot    wake_flow_reset_class() == SETUP_TRIGGER_RESET_COLD: a
                  deliberate start, told apart from a deep-sleep wake AND
                  from a crash (a fault reset is not a cold boot here).
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

   A completed BOOT hold (boot_hold_completed) wins outright, before
   either rule below runs and regardless of SSID state or wake cause: the
   plan's hold row is unconditional, so the explicit gesture can never be
   weaker than the automatic rule it sits above.

   No SSID, no completed hold: setup on a cold boot or any button wake
   (this is also the recovery path after an nvs_flash_erase() wipe, which
   leaves the device with no SSID too) — but NOT on a plain timer wake,
   so timers keep running offline and the no-SSID network path keeps
   failing cleanly (wifi_session.c:75 returns ESP_ERR_INVALID_STATE).

   SSID present, no completed hold: normal, always. An empty MQTT URI and
   repeated WiFi join failures are deliberately not inputs here — see
   setup_trigger_wifi_failing_hint() below for the latter. */
setup_trigger_mode_t setup_trigger_decide(const setup_trigger_in_t *in);

/* ---- BOOT hold tracker --------------------------------------------------- */

/* The gesture is the same wherever BOOT was first seen down: a BOOT wake
   starts the tracker, and so does an A-D press that wakes the device while
   BOOT is already held (wake_flow.c). Either way the hold is timed from the
   first sample after boot, not from the physical press. */

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
   tick-to-ms) that callers must keep monotonic. Elapsed time is computed
   with unsigned subtraction, so a genuine wraparound past UINT32_MAX ms
   (~49.7 days of uptime) self-corrects rather than reporting a huge
   hold; a signed clamp on top of that treats a `now` that is earlier
   than the hold's start for any OTHER reason (two clocks mixed, a
   non-monotonic source) as zero elapsed rather than an instant arm —
   see the clamp's comment in setup_trigger.c for why a real wrap and an
   implausible backward jump can be told apart at all.

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

/* The status line itself. Shared by the caller that sets it and the render
   test that draws it, so the wording that is checked is the wording that
   ships. It replaces the Last-sync label and shares the header row with the
   date and time, which end near x=125 on a 296 px panel: at 12 pt that leaves
   room for about 18 characters. "WiFi failing: hold BOOT" (23) ran into the
   time and "WiFi failing - hold BOOT for setup" is longer still, so the line
   says what the owner can act on and drops the diagnosis.

   The advice has to be true for the build. Where BOOT can wake the device
   (CONFIG_MAGTAG_BOOT_WAKES) holding it is the whole gesture. Where it
   cannot, nothing is listening to BOOT until some other press has woken the
   device, so the owner holds BOOT first and then presses a button: the
   press wakes the device with BOOT already down, and the hold is timed from
   there. */
#define SETUP_TRIGGER_WIFI_FAILING_HINT_TEXT "No WiFi: hold BOOT"
#define SETUP_TRIGGER_WIFI_FAILING_HINT_TEXT_NO_BOOT_WAKE "BOOT+button: setup"

/* 1 when this build arms BOOT as a wake source. Host builds have no
   sdkconfig and read the symbol as undefined, which is the off case. */
#if CONFIG_MAGTAG_BOOT_WAKES
#define SETUP_TRIGGER_BOOT_WAKES_BUILD 1
#else
#define SETUP_TRIGGER_BOOT_WAKES_BUILD 0
#endif

/* The status line for a paint, or NULL for none. Shown only when the
   failures reach the threshold AND the device has an SSID: with no SSID any
   button press enters setup by itself, so advice about holding BOOT would
   be false. `boot_wakes` is a parameter so a host test can drive both
   builds; the caller passes SETUP_TRIGGER_BOOT_WAKES_BUILD. Pure. */
const char *setup_trigger_status_hint(uint32_t consecutive_join_failures, bool has_wifi_ssid, bool boot_wakes);

/* True once `consecutive_join_failures` reaches the threshold: the main
   header should carry SETUP_TRIGGER_WIFI_FAILING_HINT_TEXT. Pure function
   over a caller-supplied count. The count is timer_wifi_join_failures()
   (RTC, survives deep sleep), advanced by net_apply_finish() once per
   network window — not wifi_session.c's s_retry_num, which resets on every
   connect attempt, and not stats_json.h's ota->fails, which counts OTA
   retries, a different failure. */
bool setup_trigger_wifi_failing_hint(uint32_t consecutive_join_failures);

#ifdef __cplusplus
}
#endif
