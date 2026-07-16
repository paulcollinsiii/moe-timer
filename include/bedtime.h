/* Bed Time gate policy (pure C, host-tested). At/after a configured
 * evening time the orchestrator locks the device onto the Bed Time
 * screen until day rollover; this module holds the decisions, main.c
 * holds the plumbing. HHMM 0 disables the feature entirely (the value
 * is nullable — not every device belongs to a kid). */
#ifndef BEDTIME_H
#define BEDTIME_H

#include <stdbool.h>

#include "timer.h" /* timer_state_t */

/* Valid bedtime config: 0 (disabled) or an evening time 1800-2359.
   The floor guarantees a bad edit can't lock the device during the day —
   HA (picked up at the ~2 h bedtime wakes) is the only remote fix path. */
bool bedtime_hhmm_valid(int hhmm);

/* Config HHMM -> minutes-of-day for the checks below; -1 = disabled
   (HHMM 0 or invalid). */
int bedtime_minutes(int hhmm);

/* In the bedtime window? Never wraps midnight: validation pins bedtime
   to the evening, and the post-midnight wake runs day rollover first,
   which clears the lock. */
bool bedtime_active(int now_min, int bed_min);

/* Would a screen break started now still be running at bedtime? Then
   skip the break and go straight to Bed Time. */
bool bedtime_break_would_cross(int now_min, int dur_min, int bed_min);

/* Audible alert only when the crossing interrupts someone: a RUNNING
   timer or an in-progress BREAK. Idle/paused/expired engage silently. */
bool bedtime_should_alert(timer_state_t state);

#endif /* BEDTIME_H */
