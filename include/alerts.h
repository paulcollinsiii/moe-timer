#pragma once
#include <stdbool.h>

/* Audible alert engine: one engine for every alarm (expiry, break start,
   bed time, locate) — drains the press latch so only presses AFTER the
   alarm dismiss it, pulses the NeoPixels, runs the audio pattern on its
   own short task, and polls for dismissal. Alert-class, so it fires
   during quiet hours. Pattern details (colour, audio, length) live in
   alerts.c. */

typedef enum {
    ALERT_EXPIRY = 0, /* red; self-terminates after the configured cycles */
    ALERT_BREAK,      /* cyan; break start — matches the BREAK identity */
    ALERT_BEDTIME,    /* purple; bed time engage while RUNNING/BREAK */
} alert_kind_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Returns true when a button dismissed the alert (vs. audio running out). */
bool alert_run(alert_kind_t kind);

/* "Help, I lost the timer": beep + red pulse until a button press or
   ~10 min. Runs after WiFi is down (audio/LEDs need the radio quiet).
   `extend_awake` pushes the caller's awake failsafe past the alarm. */
void alert_run_locate(void (*extend_awake)(int seconds));

#ifdef __cplusplus
}
#endif
