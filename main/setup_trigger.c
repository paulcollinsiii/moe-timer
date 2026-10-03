/* Pure setup-mode decisions — no ESP dependencies; host-tested. The
   hardware (GPIO0, EXT1, the BOOT level sampler) lives in buttons.c, same
   split as wake_policy.c / sleep_plan.c. */
#include "setup_trigger.h"

setup_trigger_mode_t setup_trigger_decide(const setup_trigger_in_t *in) {
    if (!in->has_wifi_ssid) {
        /* Cold boot and a button wake both recover into setup — the
           button-wake leg is also what an nvs_flash_erase() wipe falls
           back to, since that leaves the device with no SSID too and no
           dedicated trigger of its own. A timer wake stays NORMAL: timers
           must keep running offline, and wifi_session.c already fails a
           no-SSID join cleanly. */
        return (in->cold_boot || in->button_wake) ? SETUP_TRIGGER_MODE_SETUP : SETUP_TRIGGER_MODE_NORMAL;
    }
    /* SSID present: only a completed BOOT hold asks for setup. Neither an
       empty MQTT URI nor a string of WiFi join failures is read here —
       the former is a supported WiFi-only state, the latter is the status
       hint below instead of an automatic trigger (a router outage must
       not turn every wake into a SoftAP session). */
    return in->boot_hold_completed ? SETUP_TRIGGER_MODE_SETUP : SETUP_TRIGGER_MODE_NORMAL;
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
            /* Unsigned subtraction: a wrap past UINT32_MAX ms (~49.7 days
               of uptime) still reads as the true elapsed time, not a huge
               or negative one. */
            if ((uint32_t)(now_ms - t->press_start_ms) >= SETUP_TRIGGER_BOOT_HOLD_MS) {
                t->state = SETUP_TRIGGER_BOOT_HOLD_ARMED;
                return SETUP_TRIGGER_BOOT_HOLD_EVENT_ARMED;
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
