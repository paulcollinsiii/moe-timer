/* Pure sleep-duration planner — no ESP dependencies; host-tested.

   Consolidates every "how long should the device sleep" rule:
   - all states align wakes to wall-clock minute boundaries so renders show
     clean times (the RUNNING countdown's seconds digit stays constant)
   - RUNNING wakes early when an NTP sync will be due, so the sync finishes
     before the :00 render
   - RUNNING/BREAK wakes land ~SLEEP_PLAN_EVENT_LEAD_SEC before their event
     (expiry / break end) so the awake-side watch loop takes over. */
#include "sleep_plan.h"

int32_t sleep_plan_seconds(const sleep_plan_in_t *in) {
    int32_t sleep_sec = 60 - in->sec_into_minute;
    if (sleep_sec < SLEEP_PLAN_MIN_SEC)
        sleep_sec += 60; /* boundary too close — take the following minute */

    if (in->state == TIMER_RUNNING && in->sync_due_by_next_wake)
        sleep_sec -= SLEEP_PLAN_SYNC_LEAD_SEC;

    if ((in->state == TIMER_RUNNING || in->state == TIMER_BREAK) && in->event_remaining_sec > 0) {
        int32_t to_event_wake = in->event_remaining_sec - SLEEP_PLAN_EVENT_LEAD_SEC;
        if (to_event_wake < sleep_sec)
            sleep_sec = to_event_wake;
    }

    if (sleep_sec < SLEEP_PLAN_MIN_SEC)
        sleep_sec = SLEEP_PLAN_MIN_SEC;
    return sleep_sec;
}
