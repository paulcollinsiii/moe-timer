#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
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

/* Wait (bounded) for NTP-settled; on success the sync is recorded via
   timer_record_ntp_sync. true = sync succeeded (clock_step valid). */
bool net_window_wait_ntp(void);

/* Hand the stats snapshot to the window task. Called exactly once per
   window, AFTER the wake's paint (the task blocks on this rendezvous
   before opening the MQTT session, keeping display refresh current and
   radio TX bursts apart). No-op when no window is open. */
void net_window_post_snapshot(const stats_snapshot_t *snap);

/* Join the window task; poll_cb (may be NULL) runs every 100 ms while
   waiting — the orchestrator keeps Button A live during the MQTT tail.
   false = the task is wedged past timeout_ms: it stays marked active and
   the awake failsafe is the backstop. Never pass a poll_cb from the
   failsafe's esp_timer context. */
bool net_window_join(int timeout_ms, void (*poll_cb)(void));

bool net_window_active(void);
esp_err_t net_window_ntp_result(void);
/* Measured mono-vs-wall clock step; valid when the sync succeeded. */
int64_t net_window_clock_step(void);

#ifdef __cplusplus
}
#endif
