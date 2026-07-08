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
   BEFORE timer_reset wipes the counters). */
void mqtt_ha_queue_summary(const char *date, int32_t screen_used_s, const uint16_t completions[TIMER_EXTRA_SLOTS]);

/* Connect, publish discovery (when the schema version changed), publish
   the stat snapshot + any queued summary, apply retained config/command,
   disconnect. Bounded ~10 s. */
void mqtt_ha_window(const stats_snapshot_t *snap);

/* True (once) if a locate command was applied this window — main.c runs
   the locate alarm after the window closes (audio/LEDs, WiFi down). */
bool mqtt_ha_locate_pending(void);

#ifdef __cplusplus
}
#endif
