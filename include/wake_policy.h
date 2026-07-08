#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "timer.h"

/* Pure wake-orchestration decisions extracted from main.c so the logic
   that historically carried the bugs (refresh choice, alert firing, sync
   cadence, display snapping) is host-tested. main.c stays the executor. */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WAKE_RENDER_PARTIAL = 0,  /* routine tick; display.c may promote to full */
    WAKE_RENDER_FULL,         /* state change or button wake */
    WAKE_RENDER_EXPIRY_ALERT, /* RUNNING->EXPIRED just happened: TIME'S UP + alarm */
} wake_render_t;

/* Decide the render/alert action for a wake, given the state before the
   wake's action ran and after. An already-EXPIRED timer never re-fires
   the alert (e.g. a swap landing on an expired slot). */
wake_render_t wake_policy_render(timer_state_t before, timer_state_t after, bool button_wake);

/* Absorb +-2 s of wake/render jitter so an on-grid countdown renders as a
   round minute (1:10:59 never shows). Values at/below watch_threshold_sec
   (the event-watch window renders exact seconds) and honest off-grid
   values pass through unchanged. */
int32_t wake_policy_snap_minute(int32_t sec, int32_t watch_threshold_sec);

/* NTP cadence: RUNNING follows the 10-min recheck flag
   (timer_needs_ntp_sync); clock-only states (IDLE/PAUSED/EXPIRED) re-sync
   on the slower idle interval so the minute header doesn't drift; BREAK
   never syncs (it sleeps through). */
bool wake_policy_sync_due(timer_state_t state, bool running_recheck_due, time_t now, time_t last_sync,
                          int32_t idle_interval_sec);

#ifdef __cplusplus
}
#endif
