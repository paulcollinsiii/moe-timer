/* Pure sleep-duration planner — no ESP dependencies; host-tested.

   Consolidates every "how long should the device sleep" rule:
   - clock-only states (IDLE/PAUSED/EXPIRED) align wakes to WALL-CLOCK
     minute boundaries so the header time flips with real clocks
   - RUNNING/BREAK align to the COUNTDOWN's minute grid — wake when the
     remaining value hits a round minute, so the display truly reads
     1:11:00 (a fresh start/resume shows one precise value, then the next
     wake lands on the grid)
   - RUNNING wakes SLEEP_PLAN_SYNC_LEAD_SEC early when an NTP sync will be
     due, so the sync finishes before the grid render
   - RUNNING/BREAK wakes land ~SLEEP_PLAN_EVENT_LEAD_SEC before their event
     (expiry / break end) so the awake-side watch loop takes over
   - a Screen Break running BEHIND another selected timer is a secondary
     event with the same lead, so its chime lands on time too */
#include "sleep_plan.h"

int32_t sleep_plan_seconds(const sleep_plan_in_t *in) {
    int32_t sleep_sec;

    if (in->state == TIMER_RUNNING || in->state == TIMER_BREAK) {
        /* Countdown grid: seconds until event_remaining is a round minute */
        sleep_sec = in->event_remaining_sec % 60;
        if (sleep_sec == 0)
            sleep_sec = 60;
        if (in->state == TIMER_RUNNING && in->sync_due_by_next_wake)
            sleep_sec -= SLEEP_PLAN_SYNC_LEAD_SEC;
        if (sleep_sec < SLEEP_PLAN_MIN_SEC)
            sleep_sec += 60; /* grid point too close — take the previous grid minute */

        /* Event handoff: land ~LEAD before expiry/break end for the watch */
        if (in->event_remaining_sec > 0) {
            int32_t to_event_wake = in->event_remaining_sec - SLEEP_PLAN_EVENT_LEAD_SEC;
            if (to_event_wake < sleep_sec)
                sleep_sec = to_event_wake;
        }
    } else {
        /* Wall grid for the clock-only states */
        sleep_sec = 60 - in->sec_into_minute;
        if (sleep_sec < SLEEP_PLAN_MIN_SEC)
            sleep_sec += 60;
    }

    /* Secondary event: a break running on slot 0 behind the selected
       timer, whose end will chime. Same lead as the primary — the awake
       watch takes over from there. Only ever pulls the wake IN. */
    if (in->break_remaining_sec > 0) {
        int32_t to_break_wake = in->break_remaining_sec - SLEEP_PLAN_EVENT_LEAD_SEC;
        if (to_break_wake < sleep_sec)
            sleep_sec = to_break_wake;
    }

    if (sleep_sec < SLEEP_PLAN_MIN_SEC)
        sleep_sec = SLEEP_PLAN_MIN_SEC;
    return sleep_sec;
}
