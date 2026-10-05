/* Pure setup-mode decisions — no ESP dependencies; host-tested. The
   hardware (GPIO0, EXT1, the BOOT level sampler) lives in buttons.c, same
   split as wake_policy.c / sleep_plan.c. */
#include "setup_trigger.h"

#include <stddef.h>

setup_trigger_mode_t setup_trigger_decide(const setup_trigger_in_t *in) {
    /* The explicit gesture outranks every automatic rule below it,
       whatever the SSID state or wake cause: the plan's BOOT-hold row is
       unconditional ("held >= 5 s -> releasing enters setup"), and a
       completed hold on a no-SSID device must not be weaker than that
       same device's own cold-boot/button-wake rule. Checking
       has_wifi_ssid first would make a no-SSID timer wake with a
       completed hold read NORMAL while the identical hold on a
       provisioned device read SETUP. */
    if (in->boot_hold_completed)
        return SETUP_TRIGGER_MODE_SETUP;
    if (!in->has_wifi_ssid) {
        /* Cold boot and a button wake both recover into setup — the
           button-wake leg is also what an nvs_flash_erase() wipe falls
           back to, since that leaves the device with no SSID too and no
           dedicated trigger of its own. A timer wake stays NORMAL: timers
           must keep running offline, and wifi_session.c already fails a
           no-SSID join cleanly. */
        return (in->cold_boot || in->button_wake) ? SETUP_TRIGGER_MODE_SETUP : SETUP_TRIGGER_MODE_NORMAL;
    }
    /* SSID present and no completed hold: neither an empty MQTT URI nor a
       string of WiFi join failures is read here — the former is a
       supported WiFi-only state, the latter is the status hint below
       instead of an automatic trigger (a router outage must not turn
       every wake into a SoftAP session). */
    return SETUP_TRIGGER_MODE_NORMAL;
}

void setup_trigger_boot_hold_reset(setup_trigger_boot_hold_t *t) {
    t->state = SETUP_TRIGGER_BOOT_HOLD_IDLE;
    t->press_start_ms = 0;
}

setup_trigger_boot_hold_event_t setup_trigger_boot_hold_sample(setup_trigger_boot_hold_t *t, uint32_t now_ms,
                                                               bool down) {
    switch (t->state) {
        case SETUP_TRIGGER_BOOT_HOLD_IDLE:
            if (!down)
                return SETUP_TRIGGER_BOOT_HOLD_EVENT_NONE;
            t->state = SETUP_TRIGGER_BOOT_HOLD_HOLDING;
            t->press_start_ms = now_ms;
            return SETUP_TRIGGER_BOOT_HOLD_EVENT_NONE;

        case SETUP_TRIGGER_BOOT_HOLD_HOLDING:
            if (!down) {
                t->state = SETUP_TRIGGER_BOOT_HOLD_IDLE;
                return SETUP_TRIGGER_BOOT_HOLD_EVENT_CANCELLED;
            }
            /* Unsigned subtraction so a genuine wrap past UINT32_MAX ms
               (~49.7 days of uptime) still reads as the true elapsed
               time. The int32_t cast guards the OTHER direction: a `now`
               that is earlier than `press_start_ms` for a reason that is
               NOT a 32-bit wrap (two clocks mixed, a non-monotonic
               source) would otherwise read as a huge unsigned elapsed
               time and arm instantly. Every real hold threshold fits
               comfortably inside int32_t's positive half (the Kconfig
               range tops out at 15 s), so a genuine wrap still comes
               through here as a small positive number and arms exactly
               as before; only an implausible backward jump clamps to 0
               elapsed, i.e. "no progress yet" rather than "armed" — a
               one-sample hiccup that self-corrects on the next forward
               sample. Callers still owe this one monotonic clock
               (setup_trigger.h); this is a safety clamp, not licence to
               mix them. */
            {
                int32_t elapsed_signed = (int32_t)(now_ms - t->press_start_ms);
                uint32_t elapsed = elapsed_signed < 0 ? 0 : (uint32_t)elapsed_signed;
                if (elapsed >= SETUP_TRIGGER_BOOT_HOLD_MS) {
                    t->state = SETUP_TRIGGER_BOOT_HOLD_ARMED;
                    return SETUP_TRIGGER_BOOT_HOLD_EVENT_ARMED;
                }
            }
            return SETUP_TRIGGER_BOOT_HOLD_EVENT_NONE;

        case SETUP_TRIGGER_BOOT_HOLD_ARMED:
        default:
            if (!down) {
                t->state = SETUP_TRIGGER_BOOT_HOLD_IDLE;
                return SETUP_TRIGGER_BOOT_HOLD_EVENT_ENTER_SETUP;
            }
            return SETUP_TRIGGER_BOOT_HOLD_EVENT_NONE; /* already armed; no repeat event */
    }
}

bool setup_trigger_wifi_failing_hint(uint32_t consecutive_join_failures) {
    return consecutive_join_failures >= SETUP_TRIGGER_WIFI_FAIL_HINT_THRESHOLD;
}

const char *setup_trigger_status_hint(uint32_t consecutive_join_failures, bool has_wifi_ssid, bool boot_wakes) {
    if (!has_wifi_ssid || !setup_trigger_wifi_failing_hint(consecutive_join_failures))
        return NULL;
    return boot_wakes ? SETUP_TRIGGER_WIFI_FAILING_HINT_TEXT : SETUP_TRIGGER_WIFI_FAILING_HINT_TEXT_NO_BOOT_WAKE;
}
