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
    ALERT_BEDTIME,    /* purple; bed time engage while RUNNING or mid-break */
} alert_kind_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Returns true when a button dismissed the alert (vs. audio running out). */
bool alert_run(alert_kind_t kind);

/* Install the awake-failsafe extension the locate alarm needs. The
   failsafe is an esp_timer handle owned by main.c with no module home, so
   the engine is handed a way to push it out rather than the handle
   itself. The real deadline is the first network window, since that is
   where net_apply dispatches .on_locate; main.c installs earlier, next to
   net_apply_init(), because that is the line where locate first becomes
   reachable at all and it is the easiest place to notice a missing
   install. NULL (also the state before the first install) is legal and
   means "no extension available": the alarm still runs, but the failsafe
   may cut it short. */
void alerts_set_extend_awake(void (*cb)(int seconds));

/* "Help, I lost the timer": beep + red pulse until a button press or
   ~10 min. Runs after WiFi is down (audio/LEDs need the radio quiet).
   Pushes the awake failsafe out first, via the callback above. */
void alert_run_locate(void);

#ifdef __cplusplus
}
#endif
