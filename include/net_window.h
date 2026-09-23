#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifndef NATIVE
#include "esp_err.h"
#endif
#include "stats_json.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bounded network window — one per sync: WiFi + SNTP + MQTT/HA session on
   a dedicated task, radio off outside the window. Two-signal design lets
   interactive wakes paint as soon as NTP settles while MQTT keeps
   draining behind the panel. The task owns the radio and NOTHING else —
   it never mutates timer state, never paints, never touches LEDs beyond
   the WiFi status pixel (main-task side). Results come back through the
   getters below plus the mqtt_ha take-accessors, all consumed by the
   orchestrator (main.c). */

#define NET_NTP_SETTLE_TIMEOUT_MS 35000 /* WiFi assoc (15 s) + SNTP (15 s) + margin */
#define NET_JOIN_TIMEOUT_MS 90000       /* + MQTT (~10 s) + teardown; awake failsafe backstop */

/* Spawn the window task. false = fail-open, no window this wake (callers
   paint with the uncorrected clock, exactly like a WiFi failure). */
bool net_window_spawn(void);

/* Stand the WiFi status pixel down for the rest of this wake: something
   else has claimed the whole strip and this module's four writes to it
   would corrupt what that owner painted.

   THE CLAIMANT TODAY IS THE CHORE CHECKLIST (design §2.5), and the
   corruption is not cosmetic. This module's pixel is index 3, which is
   the checklist's GATE under status_led.c's mapping — it was chore slot
   2's row until M2-HW2 inverted the strip on 2026-09-22, and the
   inversion moved the gate ONTO this pixel. The triple written on a
   successful sync is (0, 20, 0) — byte for byte the checklist's "done"
   green — and on the gate that green means the day is RELEASED and the
   withheld time granted, not merely that one row is ticked. A sync
   landing while the checklist is up therefore paints a perfectly
   convincing release that nobody earned, with nothing on the screen to
   contradict it. The failure triple (30, 0, 0) is a near-match for the
   checklist's red, which on the gate reads as "still locked" — wrong
   whenever the day has in fact been released. The dark write in
   net_window_join() reads on that screen as "nothing is withheld".

   SUPPRESSED AT THE SOURCE rather than repainted afterwards, because the
   false green is written by net_window_wait_ntp() and the caller then
   spends ~1.9 s on the panel partial before it could repaint anything —
   which is exactly the stretch the user is looking at the pixels.

   ONE-WAY and wake-scoped: there is no release. The claim lasts until the
   next boot, because the strip the claimant painted stays lit until
   enter_deep_sleep()'s neopixel_stop_sync(), and a mid-wake release would
   hand the pixel back while the claimant's colours were still standing.
   Idempotent, so a caller need not track whether it has already asked.

   Timer modes DO NOT call this: there the sync pixel is how the user
   knows the device is awake and working, and nothing else claims the
   strip. Nor does an unattended wake in chore mode — row C17 keeps the
   chore pixels dark there, so this module's pixel is the only feedback
   there is and it keeps it. The key is "the strip is lit by someone
   else", not "the device is in chore mode"; only the caller knows that,
   which is why this is a call and not a mode test in here. */
void net_window_claim_leds(void);

/* Wait (bounded) for NTP-settled; on success the sync is recorded via
   timer_record_ntp_sync. true = sync succeeded (clock_step valid). */
bool net_window_wait_ntp(void);

/* Hand the stats snapshot to the window task. Called exactly once per
   window, AFTER the wake's paint (the task blocks on this rendezvous
   before opening the MQTT session, keeping display refresh current and
   radio TX bursts apart). No-op when no window is open. */
void net_window_post_snapshot(const stats_snapshot_t *snap);

/* Join the window task; poll_cb (may be NULL) runs every 100 ms while
   waiting — the orchestrator keeps Button B live during the MQTT tail.
   false = the task is wedged past timeout_ms: it stays marked active and
   the awake failsafe is the backstop. Never pass a poll_cb from the
   failsafe's esp_timer context. */
bool net_window_join(int timeout_ms, void (*poll_cb)(void));

bool net_window_active(void);
esp_err_t net_window_ntp_result(void);
/* Repeat the last window's phase timing (no-op if none this boot) — called
   at sleep entry, where the USB CDC console has had the whole wake to come
   up (the boot-time line is often lost to re-enumeration). */
void net_window_log_last(void);
/* Take (consume-once) the measured mono-vs-wall clock step; valid when
   the sync succeeded. Subsequent calls return 0 until the next window
   measures a new step — the orchestrator applies it to the expiry exactly
   once, whether the sync settled before the paint or during the tail. */
int64_t net_window_take_clock_step(void);

#ifdef __cplusplus
}
#endif
