#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "display.h"
#include "stats_json.h"

/* Assembly of the two read-only views main.c hands out — the
   display_state_t a render consumes and the stats_snapshot_t the HA
   session publishes. Pure over its inputs struct plus the host-testable
   modules (timer, schedule, nvs_config, battery curve/policy); the
   device reads (ADC, app descriptor, reset reason) are injected. */

typedef struct {
    int batt_mv;              /* battery_read_mv() */
    int light_mv;             /* stats only; display never reads light */
    bool charge_locked;       /* stats only */
    const char *fw_version;   /* stats AND the main screen's battery row */
    const char *reset_reason; /* stats only */
} app_state_in_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Build the render state. IDLE shows today's full EFFECTIVE allocation,
   not 0 (docs/behavior/timers_and_schedule.md, "Timer states") — a full bar only when nothing is adjusted or
   withheld, since the bar's denominator is the day's default and the
   chore gate clamps the free tranche (see remaining_sec in display.h);
   extra timers use their fixed configured duration while Screen (slot 0)
   follows the day schedule. */
display_state_t app_state_display(const app_state_in_t *in, int32_t remaining, time_t now);

/* Side-effect-free stat snapshot for the HA session (never ticks the
   state machine). Per-slot remaining/limit ([0] = Screen): a started
   slot's allocation includes HA grants; IDLE falls back to the
   schedule/def value; disabled slots report 0/0. */
void app_state_stats(const app_state_in_t *in, time_t now, stats_snapshot_t *out);

#ifdef __cplusplus
}
#endif
