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
   the alert.

   `before` must be the state that was actually PAINTED — the break-screen
   boundary check depends on it, so callers must not overwrite it.
   break_ended = a background Screen Break ended on this wake: the panel
   changed (chip gone, possibly a snap back to Screen) even when before ==
   after, so force a full refresh.
   selection_changed = Button C swapped the active slot this wake, so an
   EXPIRED `after` is a different timer's old news, not a transition —
   this is how expiry suppression is expressed WITHOUT clobbering
   `before`. The expiry alert still outranks break_ended. */
wake_render_t wake_policy_render(timer_state_t before, timer_state_t after, bool button_wake, bool break_ended,
                                 bool selection_changed);

/* Break-over chime policy: fire only when nothing else is RUNNING (rule
   4 — the kid mid-activity gets that timer's own alert) and the
   transition is being acted on within BREAK_CHIME_GRACE_SEC of its wall
   end (rule 6 — a charge lock, bed-time lock or power cycle can span it,
   and the chime is an "it just happened" signal, not a replay). The snap
   back to Screen rides the same answer. */
bool wake_policy_break_chime(bool extra_running, int32_t overdue_sec);

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

/* Render-grid residue: seconds to wait so the render lands on the state's
   grid — RUNNING/BREAK on the countdown's round minute (the display truly
   reads 1:11:00), clock-only states on the wall :00. 0 = render in place:
   already on grid, the residue exceeds max_wait_sec (legitimately
   off-grid wakes render where they are and self-correct next cycle), or
   the event already passed. */
int32_t wake_policy_grid_wait_sec(timer_state_t state, int32_t event_remaining_sec, int sec_into_minute,
                                  int32_t max_wait_sec);

/* Final-minute countdown: partial display steps at the quarter-minute
   marks, values pinned so the text reads exactly 00:01:00/45/30/15. */
#define WAKE_COUNTDOWN_STEPS 4
/* Pinned display value of step idx; 0 out of range. */
int32_t wake_policy_countdown_step(int idx);
/* First step not yet passed at remaining_sec (a late wake skips passed
   marks); WAKE_COUNTDOWN_STEPS when only the LED countdown remains. */
int wake_policy_first_countdown_step(int32_t remaining_sec);

#ifdef __cplusplus
}
#endif
