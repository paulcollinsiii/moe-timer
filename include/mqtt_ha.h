#pragma once
#include <stdint.h>

#include "stats_json.h"

/* Home Assistant MQTT session — rides an already-open wifi_session
   window, entirely best-effort: any failure is logged and never affects
   the sync that opened the window. Empty broker URI in NVS = disabled. */

#ifdef __cplusplus
extern "C" {
#endif

/* Queue yesterday's summary for the next window (captured at day rollover
   BEFORE timer_reset wipes the counters and the chore acks).
   chores_done/chores: the day's acked chores and the configured count,
   or chores = STATS_JSON_CHORES_UNKNOWN when the list could not be read
   (both fields are then left out of the summary); see
   stats_json_summary(). */
void mqtt_ha_queue_summary(const char *date, int32_t screen_used_s, const uint16_t completions[TIMER_EXTRA_SLOTS],
                           uint8_t chores_done, int chores);

/* Connect, publish discovery (when the schema version changed), publish
   the stat snapshot + any queued summary, apply retained config/command,
   disconnect. Bounded ~10 s. */
void mqtt_ha_window(const stats_snapshot_t *snap);

/* True (once) if a locate command was applied this window — main.c runs
   the locate alarm after the window closes (audio/LEDs, WiFi down). */
bool mqtt_ha_locate_pending(void);

/* Timer effects parsed during the window, buffered for the orchestrator:
   the window runs on the network task, which never mutates timer state.
   Each returns true (once) and fills the out params when one is pending. */
bool mqtt_ha_take_bonus_target(int32_t *target_sec); /* set/screen_bonus */
bool mqtt_ha_take_grant(int *slot, int32_t *sec);    /* cmd grant */

/* At day rollover, clear the retained "Screen bonus today" target next
   window so the bonus doesn't repeat on the new day. */
void mqtt_ha_queue_bonus_clear(void);

#ifdef __cplusplus
}
#endif
